// IBKRCrypto INTRADAY SHADOW LEDGER (C++ port of refresh_shadow_intraday.py):
// sibling of shadow_refresh.cpp for the 1h/4h trend stack. SEPARATE state dir
// (data/ibkrcrypto_intraday/) so the daily book is never touched. Same faithful-
// flip accounting; every leg tagged with its IBKR venue + cost(bps) + tf_ms so the
// perp-vs-ladder comparison reads directly off the book.
//
// Built for state.json PARITY with refresh_shadow_intraday.py on identical inputs
// (verify-before-cutover gate). Deltas vs the daily producer: 34-leg roster with
// per-leg tf_ms (H4/H1), tf-scaled freshness bound ((tf_ms/day)*3+0.1), a
// (sym,strat) regime-gate set, a BT_TF_MS env on every ibkrcrypto_bt call, 4-decimal
// px, POOL default 10000, engine tag "IBKRCrypto-Intraday". NO NDX leg / live mark /
// clip / BE-ratchet / companion protection (those are daily-book concepts).
#include <cstdio>
#include <cmath>
#include <string>
#include <vector>
#include <map>
#include <set>
#include <algorithm>
#include <fstream>
#include <sstream>
#include <sys/stat.h>
#include "json.hpp"
#include "crypto/Roster.hpp"      // env_or, csv_dir, fund_dir, bt_bin, slip_bps
#include "crypto/DataHealth.hpp"  // integrity_gate, data_age_days
#include "crypto/StateJson.hpp"   // Prior, load_prior, round_n, json
#include "crypto/ShadowBook.hpp"  // run_capture, cost_env_prefix, parse_after, pearson, unix_from, utc_now_min

using crypto::json;
using namespace crypto;

static bool file_exists(const std::string& p) { struct stat s; return stat(p.c_str(), &s) == 0; }

// Python round(x) -> int uses banker's rounding (half-to-even). Contracts feed
// notional/pnl, so match it exactly to keep parity.
static long py_round_int(double x) {
    double fl = std::floor(x);
    double diff = x - fl;
    if (diff < 0.5) return (long)fl;
    if (diff > 0.5) return (long)fl + 1;
    long a = (long)fl;
    return (a % 2 == 0) ? a : a + 1; // half -> even
}

static std::string fmt(double v, int prec) {
    char buf[64]; std::snprintf(buf, sizeof(buf), "%.*f", prec, v); return std::string(buf);
}

static const long H4 = 14400000, H1 = 3600000;

// (key, sym, csvf, tf_ms, cost_bps, strat, mult, dsym, dstrat)
struct ILeg {
    std::string key, sym, csvf;
    long        tf_ms;
    int         cost;
    std::string strat;
    double      mult;
    std::string dsym, dstrat;
};

static std::string D(const std::string& sym, const std::string& tf) {
    return csv_dir() + "/" + sym + "USDT_" + tf + ".csv";
}

