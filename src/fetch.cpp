// fetch.cpp (T11) — C++ replacement for the four Python Binance/NDX fetchers.
// Subcommands map 1:1 onto the cron's Python invocations so each cron line can be
// swapped independently at cutover (Phase 6):
//   fetch daily      <- fetch_crypto.py            (append newest CLOSED 1d klines)
//   fetch intraday   <- fetch_crypto_intraday.py   (paginated incremental 1h/4h)
//   fetch funding    <- fetch_funding.py           (full-history funding rewrite)
//   fetch ndx        <- fetch_ndx.py               (Yahoo ^NDX, freshness-gated atomic write)
// Append/format/exit-code semantics are matched to the Python so the parity gate
// (Checkpoint C: identical rows appended) holds.
#include "crypto/BinanceFetch.hpp"
#include "crypto/Roster.hpp"   // env_or, csv_dir, fund_dir, ndx_csv
#include <cstdio>
#include <cstring>
#include <string>
#include <vector>
#include <fstream>
#include <sstream>
#include <ctime>
#include <cmath>
#include <unistd.h>
#include <limits>
#include <climits>
#include <map>
#include <array>
#include <utility>

using namespace crypto;

// ---- small file helpers ----
static std::vector<std::string> read_lines(const std::string& path) {
    std::vector<std::string> v; std::ifstream f(path); std::string ln;
    while (std::getline(f, ln)) v.push_back(ln);
    return v;
}

// Newest first-column integer already in the CSV (skips header/blank), or -1.
static long long last_open_ms(const std::string& path) {
    auto lines = read_lines(path);
    for (auto it = lines.rbegin(); it != lines.rend(); ++it) {
        if (it->empty()) continue;
        std::string first = it->substr(0, it->find(','));
        bool digits = !first.empty();
        for (char c : first) if (!std::isdigit((unsigned char)c)) { digits = false; break; }
        if (digits) { try { return std::stoll(first); } catch (...) {} }
    }
    return -1;
}

static std::string gmt_date(long long ms) {
    time_t t = (time_t)(ms / 1000);
    struct tm g; gmtime_r(&t, &g);
    char b[16]; std::strftime(b, sizeof b, "%Y-%m-%d", &g);
    return b;
}
static std::string gmt_datetime(long long ms) {
    time_t t = (time_t)(ms / 1000);
    struct tm g; gmtime_r(&t, &g);
    char b[24]; std::strftime(b, sizeof b, "%Y-%m-%d %H:%M", &g);
    return b;
}
static std::string f8(double v) { char b[64]; std::snprintf(b, sizeof b, "%.8f", v); return b; }
static std::string f2(double v) { char b[64]; std::snprintf(b, sizeof b, "%.2f", v); return b; }

