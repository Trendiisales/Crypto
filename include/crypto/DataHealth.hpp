#pragma once
// Port of integrity_gate.py (correctness) + the refresh_shadow.py freshness guard.
//  - integrity_check: OHLC valid, monotonic ts, >=60 bars, x1000-glitch reject.
//  - data_age_days / fresh_ok: a source is fresh only if BOTH its newest bar AND
//    the file mtime are within max_age_days of now. A stale leg is FROZEN upstream.
#include <string>
#include <vector>
#include <fstream>
#include <sstream>
#include <cmath>
#include <ctime>
#include <sys/stat.h>

namespace crypto {

// returns the list of integrity errors; empty == clean (gate passes).
inline std::vector<std::string> integrity_check(const std::string& path) {
    std::vector<std::string> errs;
    std::ifstream f(path);
    if (!f) { errs.push_back("[MISS]"); return errs; }
    struct Row { long ts; double o, h, l, c; };
    std::vector<Row> rows;
    std::string line;
    while (std::getline(f, line)) {
        if (line.empty() || !std::isdigit((unsigned char)line[0])) continue; // skip header/blank
        std::stringstream ss(line);
        std::string cell; std::vector<std::string> cols;
        while (std::getline(ss, cell, ',')) cols.push_back(cell);
        if (cols.size() < 5) continue;
        try {
            rows.push_back({std::stol(cols[0]), std::stod(cols[1]), std::stod(cols[2]),
                            std::stod(cols[3]), std::stod(cols[4])});
        } catch (...) { /* mirror Python's try/except pass */ }
    }
    if (rows.size() < 60) { errs.push_back("<60 bars"); return errs; }
    const double DAY = 86400000.0;
    bool have_prev = false; long prev_ts = 0; double prev_c = 0; int gaps = 0;
    for (size_t i = 0; i < rows.size(); ++i) {
        const auto& r = rows[i];
        double mn = std::min(std::min(r.o, r.h), std::min(r.l, r.c));
        if (mn <= 0) errs.push_back("row" + std::to_string(i) + " non-positive px");
        if (r.h < r.l || r.h < r.o || r.h < r.c || r.l > r.o || r.l > r.c)
            errs.push_back("row" + std::to_string(i) + " OHLC violation");
        if (have_prev) {
            if (r.ts <= prev_ts) errs.push_back("row" + std::to_string(i) + " non-monotonic/dupe ts");
            double d = (r.ts - prev_ts) / DAY;
            if (d > 3.0) gaps++;
        }
        if (have_prev && prev_c > 0) {
            double jump = std::fabs(r.c - prev_c) / prev_c;
            // Threshold 3.0 (300%) mirrors integrity_gate.py: a x1000/x100/x10 glitch is
            // 900%..99900% single-bar, so 300% nails every order-of-magnitude corruption
            // WITHOUT false-rejecting genuine crypto 1h pumps (e.g. DOGE +70% Jan-2021 mania,
            // the exact wide up-jumps UpJump trades). Was 0.5 -> wrongly rejected DOGE 1h.
            if (jump > 3.0) errs.push_back("row" + std::to_string(i) + " x-glitch jump (poss x1000)");
        }
        prev_ts = r.ts; prev_c = r.c; have_prev = true;
        if (errs.size() > 8) break;
    }
    if (gaps > (int)(rows.size() * 0.05)) errs.push_back(std::to_string(gaps) + " gaps >3d");
    return errs;
}

inline bool integrity_gate(const std::string& path) { return integrity_check(path).empty(); }

// last data-row close of a CSV (col 5), or -1 on miss/parse-fail.
inline double last_close_of(const std::string& path) {
    std::ifstream f(path);
    if (!f) return -1.0;
    std::string line, last;
    while (std::getline(f, line))
        if (!line.empty() && std::isdigit((unsigned char)line[0])) last = line;
    if (last.empty()) return -1.0;
    std::vector<std::string> c; std::stringstream ss(last); std::string cell;
    while (std::getline(ss, cell, ',')) c.push_back(cell);
    if (c.size() < 5) return -1.0;
    try { return std::stod(c[4]); } catch (...) { return -1.0; }
}

// Cross-feed VALUE sanity: a leg's daily latest close must agree with the same
// symbol's intraday (4h) latest close within tol. integrity_check only tests a
// file against ITSELF (per-bar glitch), so it MISSES a corrupt/rescaled daily feed
// that is internally smooth but disagrees with the live intraday tape. Canonical
// failure: BTCUSDT_1d.csv overwritten ~5.4x on 2026-07-04 (climbed 30k->335k with
// no single-bar >300% jump -> passed integrity) while 4h stayed ~62.5k; shadow_refresh
// entered a phantom LONG @335720 vs real 62583 -> -81% -> desk health RED.
// Returns true (pass) when it cannot compare (no _1d leg / missing 4h / bad values)
// so it never false-freezes a leg it has no evidence against.
inline bool cross_feed_sane(const std::string& daily_path, double tol = 0.50) {
    auto pos = daily_path.rfind("_1d.csv");
    if (pos == std::string::npos) return true;            // not a daily leg -> skip
    std::string intr = daily_path.substr(0, pos) + "_4h.csv";
    double dc = last_close_of(daily_path), ic = last_close_of(intr);
    if (dc <= 0 || ic <= 0) return true;                  // can't compare -> don't freeze
    return std::fabs(dc - ic) / ic <= tol;
}

inline double max_age_for(const std::string& sym) { return sym == "NDX" ? 4.0 : 2.5; }

// (bar_age_days, file_age_days): age of newest bar + file mtime, in days. inf on error.
struct DataAge { double bar; double file; };
inline DataAge data_age_days(const std::string& path) {
    const double INF = std::numeric_limits<double>::infinity();
    double now = (double)std::time(nullptr);
    struct stat st;
    if (stat(path.c_str(), &st) != 0) return {INF, INF};
    double fage = (now - (double)st.st_mtime) / 86400.0;
    // last data row's first column
    std::ifstream f(path);
    std::vector<std::string> lines; std::string line;
    while (std::getline(f, line)) lines.push_back(line);
    double last_ts = 0; bool found = false;
    for (auto it = lines.rbegin(); it != lines.rend(); ++it) {
        const std::string& ln = *it;
        if (ln.empty()) continue;
        char c0 = ln[0];
        if (std::isdigit((unsigned char)c0) || c0 == '-') {
            std::string first = ln.substr(0, ln.find(','));
            try {
                double v = std::stod(first);
                if (v > 1e12) v /= 1000.0; // ms epoch -> seconds
                last_ts = v; found = true; break;
            } catch (...) { found = false; }
        }
    }
    double bage = found ? (now - last_ts) / 86400.0 : INF;
    return {bage, fage};
}

struct Freshness { bool ok; double bar; double file; };
inline Freshness fresh_ok(const std::string& path, const std::string& sym) {
    DataAge a = data_age_days(path);
    double m = max_age_for(sym);
    return {a.bar <= m && a.file <= m, a.bar, a.file};
}

} // namespace crypto