// Faithful transcription of ROSTER in refresh_shadow_intraday.py (34 legs).
static std::vector<ILeg> intraday_roster() {
    const std::string B4 = D("BTC","4h"), E4 = D("ETH","4h"), S4 = D("SOL","4h");
    const std::string B1 = D("BTC","1h"), E1 = D("ETH","1h"), S1 = D("SOL","1h");
    return {
     // --- BTC: ladder MBT(6) vs perp QTF-SQF(22), 4h ---
     {"btc_emax_mbt","BTC",B4,H4, 6,"EMAx",   0.01,"BTC MBT micro (ladder ~6bps)","Trend EMAx 4h"},
     {"btc_kelt_mbt","BTC",B4,H4, 6,"Kelt",   0.01,"BTC MBT micro (ladder ~6bps)","Trend Kelt 4h"},
     {"btc_emax_sqf","BTC",B4,H4,22,"EMAx",   0.01,"BTC QTF SQF perp (~22bps)","Trend EMAx 4h"},
     {"btc_kelt_sqf","BTC",B4,H4,22,"Kelt.vt",0.01,"BTC QTF SQF perp (~22bps)","Trend Kelt.vt 4h"},
     // --- ETH: ladder MET(8) vs perp QEF-SQF(28), 4h ---
     {"eth_emax_met","ETH",E4,H4, 8,"EMAx",   0.20,"ETH MET micro (ladder ~8bps)","Trend EMAx 4h"},
     {"eth_kelt_met","ETH",E4,H4, 8,"Kelt",   0.20,"ETH MET micro (ladder ~8bps)","Trend Kelt 4h"},
     {"eth_ichi_met","ETH",E4,H4, 8,"Ichi",   0.20,"ETH MET micro (ladder ~8bps)","Trend Ichi 4h"},
     {"eth_tsmom_met","ETH",E4,H4,8,"TSMom50",0.20,"ETH MET micro (ladder ~8bps)","Trend TSMom50 4h"},
     {"eth_macd_met","ETH",E4,H4, 8,"Macd",   0.20,"ETH MET micro (ladder ~8bps)","Trend Macd 4h"},
     {"eth_donch40_met","ETH",E4,H4,8,"Donch40",0.20,"ETH MET micro (ladder ~8bps)","Breakout Donch40 4h"},
     {"eth_emax_sqf","ETH",E4,H4,28,"EMAx",   0.20,"ETH QEF SQF perp (~28bps)","Trend EMAx 4h"},
     {"eth_kelt_sqf","ETH",E4,H4,28,"Kelt.vt",0.20,"ETH QEF SQF perp (~28bps)","Trend Kelt.vt 4h"},
     {"eth_ichi_sqf","ETH",E4,H4,28,"Ichi.vt",0.20,"ETH QEF SQF perp (~28bps)","Trend Ichi.vt 4h"},
     {"eth_tsmom_sqf","ETH",E4,H4,28,"TSMom50",0.20,"ETH QEF SQF perp (~28bps)","Trend TSMom50 4h"},
     {"eth_macd_sqf","ETH",E4,H4,28,"Macd.vt",0.20,"ETH QEF SQF perp (~28bps)","Trend Macd.vt 4h"},
     // Donch40 = ONLY non-EMAx strat that BEATS the perp wall (DonchianPerpBreakout).
     {"eth_donch40_sqf","ETH",E4,H4,28,"Donch40",0.20,"ETH QEF SQF perp (~28bps)","Breakout Donch40 4h"},
     // --- SOL: ladder SOL-fut(2) only (QSOL SQF not on account yet), 4h ---
     {"sol_emax_fut","SOL",S4,H4, 2,"EMAx",   5.00,"SOL fut (ladder ~2bps; QSOL pending)","Trend EMAx 4h"},
     {"sol_kelt_fut","SOL",S4,H4, 2,"Kelt.vt",5.00,"SOL fut (ladder ~2bps; QSOL pending)","Trend Kelt.vt 4h"},
     {"sol_ichi_fut","SOL",S4,H4, 2,"Ichi",   5.00,"SOL fut (ladder ~2bps; QSOL pending)","Trend Ichi 4h"},
     {"sol_tsmom_fut","SOL",S4,H4,2,"TSMom50",5.00,"SOL fut (ladder ~2bps; QSOL pending)","Trend TSMom50 4h"},
     {"sol_macd_fut","SOL",S4,H4, 2,"Macd",   5.00,"SOL fut (ladder ~2bps; QSOL pending)","Trend Macd 4h"},
     // --- 1h EMAx survivors (only slow EMAx clears the 1h cost wall) ---
     {"eth_emax_1h","ETH",E1,H1, 8,"EMAx",   0.20,"ETH MET micro 1h (ladder ~8bps)","Trend EMAx 1h"},
     {"sol_emax_1h","SOL",S1,H1, 2,"EMAx",   5.00,"SOL fut 1h (ladder ~2bps)","Trend EMAx 1h"},
     {"sol_donch40_1h","SOL",S1,H1, 2,"Donch40",5.00,"SOL fut 1h (ladder ~2bps; QSOL pending)","Breakout Donch40 1h"},
     // --- UPJUMP companion parents (S-2026-07-03, CryptoUpJumpCompanion) ---
     // MAJORS RE-LOCKED S-2026-07-07 (backtest/rider_sweep_wf.py, walk-forward IS/OOS + plateau,
     // real 23-25bp RT, run on BOTH the 9y Binance archive and the live 5.5y CSVs): the uniform
     // 2%/24h trigger is NET NEGATIVE across the full history; the robust plateau is the BIG slow
     // jump -- 48-bar window, BTC/ETH 4%, SOL 7% (per-coin thr reflects vol, mechanism identical).
     // OOS nets: BTC +15.8k bp (9y) / +3.5-4k (5.5y), ETH +4.8-11.7k, SOL +1.6-2.3k. The alt legs
     // below keep UpJump2 UNTESTED at the new definition -- re-sweep before promoting any of them.
     {"btc_upjump","BTC",B1,H1,18,"UpJump4x48", 1.0,"BTC spot up-jump (companion parent, 0.20% RT)","UpJump 4%x48h 1h"},
     {"eth_upjump","ETH",E1,H1,18,"UpJump4x48", 1.0,"ETH spot up-jump (companion parent, 0.20% RT)","UpJump 4%x48h 1h"},
     {"sol_upjump","SOL",S1,H1,18,"UpJump7x48", 1.0,"SOL spot up-jump (companion parent, 0.20% RT)","UpJump 7%x48h 1h"},
     // S-2026-07-12 UpJump viability audit (faithful --protect-sweep, 1h, 20bp RT, gate =
     // net+ & PF>=1.3 on OOS_23-26 fair long-only window, 2x-cost-checked). The 2%/24h
     // "UpJump2" config was a LOSER on every alt (OP FULL -264/OOS -343, ADA/AAVE negative,
     // DOGE/BNB/NEAR failed OOS). A loss-cut mimic does NOT rescue it (tested hs=0.05..0.30 ->
     // worse/unchanged: the loss is the wrong signal firing, not a deep tail). The BIG-slow-jump
     // config (4%/48h, 7%/48h) that already passes on majors DOES pass on the alts -> migrated.
     // CULLED (fail gate on EVERY config): op_upjump (dead all), ada_upjump (OOS PF 1.27),
     // aave_upjump (OOS PF 1.17). ADVERSE-PROTECTION: flip/selectivity-only -- a cold loss-cut
     // LOWERS net (backtested S-2026-07-12, hs sweep), so no floor is added by design.
     {"doge_upjump","DOGE",D("DOGE","1h"),H1,18,"UpJump7x48",1.0,"DOGE spot up-jump (companion parent, 0.20% RT)","UpJump 7%x48h 1h"},
     {"trx_upjump","TRX",D("TRX","1h"),H1,18,"UpJump4x48", 1.0,"TRX spot up-jump (companion parent, 0.20% RT)","UpJump 4%x48h 1h"},
     {"near_upjump","NEAR",D("NEAR","1h"),H1,18,"UpJump4x48",1.0,"NEAR spot up-jump (companion parent, 0.20% RT)","UpJump 4%x48h 1h"},
     {"bnb_upjump","BNB",D("BNB","1h"),H1,18,"UpJump7x48",1.0,"BNB spot up-jump (companion parent, 0.20% RT)","UpJump 7%x48h 1h"},
    };
}