// ========================= daily (fetch_crypto.py) =========================
static int run_daily() {
    // Full CSM spot-sleeve universe (the Paxos-tradeable coins the CrossSectionalAllocator
    // ranks). Was BTC/ETH/SOL only -> the other 6 daily CSVs went stale (not refreshed),
    // so the live CSM sleeve ranked on stale momentum. All 14 now refresh daily.
    const std::vector<std::pair<std::string,std::string>> PAIRS{
        {"BTCUSDT","BTCUSDT_1d.csv"}, {"ETHUSDT","ETHUSDT_1d.csv"}, {"SOLUSDT","SOLUSDT_1d.csv"},
        {"ADAUSDT","ADAUSDT_1d.csv"}, {"BCHUSDT","BCHUSDT_1d.csv"}, {"DOGEUSDT","DOGEUSDT_1d.csv"},
        {"LINKUSDT","LINKUSDT_1d.csv"}, {"LTCUSDT","LTCUSDT_1d.csv"}, {"XRPUSDT","XRPUSDT_1d.csv"},
        {"AVAXUSDT","AVAXUSDT_1d.csv"}, {"UNIUSDT","UNIUSDT_1d.csv"}, {"NEARUSDT","NEARUSDT_1d.csv"},
        {"LDOUSDT","LDOUSDT_1d.csv"}, {"AAVEUSDT","AAVEUSDT_1d.csv"}};
    long long now_ms = (long long)std::time(nullptr) * 1000;
    int rc = 0;
    for (auto& [sym, fname] : PAIRS) {
        std::string path = csv_dir() + "/" + fname;
        try {
            long long last_ot = last_open_ms(path);
            std::vector<std::string> added;
            json ks = get_json(binance_klines_url(sym, "1d", 0, 30), 15);  // 30 bars: close multi-day gaps
            for (auto& k : ks) {
                long long ot = k[0].get<long long>(), ct = k[6].get<long long>();
                if (ot > last_ot && ct < now_ms) {        // newer AND completed only
                    added.push_back(std::to_string(ot) + "," +
                                    f8(std::stod(k[1].get<std::string>())) + "," +
                                    f8(std::stod(k[2].get<std::string>())) + "," +
                                    f8(std::stod(k[3].get<std::string>())) + "," +
                                    f8(std::stod(k[4].get<std::string>())));
                }
            }
            if (!added.empty()) {
                std::ofstream fh(path, std::ios::app);
                for (auto& ln : added) fh << ln << "\n";
            }
            long long newest = last_open_ms(path);
            std::printf("%s: +%zu bar(s), newest %s\n", sym.c_str(), added.size(),
                        newest >= 0 ? gmt_date(newest).c_str() : "?");
            std::fflush(stdout);
        } catch (const std::exception& e) {
            std::fprintf(stderr, "%s: FETCH FAILED -- %s\n", sym.c_str(), e.what());
            std::fflush(stderr);
            rc = 1;
        }
    }
    return rc;
}

// ===================== intraday (fetch_crypto_intraday.py) =====================
static json fetch_klines_paginated(const std::string& sym, const std::string& interval,
                                   long long start_ms) {
    const int LIMIT = 1000;
    json out = json::array();
    long long cur = start_ms;
    while (true) {
        json batch = get_json(binance_klines_url(sym, interval, cur, LIMIT), 30);
        if (batch.empty()) break;
        for (auto& b : batch) out.push_back(b);
        long long last_open = batch.back()[0].get<long long>();
        if ((int)batch.size() < LIMIT) break;
        cur = last_open + 1;
        usleep(250000);  // 0.25s — be polite to the weight limiter
    }
    return out;
}

static int run_intraday() {
    const std::vector<std::string> PAIRS{"BTCUSDT", "ETHUSDT", "SOLUSDT"};
    const std::vector<std::string> TFS{"1h", "4h"};
    const long long START_MS = 1609459200000LL;  // 2021-01-01 UTC
    for (auto& sym : PAIRS) {
        for (auto& tf : TFS) {
            std::string path = csv_dir() + "/" + sym + "_" + tf + ".csv";
            long long prev_last = last_open_ms(path);
            long long start = (prev_last >= 0) ? prev_last + 1 : START_MS;
            json rows;
            try {
                rows = fetch_klines_paginated(sym, tf, start);
            } catch (const std::exception& e) {
                std::fprintf(stderr, "%s %s: FETCH FAILED -- %s\n", sym.c_str(), tf.c_str(), e.what());
                continue;
            }
            if (rows.empty()) {
                if (prev_last < 0) std::fprintf(stderr, "%s %s: NO DATA\n", sym.c_str(), tf.c_str());
                continue;
            }
            if (prev_last >= 0) {                          // APPEND new bars (header present)
                std::ofstream f(path, std::ios::app);
                for (auto& k : rows) {
                    if (k[0].get<long long>() <= prev_last) continue;   // de-dupe boundary bar
                    f << k[0].get<long long>() << "," << k[1].get<std::string>() << ","
                      << k[2].get<std::string>() << "," << k[3].get<std::string>() << ","
                      << k[4].get<std::string>() << "\n";
                }
            } else {                                       // cold pull -> fresh file with header
                std::ofstream f(path);
                f << "open_time_ms,open,high,low,close\n";
                for (auto& k : rows) {
                    f << k[0].get<long long>() << "," << k[1].get<std::string>() << ","
                      << k[2].get<std::string>() << "," << k[3].get<std::string>() << ","
                      << k[4].get<std::string>() << "\n";
                }
            }
            long long last_ot = rows.back()[0].get<long long>();
            std::printf("%s %s: +%zu bars  -> %s\n", sym.c_str(), tf.c_str(),
                        rows.size(), gmt_datetime(last_ot).c_str());
        }
    }
    return 0;
}

