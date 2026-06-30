// IBKRCrypto SHADOW LEDGER (C++ port of refresh_shadow.py): paper-as-real executor.
// A "trade" = a position FLIP. On flip: close old leg (bank realized P&L at full cost)
// + open new (record entry px/time). Open legs show live unrealized P&L. Built for
// state.json parity with the Python driver on identical inputs (Phase 1, T8).
#include <cstdio>
#include <cmath>
#include <string>
#include <vector>
#include <map>
#include <fstream>
#include <sstream>
#include <sys/stat.h>
#include "json.hpp"
#include "crypto/Roster.hpp"
#include "crypto/DataHealth.hpp"
#include "crypto/StateJson.hpp"
#include "crypto/ShadowBook.hpp"

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

// ── LIVE NDX MARK from our IBKR feed (SSH omega-vps -> aurora_NQ.json). Reject >30min
// stale. Fallback: daily-close. Mirrors _read_live_nq. Returns NQ price or NaN.
static double read_live_nq() {
    if (std::getenv("CRYPTO_SKIP_LIVE_NDX")) return std::nan("");
    std::string cmd =
        "ssh -o ConnectTimeout=6 -o BatchMode=yes omega-vps "
        "'powershell -NoProfile -Command \"$j=Get-Content C:\\Omega\\logs\\aurora\\aurora_NQ.json -Raw|ConvertFrom-Json; \\\"$($j.price) $($j.stamp_ms)\\\"\"' 2>/dev/null";
    std::string out = run_capture(cmd);
    std::stringstream ss(out);
    double nq = 0; ss >> nq;
    double stamp = 0; ss >> stamp;
    if (nq > 0) {
        double now = (double)std::time(nullptr);
        if (stamp == 0 || (now - stamp / 1000.0) < 1800) return nq;
        std::fprintf(stderr, "[live-mark] aurora NQ stale (>30min) -> daily-close fallback\n");
    }
    return std::nan("");
}