// REGIME GATE — LIVE BOOK IS UNGATED (feedback-no-200dma-crypto). rma=0 by default, so the
// live intraday ledger is byte-identical to the ungated book. The 2026-07-06 blanket ban stands
// for the LIVE path.
//
// SHADOW A/B (S-2026-07-10, ablation-backed) — the blanket "NO 200DMA" ban is being tested for a
// NARROW, operator-authorised exception ("show me real value in trades", 2026-07-10). The DD-
// attribution ablation (backtest/ibkrcrypto_bt --protect-sweep, gate OFF vs REGIME_MA=200) showed
// a per-symbol close>SMA200 gate BEATS vol-target on the high-vol trend legs (ETH/SOL EMAx/TSMom:
// net-preserving DD cut where vt crushes net) and is the ONLY tool that fully avoids bear-regime
// longs (2022 pruned to zero). BTC-TSMom is EXCLUDED (vt wins there). Companions/UpJump are
// EXCLUDED (a gate there only sits in cash — the original ban rationale).
//
// This is a SHADOW A/B ONLY: gated iff env AB_GATE_TREND is set (=200), and ONLY when run with a
// separate IBKRCRYPTO_DATADIR so the LIVE ledger + stall_companion + GUI are never touched. Do NOT
// flip the live book to gated without forward A/B evidence + explicit operator sign-off.
static bool ab_gate_leg(const ILeg& L) {
    if (L.key.find("upjump") != std::string::npos) return false;   // companions NEVER gated
    if (L.sym != "ETH" && L.sym != "SOL")          return false;   // BTC: vt wins, no gate
    return true;                                                    // remaining ETH/SOL legs are trend legs
}

