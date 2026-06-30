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
            if (jump > 0.5) errs.push_back("row" + std::to_string(i) + " x-glitch jump (poss x1000)");
        }
        prev_ts = r.ts; prev_c = r.c; have_prev = true;
        if (errs.size() > 8) break;
    }
    if (gaps > (int)(rows.size() * 0.05)) errs.push_back(std::to_string(gaps) + " gaps >3d");
    return errs;
}

inline bool integrity_gate(const std::string& path) { return integrity_check(path).empty(); }

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
