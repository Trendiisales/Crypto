#pragma once
// Port of the ROSTER + cost/gate constants from refresh_shadow.py.
// Each leg = (key, sym, csv, costbps, strat, mult, display-sym, display-strat).
// CSV/dir locations resolve from env with live-layout defaults so the driver
// can be pointed at a copy of live data for the parity gate (T9).
#include <string>
#include <vector>
#include <set>
#include <map>
#include <cstdlib>

namespace crypto {

inline std::string env_or(const char* k, const std::string& d) {
    const char* v = std::getenv(k);
    return (v && *v) ? std::string(v) : d;
}

// Live-layout defaults (the validated ~/Crypto/backtest tree, consolidated from
// ~/IBKRCrypto 2026-07-01). Env vars keep their IBKRCRYPTO_ names for cron compat.
inline std::string csv_dir()  { return env_or("IBKRCRYPTO_CSVDIR",  "/Users/jo/Crypto/backtest/data"); }
inline std::string ndx_csv()  { return env_or("IBKRCRYPTO_NDXCSV",  "/Users/jo/Tick/NDX_daily_2016_2026.csv"); }
inline std::string fund_dir() { return env_or("IBKRCRYPTO_FUNDDIR", "/Users/jo/Crypto/backtest/data/funding"); }
inline std::string bt_bin()   { return env_or("IBKRCRYPTO_BT",      "/Users/jo/Crypto/backtest/ibkrcrypto_bt"); }
inline std::string data_dir() { return env_or("IBKRCRYPTO_DATADIR", "/Users/jo/Crypto/backtest/data/ibkrcrypto"); }

struct Leg {
    std::string key, sym, csvf;
    int         cost;        // COSTBPS
    std::string strat;
    double      mult;
    std::string dsym, dstrat;
};

// REGIME GATE (S-2026-06-30): per-symbol close>SMA(REGIME_GATE_MA) on the daily
// TREND legs only. REGIME_GATE_MA=0 disables.
inline int regime_gate_ma() { return std::atoi(env_or("REGIME_GATE_MA", "200").c_str()); }
inline const std::set<std::string>& gated_strats() {
    static const std::set<std::string> g{"EMAx", "TSMom50", "Roc"};
    return g;
}

// live-fidelity cost overlays
inline int slip_bps(const std::string& sym) {
    static const std::map<std::string, int> s{{"BTC", 3}, {"ETH", 3}, {"SOL", 5}, {"NDX", 1}};
    auto it = s.find(sym);
    return it == s.end() ? 3 : it->second;
}

inline std::vector<Leg> roster() {
    const std::string B = csv_dir() + "/BTCUSDT_1d.csv";
    const std::string E = csv_dir() + "/ETHUSDT_1d.csv";
    const std::string S = csv_dir() + "/SOLUSDT_1d.csv";
    const std::string N = ndx_csv();
    return {
        {"eth_emax",  "ETH", E, 28, "EMAx",    0.20, "ETH Spot-Quoted (QEF)",   "Trend (EMAx)"},
        {"eth_kelt",  "ETH", E, 28, "Kelt",    0.20, "ETH Spot-Quoted (QEF)",   "Trend (Keltner)"},
        {"btc_emax",  "BTC", B, 14, "EMAx",    0.01, "BTC Spot-Quoted (QTF)",   "Trend (EMAx)"},
        {"btc_kelt",  "BTC", B, 14, "Kelt",    0.01, "BTC Spot-Quoted (QTF)",   "Trend (Keltner)"},
        {"sol_emax",  "SOL", S, 11, "EMAx",    5.00, "SOL (fut, SQF pending)",  "Trend (EMAx)"},
        {"sol_kelt",  "SOL", S, 11, "Kelt",    5.00, "SOL (fut, SQF pending)",  "Trend (Keltner)"},
        {"ndx_tsmom", "NDX", N,  4, "TSMom50", 0.10, "Nasdaq-100 SQF (QNDX)",   "Trend (TSMom50)"},
        {"btc_reg",   "BTC", B, 14, "Regime",  0.01, "BTC Spot-Quoted (QTF)",   "Regime-switch"},
        {"eth_reg",   "ETH", E, 28, "Regime",  0.20, "ETH Spot-Quoted (QEF)",   "Regime-switch"},
        {"sol_reg",   "SOL", S, 11, "Regime",  5.00, "SOL (fut, SQF pending)",  "Regime-switch"},
        {"btc_ibs",   "BTC", B, 14, "IBS",     0.01, "BTC Spot-Quoted (QTF)",   "Mean-Rev (IBS)"},
        {"sol_ibs",   "SOL", S, 11, "IBS",     5.00, "SOL (fut, SQF pending)",  "Mean-Rev (IBS)"},
        {"ndx_rsir",  "NDX", N,  4, "RSIrev",  0.10, "Nasdaq-100 SQF (QNDX)",   "MeanRev (RSIrev)"},
        {"btc_roc",   "BTC", B, 14, "Roc",     0.01, "BTC Spot-Quoted (QTF)",   "Momentum (Roc20)"},
        {"sol_roc",   "SOL", S, 11, "Roc",     5.00, "SOL (fut, SQF pending)",  "Momentum (Roc20)"},
    };
}

} // namespace crypto