// FRESHNESS GUARD: tf-scaled. 4h -> ~14h, 1h -> ~5.4h.
static double max_age_days_tf(long tf_ms) { return (tf_ms / 86400000.0) * 3.0 + 0.1; }
struct IFresh { bool ok; double bar; double file; };
static IFresh fresh_ok_tf(const std::string& path, long tf_ms) {
    DataAge a = data_age_days(path);
    double m = max_age_days_tf(tf_ms);
    return {a.bar <= m && a.file <= m, a.bar, a.file};
}

// ibkrcrypto_bt --signal with the intraday BT_TF_MS env added (mirrors signal()).
static Sig run_signal_tf(const std::string& sym, const std::string& csvf, int cost,
                         const std::string& strat, long rma, long tf_ms,
                         const std::string& vttgt = "") {
    std::string cmd = cost_env_prefix(sym)
        + "COSTBPS=" + std::to_string(cost) + " BT_TF_MS=" + std::to_string(tf_ms)
        + " REGIME_MA=" + std::to_string(rma)
        + (vttgt.empty() ? std::string() : " VTTGT=" + vttgt) + " "
        + "'" + bt_bin() + "' " + sym + " '" + csvf + "' --signal " + strat + " 2>/dev/null";
    std::string out = run_capture(cmd);
    std::stringstream ss(out); std::string ln;
    while (std::getline(ss, ln)) {
        if (ln.find("target=") != std::string::npos) {
            double t = 0, sz = 1.0, px = 0.0, ex = 0.0;
            parse_after(ln, "target=", t);
            parse_after(ln, "size=", sz);
            parse_after(ln, "px=", px);
            parse_after(ln, "exit=", ex);
            return {(int)t, sz, px, ex};
        }
    }
    return {0, 1.0, 0.0, 0.0};
}