int main() {
    const std::string DATADIR = data_dir();
    const std::string STATE   = DATADIR + "/state.json";
    const std::string LEDGER  = DATADIR + "/ledger.csv";
    const std::string INBOUND = DATADIR + "/crypto_inbound.csv";

    Prior prior = load_prior(STATE);
    std::map<std::string, int> clipped;
    for (auto it = prior.clipped.begin(); it != prior.clipped.end(); ++it)
        clipped[it.key()] = it.value().get<int>();

    const std::string now = utc_now_min();
    // Account capital = NZ$5000 (operator basis 2026-06-30). NZDUSD 0.584 (latest tick
    // 2026-04-10, ~/Tick/NZDUSD; no live NZDUSD feed) -> USD ~2920. Override via POOL_USD
    // env to correct FX/resize. Was 10000 (pre-cutover paper). Single live book = daily.
    const double POOL = std::stod(env_or("POOL_USD", "2920"));

    // COMPANION PROTECTION (default OFF; env-gated). Real-leg rides wide.
    const bool   PROTECT       = env_or("COMPANION_PROTECT", "0") != "0";
    const double PROT_GATE_USD = std::stod(env_or("PROT_GATE_USD", "25"));
    const double PROT_TRAIL    = std::stod(env_or("PROT_TRAIL", "0.40"));
    const double PROT_STALL_SEC= std::stod(env_or("PROT_STALL_SEC", "86400"));
    const double PROT_DEAD_SEC = std::stod(env_or("PROT_DEAD_SEC", "259200"));
    const double PROT_DEAD_USD = std::stod(env_or("PROT_DEAD_USD", "-20"));

    // BE-RATCHET on the NDX index-trend leg only (S-2026-06-30, NdxCompanionClip).
    // NDX (TSMom50) is the ONE leg where a break-even ratchet HELPS. NOT the giveback
    // companion -- that clip DESTROYS this trend runner (faithful ibkrcrypto_bt: 5% giveback
    // +59% PF1.16, maxDD WORSE 38.9->44.7, churns 76->350 trades). The engine-native ratchet
    // cuts green->red round-trips WITHOUT clipping runners: arm +1.5% / floor breakeven ->
    // +95.6% PF1.78 vs wide +87% PF1.72, better OOS (PF2.65 DD8.6%). NDX-only, default ON.
    const bool   NDX_BE       = env_or("NDX_BE_RATCHET", "1") != "0";
    const double NDX_BE_ARM   = std::stod(env_or("NDX_BE_ARM", "0.015"));
    const double NDX_BE_FLOOR = std::stod(env_or("NDX_BE_FLOOR", "0.0"));

    bool newled = !file_exists(LEDGER);
    bool newinb = !file_exists(INBOUND);
    std::ofstream led(LEDGER, std::ios::app);
    std::ofstream inb(INBOUND, std::ios::app);
    if (newinb) inb << "id,entry_ts,exit_ts,sym,strat,side,entry,exit,net_usd\n";
    if (newled) led << "ts,key,event,pos,px,realized_pct,cum_pct\n";

    // ── daily-anchored NDX cash<-NQ live mark ──
    double ndx_cash_close = 0; std::string ndx_cash_date;
    {
        std::ifstream nf(ndx_csv());
        std::string line, last;
        while (std::getline(nf, line)) if (!line.empty()) last = line;
        if (!last.empty()) {
            std::vector<std::string> c; std::stringstream ls(last); std::string cell;
            while (std::getline(ls, cell, ',')) c.push_back(cell);
            if (c.size() >= 5) { try { ndx_cash_close = std::stod(c[4]); ndx_cash_date = c[0]; } catch (...) {} }
        }
    }
    double nq_live = read_live_nq();
    const double NDX_NQ_BASIS = 0.991;
    double ndx_live_mark = std::nan("");
    if (std::isfinite(nq_live) && ndx_cash_close > 0) {
        double m = nq_live * NDX_NQ_BASIS;
        if (ndx_cash_close * 0.96 <= m && m <= ndx_cash_close * 1.05) ndx_live_mark = m;
    }
    bool have_live_ndx = std::isfinite(ndx_live_mark) && ndx_live_mark > 0;

    // ── pass 1: resolve every leg's signal so we can count the open legs ──
    struct LegState {
        Leg L;
        bool fresh, integ, clean;
        double bar_age, file_age;
        int t; double sz, px, expx;
    };
    std::vector<LegState> legs;
    auto R = roster();
    for (auto& L : R) {
        Freshness fr = fresh_ok(L.csvf, L.sym);
        bool integ = integrity_gate(L.csvf);
        json p0 = prior.slots_by_key.contains(L.key) ? prior.slots_by_key[L.key] : json::object();
        int t; double sz, px, expx;
        if (!fr.ok) {
            std::fprintf(stderr, "[STALE-DATA] %s %s bar_age=%.1fd file_age=%.1fd -- FROZEN\n",
                         L.key.c_str(), L.sym.c_str(), fr.bar, fr.file);
            t = p0.value("pos", 0); sz = 1.0;
            px = p0.contains("px") && p0["px"].get<double>() ? p0["px"].get<double>()
                 : (p0.contains("entry_px") ? p0["entry_px"].get<double>() : 0.0);
            expx = 0.0;
        } else if (!integ) {
            t = 0; sz = 1.0; px = 0.0; expx = 0.0;
        } else {
            int rma = gated_strats().count(L.strat) ? regime_gate_ma() : 0;
            Sig s = run_signal(L.sym, L.csvf, L.cost, L.strat, rma);
            t = s.t; sz = s.sz; px = s.px; expx = s.expx;
        }
        if (L.sym == "NDX" && have_live_ndx) px = ndx_live_mark;
        legs.push_back({L, fr.ok, integ, fr.ok && integ,
                        round_n(fr.bar, 1), round_n(fr.file, 1), t, sz, px, expx});
    }
    int n_open = 0; for (auto& g : legs) if (g.t != 0) n_open++;
    const double per_leg = POOL / std::max(1, n_open);

    std::vector<LegRef> legrefs;
    for (auto& g : legs) legrefs.push_back({g.L.key, g.L.sym, g.L.csvf, g.L.strat});
    auto cmap = corr_downsize(legrefs);

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
        const Leg& L = g.L;
        int t = g.t; double sz = g.sz, px = g.px, expx = g.expx; bool clean = g.clean;
        json p = prior.slots_by_key.contains(L.key) ? prior.slots_by_key[L.key] : json::object();
        int pos0 = p.value("pos", 0);
        double epx = p.contains("entry_px") ? p["entry_px"].get<double>() : px;
        if (epx == 0) epx = px;
        json ets = p.contains("entry_ts") ? p["entry_ts"] : json(nullptr);
        double cum = p.value("realized_pct", 0.0);
        double c = (L.cost + 2) / 10000.0;
        double cum_usd = p.value("realized_usd", 0.0);
        auto cm = cmap[L.key]; double corr = cm.first, scale = cm.second;
        long qty = qty_for((t != 0 && epx > 0) ? epx : px, L.mult, scale);

        double peak_usd = (p.contains("peak_usd") && p["peak_usd"].is_number()) ? p["peak_usd"].get<double>() : 0.0;
        long   peak_ts  = (p.contains("peak_ts")  && p["peak_ts"].is_number())  ? p["peak_ts"].get<long>()    : 0;
        double be_peak  = (p.contains("be_peak")  && p["be_peak"].is_number())  ? p["be_peak"].get<double>()  : 0.0;

        // re-open guard: honor a clip ONLY while its owning protection is enabled (else it's a stale
        // clip from a now-disabled protection -> drop it, don't resurrect a force-flat). Hold flat
        // until the signal flips off the clipped direction.
        {
            auto cl = clipped.find(L.key);
            if (cl != clipped.end()) {
                bool owner_on = PROTECT || (NDX_BE && L.sym == "NDX" && L.strat == "TSMom50");
                if (!owner_on || t != cl->second) clipped.erase(cl);
                else t = 0;
            }
        }
        // BE-RATCHET: NDX index-trend leg only (engine-native break-even ratchet, NOT the giveback companion)
        if (NDX_BE && L.sym == "NDX" && L.strat == "TSMom50" && pos0 != 0 && epx > 0 && t == pos0) {
            double cur_ret = pos0 * (px - epx) / epx;                  // favorable return frac (signed by direction)
            double be_pk = (p.contains("be_peak") && p["be_peak"].is_number()) ? p["be_peak"].get<double>() : cur_ret;
            if (cur_ret > be_pk) be_pk = cur_ret;
            be_peak = be_pk;
            if (be_pk >= NDX_BE_ARM && cur_ret <= NDX_BE_FLOOR) {       // armed (+1.5%) then gave it all back to breakeven
                clipped[L.key] = pos0; t = 0;
                std::fprintf(stderr, "[BE-RATCHET] NDX force-close %s ret=%.2f%% peak=%.2f%%\n",
                             pos0 > 0 ? "LONG" : "SHORT", cur_ret * 100, be_pk * 100);
            }
        }
        // COMPANION PROTECTION (default OFF)
        if (PROTECT) {
            if (pos0 != 0 && epx > 0 && t == pos0) {
                double cur_usd = pos0 * (px - epx) * L.mult * qty;
                double pk = (p.contains("peak_usd") && p["peak_usd"].is_number()) ? p["peak_usd"].get<double>() : cur_usd;
                long   pk_ts = (p.contains("peak_ts") && p["peak_ts"].is_number()) ? p["peak_ts"].get<long>() : unix_from(now);
                if (cur_usd > pk) { pk = cur_usd; pk_ts = unix_from(now); }
                peak_usd = pk; peak_ts = pk_ts;
                long ent = ets.is_string() ? unix_from(ets.get<std::string>()) : unix_from(now);
                long age = unix_from(now) - ent;
                bool armed  = pk >= PROT_GATE_USD;
                bool revers = armed && cur_usd <= pk * (1.0 - PROT_TRAIL);
                bool stalled= armed && (unix_from(now) - pk_ts) >= PROT_STALL_SEC;
                bool dead   = (pk < PROT_GATE_USD) && age >= PROT_DEAD_SEC && cur_usd < PROT_DEAD_USD;
                if (revers || stalled || dead) {
                    const char* reason = revers ? "REVERSAL" : (stalled ? "STALL" : "DEAD_MONEY");
                    clipped[L.key] = pos0; t = 0;
                    std::fprintf(stderr, "[PROTECT] %s force-close %s unreal=$%.2f peak=$%.2f age=%ldh\n",
                                 reason, L.key.c_str(), cur_usd, pk, age / 3600);
                }
            }
        }

        if (t != pos0) {                            // ---- TRADE (flip) ----
            if (pos0 != 0 && epx > 0) {             // close old leg -> realized
                long q0 = qty;
                double r = (pos0 * (px - epx) / epx * sz - c * sz) * 100; cum += r; ntrades++;
                double r_usd = pos0 * (px - epx) * L.mult * q0
                               - (L.cost + 2) / 10000.0 * px * L.mult * q0; cum_usd += r_usd;
                json ce = {
                    {"sym", L.dsym}, {"strat", L.dstrat}, {"dir", pos0 > 0 ? "LONG" : "SHORT"},
                    {"entry_ts", ets}, {"exit_ts", now},
                    {"entry_px", round_n(epx, 2)}, {"exit_px", round_n(px, 2)},
                    {"contracts", q0}, {"realized_pct", round_n(r, 3)},
                    {"realized_usd", round_n(r_usd, 2)}, {"mult", L.mult}
                };
                closed.insert(closed.begin(), ce);
                led << now << "," << L.key << ",CLOSE " << pos0 << "," << t << ","
                    << fmt(px, 4) << "," << fmt(r, 3) << "," << fmt(cum, 3) << "\n";
                std::string side = pos0 > 0 ? "LONG" : "SHORT";
                std::string uid = L.key + "_" + std::to_string(unix_from(now));
                long ets_u = ets.is_string() ? unix_from(ets.get<std::string>()) : unix_from(now);
                inb << uid << "," << ets_u << "," << unix_from(now) << "," << L.sym << ","
                    << L.strat << "," << side << "," << fmt(epx, 4) << "," << fmt(px, 4)
                    << "," << fmt(r_usd, 2) << "\n";
            }
            if (t != 0) {
                epx = px; ets = json(now); qty = qty_for(epx, L.mult, scale);
                peak_usd = 0.0; peak_ts = unix_from(now); be_peak = 0.0;
                led << now << "," << L.key << ",OPEN " << t << "," << t << ","
                    << fmt(px, 4) << ",," << fmt(cum, 3) << "\n";
                ntrades++;
            }
        }
        double unreal = (t != 0 && epx > 0) ? t * (px - epx) / epx * sz * 100 : 0.0;
        double unreal_usd = (t != 0 && epx > 0) ? t * (px - epx) * L.mult * qty : 0.0;
        if (t != 0) deployed += px * L.mult * qty;
        tot_real += cum; tot_unreal += unreal; tot_real_usd += cum_usd; tot_unreal_usd += unreal_usd;

        json slot = {
            {"key", L.key}, {"sym", L.dsym}, {"strat", L.dstrat}, {"pos", t},
            {"contracts", t ? qty : 0}, {"vt", round_n(sz, 2)}, {"mult", L.mult}, {"pool", POOL},
            {"corr", round_n(corr, 2)}, {"downsized", scale < 1.0 && t != 0},
            {"notional", t ? round_n(px * L.mult * qty, 0) : 0},
            {"px", round_n(px, 2)}, {"entry_px", round_n(epx, 2)}, {"entry_ts", ets},
            {"exit_px", round_n(expx, 2)}, {"unreal_pct", round_n(unreal, 3)},
            {"unreal_usd", round_n(unreal_usd, 2)}, {"realized_pct", round_n(cum, 3)},
            {"realized_usd", round_n(cum_usd, 2)}, {"clean", clean},
            {"peak_usd", t ? round_n(peak_usd, 2) : 0.0},
            {"peak_ts", t ? json(peak_ts) : json(nullptr)},
            {"be_peak", (t && L.sym == "NDX") ? round_n(be_peak, 5) : 0.0},
            {"asset_class", L.sym == "NDX" ? "INDEX" : "CRYPTO"},
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
                          {"max_age_d", max_age_for(g.L.sym)}, {"fresh", g.fresh}};
    }
    for (auto it = sources.begin(); it != sources.end(); ++it)
        if (!it.value()["fresh"].get<bool>()) stale_sources.push_back(it.key());
    std::sort(stale_sources.begin(), stale_sources.end());
    json data_health = {{"all_fresh", stale_sources.empty()},
                        {"stale_sources", stale_sources}, {"sources", sources}};

    // stale flag (written to DATADIR; Python writes to live dir — kept out of live during tests)
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
    json clipped_json = json::object();
    for (auto& kv : clipped) clipped_json[kv.first] = kv.second;

    json out = {
        {"engine", "IBKRCrypto"}, {"mode", "SHADOW"}, {"updated", now + " UTC"},
        {"data_health", data_health},
        {"open_unreal_pct", round_n(tot_unreal, 2)}, {"realized_pct", round_n(tot_real, 2)},
        {"total_pct", round_n(tot_unreal + tot_real, 2)},
        {"open_unreal_usd", round_n(tot_unreal_usd, 2)}, {"realized_usd", round_n(tot_real_usd, 2)},
        {"total_usd", round_n(tot_unreal_usd + tot_real_usd, 2)},
        {"pool_usd", POOL}, {"deployed_usd", round_n(deployed, 0)},
        {"n_open", n_open_state},
        {"ndx_basis", NDX_NQ_BASIS}, {"ndx_basis_date", ndx_cash_date},
        {"ndx_mark_src", have_live_ndx ? "IBKR-NQ" : "daily-close"},
        {"clipped", clipped_json}, {"slots", slots}, {"closed", closed}
    };
    std::ofstream sf(STATE);
    sf << out.dump(1);
    sf.close();

    std::printf("refresh: %d new trade-events, open unreal $%.2f (%.2f%%) realized $%.2f\n",
                ntrades, tot_unreal_usd, tot_unreal, tot_real_usd);
    return 0;
}