// ======================= funding (fetch_funding.py) =======================
static int run_funding() {
    const std::vector<std::string> SYMS{"BTC", "ETH", "SOL"};
    const long long START = 1546300800000LL;  // 2019-01-01
    long long now = (long long)std::time(nullptr) * 1000;
    for (auto& s : SYMS) {
        json out = json::array();
        long long cur = START;
        while (cur < now) {
            json rows;
            try { rows = get_json(binance_funding_url(s, cur, 1000), 20); }
            catch (const std::exception& e) { std::fprintf(stderr, "%s: %s\n", s.c_str(), e.what()); break; }
            if (rows.empty()) break;
            for (auto& r : rows) out.push_back(r);
            long long last = rows.back()["fundingTime"].get<long long>();
            cur = last + 1;
            if ((int)rows.size() < 1000) break;
            usleep(150000);
        }
        if (out.empty()) { std::fprintf(stderr, "%s: no funding\n", s.c_str()); continue; }
        std::string path = fund_dir() + "/" + s + ".csv";
        std::ofstream fh(path);
        fh << "symbol,funding_time_ms,funding_rate,mark\n";
        for (auto& r : out) {
            std::string mark = "0";
            if (r.contains("markPrice") && r["markPrice"].is_string()) {
                std::string m = r["markPrice"].get<std::string>();
                if (!m.empty()) mark = m;
            }
            fh << s << "," << r["fundingTime"].get<long long>() << ","
               << r["fundingRate"].get<std::string>() << "," << mark << "\n";
        }
        std::printf("%s: %zu funding points -> %s\n", s.c_str(), out.size(), path.c_str());
        std::fflush(stdout);
    }
    return 0;
}

// ========================== ndx (fetch_ndx.py) ==========================
static int ndx_die(const std::string& msg, int code = 1) {
    std::printf("[fetch_ndx] %s\n", msg.c_str()); std::fflush(stdout);
    return code;
}