// ibkrcrypto_bt --equity with BT_TF_MS env; {day-bucket -> cum_eq} (mirrors _eqret).
static std::map<long, double> eqret_tf(const std::string& sym, const std::string& csvf,
                                       const std::string& strat, long tf_ms) {
    std::string cmd = cost_env_prefix(sym)
        + "BT_TF_MS=" + std::to_string(tf_ms) + " "
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

struct ILegRef { std::string key, sym, csvf, strat; long tf_ms; };

// {key -> (corr, size_scale)} leave-one-out corr vs the rest of the book (mirrors corr_downsize).
static std::map<std::string, std::pair<double, double>> corr_downsize_intraday(const std::vector<ILegRef>& legs) {
    int CORR_WIN = std::atoi(env_or("CORR_WIN", "30").c_str());
    double CORR_THR = std::stod(env_or("CORR_THR", "0.5"));
    double CORR_SCALE = std::stod(env_or("CORR_SCALE", "0.5"));
    std::map<std::string, std::pair<double, double>> out;
    if (CORR_THR <= 0) { for (auto& L : legs) out[L.key] = {0.0, 1.0}; return out; }

    std::map<std::string, std::map<long, double>> cur;
    for (auto& L : legs) cur[L.key] = eqret_tf(L.sym, L.csvf, L.strat, L.tf_ms);

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

// _unix(): "YYYY-mm-dd HH:MM" -> unix seconds, 0 on failure (matches the Python helper,
// which returns 0 on a None/unparseable ets rather than a now() fallback).
static long ets_unix(const json& ets) {
    if (ets.is_string()) return unix_from(ets.get<std::string>());
    return 0;
}

int main() {
    const std::string DATADIR = env_or("IBKRCRYPTO_DATADIR",
                                       "/Users/jo/Crypto/backtest/data/ibkrcrypto_intraday");
    const std::string STATE   = DATADIR + "/state.json";
    const std::string LEDGER  = DATADIR + "/ledger.csv";
    const std::string INBOUND = DATADIR + "/crypto_inbound.csv";
    mkdir(DATADIR.c_str(), 0755);

    Prior prior = load_prior(STATE);
    const std::string now = utc_now_min();
    const double POOL = std::stod(env_or("POOL_USD", "10000"));

    bool newled = !file_exists(LEDGER);
    bool newinb = !file_exists(INBOUND);
    std::ofstream led(LEDGER, std::ios::app);
    std::ofstream inb(INBOUND, std::ios::app);
    if (newinb) inb << "id,entry_ts,exit_ts,sym,strat,side,entry,exit,net_usd\n";
    if (newled) led << "ts,key,event,pos,px,realized_pct,cum_pct\n";

    // ── pass 1: resolve every leg's signal so we can count the open legs ──
    struct LegState {
        ILeg L;
        bool fresh, integ, clean;
        double bar_age, file_age;
        int t; double sz, px, expx;
    };
    std::vector<LegState> legs;
    auto R = intraday_roster();
    for (auto& L : R) {
        IFresh fr = fresh_ok_tf(L.csvf, L.tf_ms);
        bool integ = integrity_gate(L.csvf);
        json p0 = prior.slots_by_key.contains(L.key) ? prior.slots_by_key[L.key] : json::object();
        int t; double sz, px, expx;
        if (!fr.ok) {
            std::fprintf(stderr, "[STALE-DATA] %s %s bar_age=%.2fd file_age=%.2fd (max %.2fd) -- FROZEN, no trade booked off stale data\n",
                         L.key.c_str(), L.sym.c_str(), fr.bar, fr.file, max_age_days_tf(L.tf_ms));
            t = p0.value("pos", 0); sz = 1.0;
            px = (p0.contains("px") && p0["px"].is_number() && p0["px"].get<double>())
                 ? p0["px"].get<double>()
                 : (p0.contains("entry_px") && p0["entry_px"].is_number() ? p0["entry_px"].get<double>() : 0.0);
            expx = 0.0;
        } else if (!integ) {
            t = 0; sz = 1.0; px = 0.0; expx = 0.0;
        } else {
            // LIVE: rma=0 (ungated). SHADOW A/B: if AB_GATE_TREND set AND this is an ETH/SOL
            // trend leg, gate on per-symbol close>SMA(rma) + vt OFF (the ablation-winning config).
            // Guarded so the live book (env unset) is byte-identical. See ab_gate_leg + header note.
            long rma = 0; std::string vtenv;
            if (const char* g = getenv("AB_GATE_TREND"); g && *g && ab_gate_leg(L)) {
                // AB_GATE_TREND is the MA length in *DAYS* (200 = the ablation-validated 200-day gate).
                // The BT computes SMA on the leg's OWN bars, so convert days->bars via bars-per-day
                // (4h leg -> 200*6=1200 bars, 1h leg -> 200*24=4800 bars) to reproduce a true 200-DAY
                // regime SMA on intraday data. A bare 200 here would be a 200-BAR (~33d/~8d) gate — a
                // DIFFERENT, unvalidated gate. bpd from tf_ms keeps every leg's gate at the same
                // calendar lookback the daily ablation used.
                long bpd = 86400000L / L.tf_ms;      // bars/day for this leg's timeframe
                rma = atol(g) * bpd;                 // MA length in the leg's own bars
                vtenv = "0";                         // vt OFF: gate replaces vol-target on the high-vol leg
            }
            Sig s = run_signal_tf(L.sym, L.csvf, L.cost, L.strat, rma, L.tf_ms, vtenv);
            t = s.t; sz = s.sz; px = s.px; expx = s.expx;
        }
        legs.push_back({L, fr.ok, integ, fr.ok && integ,
                        round_n(fr.bar, 2), round_n(fr.file, 2), t, sz, px, expx});
    }
    // KILL_FLAT marker (GUI kill button): force every leg flat -> pass 2's flip-close path books each out.
    const bool KILLED = file_exists(DATADIR + "/KILL_FLAT");
    if (KILLED) { for (auto& g : legs) g.t = 0; }
    int n_open = 0; for (auto& g : legs) if (g.t != 0) n_open++;
    const double per_leg = POOL / std::max(1, n_open);

    std::vector<ILegRef> legrefs;
    for (auto& g : legs) legrefs.push_back({g.L.key, g.L.sym, g.L.csvf, g.L.strat, g.L.tf_ms});
    auto cmap = corr_downsize_intraday(legrefs);

    auto qty_for = [&](double px_basis, double mult, double scale) -> long {
        double pc = px_basis * mult;
        return pc > 0 ? std::max(1L, py_round_int(per_leg * scale / pc)) : 1L;
    };

    // ── pass 2: size + P&L ──
    json slots = json::array();
    json closed = prior.closed;
    double tot_real = 0, tot_unreal = 0, tot_real_usd = 0, tot_unreal_usd = 0, deployed = 0;
    int ntrades = 0;

    for (auto& g : legs) {
        const ILeg& L = g.L;
        int t = g.t; double sz = g.sz, px = g.px, expx = g.expx; bool clean = g.clean;
        json p = prior.slots_by_key.contains(L.key) ? prior.slots_by_key[L.key] : json::object();
        int pos0 = p.value("pos", 0);
        double epx = (p.contains("entry_px") && p["entry_px"].is_number()) ? p["entry_px"].get<double>() : px;
        if (epx == 0) epx = px;
        json ets = p.contains("entry_ts") ? p["entry_ts"] : json(nullptr);
        double cum = p.value("realized_pct", 0.0);
        double c = (L.cost + 2) / 10000.0;
        double cum_usd = p.value("realized_usd", 0.0);
        auto cm = cmap[L.key]; double corr = cm.first, scale = cm.second;
        long qty = qty_for((t != 0 && epx > 0) ? epx : px, L.mult, scale);
        // CONTRACTS/VT FREEZE (honest-accounting, S-2026-07-07): a real position holds the
        // contracts it OPENED with; recomputing each run booked P&L on a phantom quantity.
        double sz_book = sz;
        if (pos0 != 0) {
            if (p.contains("contracts") && p["contracts"].is_number() && p["contracts"].get<double>() > 0)
                qty = (long)p["contracts"].get<double>();
            if (p.contains("vt") && p["vt"].is_number() && p["vt"].get<double>() > 0)
                sz_book = p["vt"].get<double>();
        }

        if (t != pos0) {                            // ---- TRADE (flip) ----
            if (pos0 != 0 && epx > 0) {             // close old leg -> realized
                long q0 = qty;                       // entry-frozen contracts (see freeze above)
                double r = (pos0 * (px - epx) / epx * sz_book - c * sz_book) * 100; cum += r; ntrades++;
                double r_usd = pos0 * (px - epx) * L.mult * q0
                               - (L.cost + 2) / 10000.0 * px * L.mult * q0; cum_usd += r_usd;
                json ce = {
                    {"sym", L.dsym}, {"strat", L.dstrat}, {"dir", pos0 > 0 ? "LONG" : "SHORT"},
                    {"entry_ts", ets}, {"exit_ts", now},
                    {"entry_px", round_n(epx, 4)}, {"exit_px", round_n(px, 4)},
                    {"contracts", q0}, {"realized_pct", round_n(r, 3)},
                    {"realized_usd", round_n(r_usd, 2)}, {"mult", L.mult}
                };
                closed.insert(closed.begin(), ce);
                led << now << "," << L.key << ",CLOSE " << pos0 << "," << t << ","
                    << fmt(px, 4) << "," << fmt(r, 3) << "," << fmt(cum, 3) << "\n";
                std::string side = pos0 > 0 ? "LONG" : "SHORT";
                std::string uid = L.key + "_" + std::to_string(unix_from(now));
                inb << uid << "," << ets_unix(ets) << "," << unix_from(now) << "," << L.sym << ","
                    << L.strat << "," << side << "," << fmt(epx, 4) << "," << fmt(px, 4)
                    << "," << fmt(r_usd, 2) << "\n";
                inb.flush();
            }
            if (t != 0) {
                epx = px; ets = json(now); qty = qty_for(epx, L.mult, scale);
                sz_book = sz;                        // fresh open: freeze THIS vt for the trade's life
                led << now << "," << L.key << ",OPEN " << t << "," << t << ","
                    << fmt(px, 4) << ",," << fmt(cum, 3) << "\n";
                ntrades++;
            }
        }
        double unreal = (t != 0 && epx > 0) ? t * (px - epx) / epx * sz_book * 100 : 0.0;
        double unreal_usd = (t != 0 && epx > 0) ? t * (px - epx) * L.mult * qty : 0.0;
        if (t != 0) deployed += px * L.mult * qty;
        tot_real += cum; tot_unreal += unreal; tot_real_usd += cum_usd; tot_unreal_usd += unreal_usd;

        json slot = {
            {"key", L.key}, {"sym", L.dsym}, {"strat", L.dstrat}, {"pos", t},
            {"contracts", t ? qty : 0L}, {"vt", round_n(t ? sz_book : sz, 2)}, {"mult", L.mult}, {"pool", POOL},
            {"tf_ms", L.tf_ms}, {"cost_bps", L.cost},
            {"corr", round_n(corr, 2)}, {"downsized", scale < 1.0 && t != 0},
            {"notional", t ? round_n(px * L.mult * qty, 0) : 0},
            {"px", round_n(px, 4)}, {"entry_px", round_n(epx, 4)}, {"entry_ts", ets},
            {"exit_px", round_n(expx, 4)}, {"unreal_pct", round_n(unreal, 3)},
            {"unreal_usd", round_n(unreal_usd, 2)}, {"realized_pct", round_n(cum, 3)},
            {"realized_usd", round_n(cum_usd, 2)}, {"clean", clean},
            {"fresh", g.fresh}, {"stale", !g.fresh},
            {"bar_age_d", g.bar_age}, {"file_age_d", g.file_age}
        };
        slots.push_back(slot);
    }
    led.close(); inb.close();
    if (closed.size() > 25) closed.erase(closed.begin() + 25, closed.end());

    // ── book-level DATA HEALTH (per-source freshness) ──
    json sources = json::object();
    std::vector<std::string> stale_sources;
    for (auto& g : legs) {
        std::string b = g.L.csvf.substr(g.L.csvf.find_last_of('/') + 1);
        if (!sources.contains(b))
            sources[b] = {{"bar_age_d", g.bar_age}, {"file_age_d", g.file_age},
                          {"max_age_d", round_n(max_age_days_tf(g.L.tf_ms), 2)}, {"fresh", g.fresh}};
    }
    for (auto it = sources.begin(); it != sources.end(); ++it)
        if (!it.value()["fresh"].get<bool>()) stale_sources.push_back(it.key());
    std::sort(stale_sources.begin(), stale_sources.end());

    std::string FLAG = DATADIR + "/stale_data.flag";
    if (!stale_sources.empty()) {
        std::ofstream ff(FLAG);
        ff << now << " UTC STALE: ";
        for (size_t i = 0; i < stale_sources.size(); ++i) ff << (i ? ", " : "") << stale_sources[i];
        ff << "\n";
        std::fprintf(stderr, "[STALE-DATA] BOOK NOT ALL-FRESH -- frozen sources\n");
    } else if (file_exists(FLAG)) {
        std::remove(FLAG.c_str());
    }

    int n_open_state = 0; for (auto& s : slots) if (s["pos"].get<int>()) n_open_state++;
    json data_health = {{"all_fresh", stale_sources.empty()},
                        {"stale_sources", stale_sources}, {"sources", sources}};

    json out = {
        {"engine", env_or("ENGINE_TAG", "IBKRCrypto-Intraday")}, {"mode", "SHADOW"}, {"updated", now + " UTC"},
        {"data_health", data_health},
        {"open_unreal_pct", round_n(tot_unreal, 2)}, {"realized_pct", round_n(tot_real, 2)},
        {"total_pct", round_n(tot_unreal + tot_real, 2)},
        {"open_unreal_usd", round_n(tot_unreal_usd, 2)}, {"realized_usd", round_n(tot_real_usd, 2)},
        {"total_usd", round_n(tot_unreal_usd + tot_real_usd, 2)},
        {"pool_usd", POOL}, {"deployed_usd", round_n(deployed, 0)},
        {"n_open", n_open_state}, {"killed", KILLED},
        {"slots", slots}, {"closed", closed}
    };
    // ATOMIC write (tmp+rename): stall_companion (crypto books) reads this file every cron
    // cycle -- a partial read parsed as "no slots" mass-ENGINE_EXITs every open companion.
    { std::ofstream sf(STATE + ".tmp"); sf << out.dump(1); }
    std::rename((STATE + ".tmp").c_str(), STATE.c_str());

    std::printf("refresh-intraday: %d new trade-events, %d open, unreal $%.2f (%.2f%%) realized $%.2f (%.2f%%)\n",
                ntrades, n_open_state, tot_unreal_usd, tot_unreal, tot_real_usd, tot_real);
    return 0;
}
