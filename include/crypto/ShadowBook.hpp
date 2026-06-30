#pragma once
// Driver helpers: shell ibkrcrypto_bt for per-leg signal + equity (exact parity
// with refresh_shadow.py's subprocess orchestration), correlation-downsize, time.
#include <string>
#include <vector>
#include <map>
#include <set>
#include <cstdio>
#include <cmath>
#include <ctime>
#include <sstream>
#include <fstream>
#include "Roster.hpp"

namespace crypto {

inline std::string run_capture(const std::string& cmd) {
    std::string out;
    FILE* p = popen(cmd.c_str(), "r");
    if (!p) return out;
    char buf[4096]; size_t n;
    while ((n = fread(buf, 1, sizeof(buf), p)) > 0) out.append(buf, n);
    pclose(p);
    return out;
}

// SLIPBPS + (if funding csv exists) FUNDCSV/USE_REAL_FUND env prefix, mirroring _cost_env.
inline std::string cost_env_prefix(const std::string& sym) {
    std::string pre = "SLIPBPS=" + std::to_string(slip_bps(sym)) + " ";
    std::string fc = fund_dir() + "/" + sym + ".csv";
    std::ifstream test(fc);
    if (test.good()) pre += "FUNDCSV='" + fc + "' USE_REAL_FUND=1 ";
    return pre;
}

// parse `float(ln.split(tok)[1].split()[0])` style token
inline bool parse_after(const std::string& ln, const std::string& tok, double& out) {
    auto p = ln.find(tok);
    if (p == std::string::npos) return false;
    p += tok.size();
    std::string rest = ln.substr(p);
    std::stringstream ss(rest);
    std::string first; ss >> first;
    try { out = std::stod(first); return true; } catch (...) { return false; }
}

struct Sig { int t; double sz; double px; double expx; };

inline Sig run_signal(const std::string& sym, const std::string& csvf, int cost,
                      const std::string& strat, int rma) {
    std::string cmd = cost_env_prefix(sym)
        + "COSTBPS=" + std::to_string(cost) + " REGIME_MA=" + std::to_string(rma) + " "
        + "'" + bt_bin() + "' " + sym + " '" + csvf + "' --signal " + strat + " 2>/dev/null";
    std::string out = run_capture(cmd);
    std::stringstream ss(out); std::string ln;
    while (std::getline(ss, ln)) {
        if (ln.find("target=") != std::string::npos) {
            double t = 0, sz = 1.0, px = 0.0, ex = 0.0;
            parse_after(ln, "target=", t);
            parse_after(ln, "size=", sz);
            parse_after(ln, "px=", px);
            parse_after(ln, "exit=", ex); // 0 if absent
            return {(int)t, sz, px, ex};
        }
    }
    return {0, 1.0, 0.0, 0.0};
}

// trailing daily cum-equity from --equity: {day -> cum_eq}
inline std::map<long, double> eqret(const std::string& sym, const std::string& csvf,
                                    const std::string& strat) {
    std::string cmd = cost_env_prefix(sym)
        + "'" + bt_bin() + "' " + sym + " '" + csvf + "' --equity " + strat + " 2>/dev/null";
    std::string out = run_capture(cmd);
    std::map<long, double> d;
    std::stringstream ss(out); std::string ln;
    while (std::getline(ss, ln)) {
        if (ln.empty() || !std::isdigit((unsigned char)ln[0])) continue;
        auto comma = ln.find(',');
        if (comma == std::string::npos) continue;
        try {
            long ts = std::stol(ln.substr(0, comma));
            double eq = std::stod(ln.substr(comma + 1));
            d[ts / 86400000] = eq;
        } catch (...) {}
    }
    return d;
}

struct LegRef { std::string key, sym, csvf, strat; };

inline double pearson(const std::vector<double>& a, const std::vector<double>& b) {
    size_t m = a.size();
    if (m < 5) return 0.0;
    double ma = 0, mb = 0;
    for (size_t i = 0; i < m; ++i) { ma += a[i]; mb += b[i]; }
    ma /= m; mb /= m;
    double ca = 0, va = 0, vb = 0;
    for (size_t i = 0; i < m; ++i) {
        ca += (a[i] - ma) * (b[i] - mb);
        va += (a[i] - ma) * (a[i] - ma);
        vb += (b[i] - mb) * (b[i] - mb);
    }
    va = std::sqrt(va); vb = std::sqrt(vb);
    return (va > 0 && vb > 0) ? ca / (va * vb) : 0.0;
}

// {key -> (corr, size_scale)} — leave-one-out corr of each leg vs the rest of the book.
inline std::map<std::string, std::pair<double, double>> corr_downsize(const std::vector<LegRef>& legs) {
    int CORR_WIN = std::atoi(env_or("CORR_WIN", "30").c_str());
    double CORR_THR = std::stod(env_or("CORR_THR", "0.5"));
    double CORR_SCALE = std::stod(env_or("CORR_SCALE", "0.5"));
    std::map<std::string, std::pair<double, double>> out;
    if (CORR_THR <= 0) { for (auto& L : legs) out[L.key] = {0.0, 1.0}; return out; }

    std::map<std::string, std::map<long, double>> cur;
    for (auto& L : legs) cur[L.key] = eqret(L.sym, L.csvf, L.strat);

    std::set<long> dayset;
    for (auto& kv : cur) for (auto& d : kv.second) dayset.insert(d.first);
    if (dayset.empty()) dayset.insert(0);
    std::vector<long> days(dayset.begin(), dayset.end());
    if ((int)days.size() > CORR_WIN + 1)
        days.erase(days.begin(), days.end() - (CORR_WIN + 1));

    std::map<std::string, std::vector<double>> ret;
    for (auto& L : legs) {
        auto& ck = cur[L.key];
        bool has_prev = false; double prev = 0.0;
        std::vector<double> rs;
        for (long d : days) {
            double v;
            auto it = ck.find(d);
            if (it != ck.end()) v = it->second;
            else v = has_prev ? prev : 0.0;
            if (has_prev) rs.push_back(v - prev);
            prev = v; has_prev = true;
        }
        ret[L.key] = rs;
    }
    size_t n = (size_t)-1;
    for (auto& kv : ret) n = std::min(n, kv.second.size());
    if (n == (size_t)-1) n = 0;

    for (auto& L : legs) {
        std::vector<double> rest(n, 0.0);
        for (auto& O : legs)
            if (O.key != L.key)
                for (size_t i = 0; i < n; ++i) rest[i] += ret[O.key][i];
        std::vector<double> self(ret[L.key].begin(), ret[L.key].begin() + n);
        double c = pearson(self, rest);
        out[L.key] = {c, c > CORR_THR ? CORR_SCALE : 1.0};
    }
    return out;
}

// parse "YYYY-mm-dd HH:MM" as UTC -> unix seconds (mirrors _unix). 0 on failure.
inline long unix_from(const std::string& s) {
    std::tm tm{};
    if (!strptime(s.c_str(), "%Y-%m-%d %H:%M", &tm)) return 0;
    return (long)timegm(&tm);
}

inline std::string utc_now_min() {
    std::time_t t = std::time(nullptr);
    std::tm tm{}; gmtime_r(&t, &tm);
    char buf[32]; std::strftime(buf, sizeof(buf), "%Y-%m-%d %H:%M", &tm);
    return std::string(buf);
}

} // namespace crypto