static int run_ndx() {
    const std::string NDX = ndx_csv();
    const long long SESSION_OFFSET = 48600;   // 13:30 UTC — matches existing bar stamping
    const double MAX_AGE_DAYS = 4.0;

    json doc;
    try { doc = get_json(yahoo_ndx_url(60), 30); }
    catch (const std::exception& e) { return ndx_die(std::string("yahoo download failed (") + e.what() + ") — keeping existing file"); }

    json result;
    try {
        result = doc["chart"]["result"][0];
        if (!result.contains("timestamp") || !result["indicators"]["quote"][0].contains("close"))
            return ndx_die("unexpected yahoo payload — keeping existing file");
    } catch (...) { return ndx_die("unexpected yahoo payload — keeping existing file"); }

    const json& ts = result["timestamp"];
    const json& q  = result["indicators"]["quote"][0];
    const json& O = q["open"]; const json& H = q["high"]; const json& L = q["low"]; const json& C = q["close"];

    // ts -> midnight-UTC epoch + SESSION_OFFSET (replicates fetch_ndx.py date stamping)
    std::vector<std::pair<long long, std::array<double,4>>> fresh;
    for (size_t i = 0; i < ts.size(); ++i) {
        if (O[i].is_null() || H[i].is_null() || L[i].is_null() || C[i].is_null()) continue;
        double ov, hv, lv, cv;
        try { ov = O[i].get<double>(); hv = H[i].get<double>(); lv = L[i].get<double>(); cv = C[i].get<double>(); }
        catch (...) { continue; }
        time_t t = (time_t)ts[i].get<long long>();
        struct tm g; gmtime_r(&t, &g);
        struct tm mid{}; mid.tm_year = g.tm_year; mid.tm_mon = g.tm_mon; mid.tm_mday = g.tm_mday;
        long long day0 = (long long)timegm(&mid);
        long long stamp = day0 + SESSION_OFFSET;
        if (cv > 0 && hv >= lv) fresh.push_back({stamp, {ov, hv, lv, cv}});  // positive close, sane OHLC
    }
    if (fresh.empty()) return ndx_die("no valid fresh bars parsed — keeping existing file");

    long long newest_ts = LLONG_MIN; for (auto& kv : fresh) newest_ts = std::max(newest_ts, kv.first);
    double age = (std::time(nullptr) - (double)newest_ts) / 86400.0;
    if (age > MAX_AGE_DAYS) {                  // FRESHNESS GATE: refuse to write a stale source
        char b[64]; std::snprintf(b, sizeof b, "%.1f", age);
        return ndx_die("yahoo newest ^NDX bar is " + gmt_date(newest_ts * 1000) + " (" + b +
                       "d old > 4.0d) — source stale, NOT overwriting");
    }

    // merge fresh into existing history (ts -> 4 string cols)
    std::map<long long, std::array<std::string,4>> rows;
    {
        std::ifstream fh(NDX); std::string ln;
        while (std::getline(fh, ln)) {
            std::stringstream ss(ln); std::string cell; std::vector<std::string> p;
            while (std::getline(ss, cell, ',')) p.push_back(cell);
            if (p.size() >= 5) {
                std::string k = p[0]; bool ok = !k.empty();
                size_t st = (k[0] == '-') ? 1 : 0;
                for (size_t i = st; i < k.size(); ++i) if (!std::isdigit((unsigned char)k[i])) { ok = false; break; }
                if (ok) { try { rows[std::stoll(p[0])] = {p[1], p[2], p[3], p[4]}; } catch (...) {} }
            }
        }
    }
    for (auto& kv : fresh)
        rows[kv.first] = {f2(kv.second[0]), f2(kv.second[1]), f2(kv.second[2]), f2(kv.second[3])};

    // atomic write: temp in same dir + rename
    std::string dir = NDX.substr(0, NDX.find_last_of('/'));
    std::string tmpl = dir + "/.ndx_fetch_XXXXXX";
    std::vector<char> tb(tmpl.begin(), tmpl.end()); tb.push_back('\0');
    int fd = mkstemp(tb.data());
    if (fd < 0) return ndx_die("mkstemp failed — original file untouched");
    {
        std::string tmp(tb.data());
        FILE* fp = fdopen(fd, "w");
        for (auto& kv : rows) {
            auto& r = kv.second;
            std::fprintf(fp, "%lld,%s,%s,%s,%s\n", kv.first, r[0].c_str(), r[1].c_str(), r[2].c_str(), r[3].c_str());
        }
        std::fclose(fp);
        if (std::rename(tmp.c_str(), NDX.c_str()) != 0) {
            std::remove(tmp.c_str());
            return ndx_die("rename failed — original file untouched");
        }
    }
    char b[64]; std::snprintf(b, sizeof b, "%.1f", age);
    std::printf("[fetch_ndx] OK — %zu bars, newest %s (age %sd), %zu fresh merged\n",
                rows.size(), gmt_date(newest_ts * 1000).c_str(), b, fresh.size());
    return 0;
}

// ================================ main ================================
int main(int argc, char** argv) {
    if (argc < 2) {
        std::fprintf(stderr, "usage: fetch {daily|intraday|funding|ndx}\n");
        return 2;
    }
    curl_global_init(CURL_GLOBAL_DEFAULT);
    std::string cmd = argv[1];
    int rc = 2;
    if      (cmd == "daily")    rc = run_daily();
    else if (cmd == "intraday") rc = run_intraday();
    else if (cmd == "funding")  rc = run_funding();
    else if (cmd == "ndx")      rc = run_ndx();
    else std::fprintf(stderr, "unknown subcommand: %s\n", cmd.c_str());
    curl_global_cleanup();
    return rc;
}
