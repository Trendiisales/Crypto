// ─────────────────────────────────────────────────────────────────────────────
// upjump_weighting_bt — canonical per-coin BT of the LIVE UpJump ladder book
// (S-2026-07-08 weighting/retirement/improvement session).
//
// LIVE config under test = ChimeraCrypto 7878eac: roster_cfg.csv per-coin
// W/thr window + TIGHT/WIDE tiers, STACKED BASE ARMS {2,4,6}% g50 rev-only,
// self-funding ladder cap 8, reclip 5%, confirm 0, RT 20bp.
//
// Fidelity chain:
//   1. EngineRun drives the REAL live header (../../ChimeraCrypto/include/core/
//      UpJumpLadderCompanion.hpp) exactly like validate_ladder.cpp (deploy rule #4).
//   2. SimBook is a parametric replica used for sweep variants; it is asserted
//      BYTE-EXACT vs EngineRun on the live config for every coin before any
//      variant number is trusted (abort on first mismatch).
//   3. Anchor: roster-sum reproduction of backtest/upjump_concurrent_arms_2026-07-07.txt
//      (cap5 2-tier ≈ +10,283% n=5477; winner ≈ +18,360% n=10895) within the
//      tolerance of the extra data days since 07-07.
//
// Honest metric set (per the 07-08 mirror-extension report lessons):
//   • clips stamped at their ACTUAL bar ts (python stamped parent-exit ts —
//     the XLM/HBAR 2022 mirage); anchor mode reproduces the old stamping.
//   • bear-2022 gate attributed by episode ENTRY year.
//   • ex-best-EPISODE (not just ex-best clip) — reclip ladders concentrate.
//   • 2x-cost = FULL re-sim at 40bp (spawn condition sees the doubled cost).
//   • random-entry control: same episode count+durations, non-overlap, 20 seeds.
//
// Companion book judged STANDALONE (never vs riding WIDE — CompanionDominanceError).
// No BE-floors, no DMA gates. Long-only.
//
// Build: g++ -O2 -std=c++17 -I../../ChimeraCrypto/include upjump_weighting_bt.cpp -o upjump_weighting_bt
// Run:   ./upjump_weighting_bt <anchor|live|caps|jump|secondjump|thr|random> [coins...]
// ─────────────────────────────────────────────────────────────────────────────
#include "core/UpJumpLadderCompanion.hpp"
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <cmath>
#include <fstream>
#include <sstream>
#include <string>
#include <vector>
#include <map>
#include <set>
#include <algorithm>
#include <random>

using chimera::UpJumpLadderCompanion;
static int env_cap(int dflt) {
    const char* e = getenv("UJW_CAP"); return e ? atoi(e) : dflt;
}
static const int64_t TF_MS = 3600LL * 1000;
static const int SMA_BARS = 200 * 24;

struct Bars { std::vector<int64_t> ts; std::vector<double> o, h, l, c, sma; int N = 0; };
struct RCfg { int W = 4; double thr = 0.05, cg = 0, ta = 3, tg = 0.5, wa = 8, wg = 0.5, rc = 0.05; int ts_ = 0, ws = 0, cap = 5; };
struct Clip { int64_t ts; double net_bp; int epi; };
struct Window { int ei, xi; double epx; double jump; };   // jump = j at trigger bar

static std::vector<std::string> split(const std::string& s) {
    std::vector<std::string> out; std::stringstream ss(s); std::string t;
    while (std::getline(ss, t, ',')) out.push_back(t);
    return out;
}

static Bars load(const std::string& coin) {
    // S-2026-07-12: UJW_TF env overrides the bar timeframe (default 1h) so the BE-cascade
    // mimic can be evaluated on DAILY (the winning-config TF), matching the parent BT.
    const char* tfe = getenv("UJW_TF"); std::string tf = tfe ? tfe : "1h";
    Bars b; std::ifstream f("data/" + coin + "USDT_" + tf + ".csv");
    if (!f) { std::fprintf(stderr, "[skip] no %s data for %s\n", tf.c_str(), coin.c_str()); b.N = 0; return b; }
    std::string ln; std::getline(f, ln);
    while (std::getline(f, ln)) { auto v = split(ln); if (v.size() < 5) continue;
        b.ts.push_back((int64_t)std::stoll(v[0])); b.o.push_back(std::stod(v[1])); b.h.push_back(std::stod(v[2])); b.l.push_back(std::stod(v[3])); b.c.push_back(std::stod(v[4])); }
    b.N = (int)b.ts.size(); b.sma.assign(b.N, 0.0); double run = 0;
    for (int i = 0; i < b.N; i++) { run += b.c[i];
        if (i >= SMA_BARS) run -= b.c[i - SMA_BARS];
        if (i >= SMA_BARS - 1) b.sma[i] = run / SMA_BARS; }
    return b;
}

// faithful python parent(): entry/exit next-open, end-flush. i0 = first evaluable bar.
static std::vector<Window> parent(const Bars& b, int W, double thr, int i0 = -1, int i1 = -1) {
    if (i0 < 0) i0 = W; if (i1 < 0) i1 = b.N;
    std::vector<Window> w; bool pos = false; int ei = 0; double epx = 0, jent = 0;
    for (int i = std::max(i0, W); i < i1; i++) {
        double j = b.c[i] / b.c[i - W] - 1.0;
        if (!pos && j >= thr) { int e = i + 1; if (e >= i1) continue; pos = true; ei = e; epx = b.o[e]; jent = j; }
        else if (pos && j <= -thr) { int x = i + 1; if (x >= i1) x = i1 - 1; w.push_back({ei, x, epx, jent}); pos = false; }
    }
    if (pos) w.push_back({ei, i1 - 1, epx, jent});
    return w;
}

// ── SimBook: parametric replica of the live ladder mechanism ────────────────
struct SLeg {
    double epx, le, arm, gb; int sb; double rc; bool open = false, clipped = false;
    double pk = 0, mfe = 0; int64_t ext = 0;
};
struct SimParams {
    RCfg r;                       // tiers/cap/reclip from roster
    bool stack246 = true;         // live stacked arms {2,4,6}% g50 rev-only
    double rt_bp = 20.0;
    int cap_override = -1;        // -1 = roster cap semantics (live uses 8)
    bool second_jump_leg = false; // variant (c): spawn 1 WIDE leg on fresh mid-window re-trigger
};
static SLeg mk_leg(double epx, double arm, int sb, double gb, double rc) {
    SLeg l; l.epx = epx; l.le = epx; l.arm = arm; l.sb = sb; l.gb = gb; l.rc = rc; return l;
}
static bool step_leg(SLeg& lg, int64_t bar, double cur, double& gross) {
    double fav = (cur - lg.epx) / lg.epx * 100.0;
    if (lg.clipped) {
        if (lg.rc > 0 && lg.pk > 0 && fav > lg.pk * (1.0 + lg.rc)) { lg.clipped = false; lg.le = cur; }
        else return false;
    }
    if (!lg.open) { lg.open = true; lg.mfe = fav; lg.ext = bar; }
    if (fav > lg.mfe + 1e-9) { lg.mfe = fav; lg.ext = bar; }
    bool armed = lg.mfe >= lg.arm; int stall = (int)(bar - lg.ext);
    auto clip = [&]() { gross = (cur / lg.le - 1.0) * 1e4; lg.pk = lg.mfe; lg.clipped = true; return true; };
    if (armed && lg.sb > 0 && stall >= lg.sb) return clip();
    if (armed && lg.gb > 0 && fav <= lg.mfe * (1.0 - lg.gb)) return clip();
    return false;
}
// one parent window through the book. Returns clips (ts = actual bar ts).
static void sim_window(const Bars& b, const Window& w, const SimParams& p, int epi,
                       int W, double thr, std::vector<Clip>& out) {
    int cap = (p.cap_override > 0) ? p.cap_override : p.r.cap;
    std::vector<SLeg> legs;
    legs.push_back(mk_leg(w.epx, p.r.ta, p.r.ts_, p.r.tg, p.r.rc));
    legs.push_back(mk_leg(w.epx, p.r.wa, p.r.ws, p.r.wg, p.r.rc));
    if (p.stack246) for (double a : {2.0, 4.0, 6.0}) legs.push_back(mk_leg(w.epx, a, 0, 0.50, p.r.rc));
    for (int i = w.ei; i < w.xi; i++) {
        double cur = b.c[i]; std::vector<SLeg> born;
        for (auto& lg : legs) {
            double g;
            if (step_leg(lg, i, cur, g)) {
                double net = g - p.rt_bp;
                out.push_back({b.ts[i], net, epi});
                if (net > 0 && (int)(legs.size() + born.size()) < cap)
                    born.push_back(mk_leg(cur, p.r.wa, p.r.ws, p.r.wg, p.r.rc));
            }
        }
        // variant (c): fresh up-jump re-trigger mid-window -> one extra WIDE leg here
        if (p.second_jump_leg && i > w.ei && i >= W + 1) {
            double j = b.c[i] / b.c[i - W] - 1.0, jp = b.c[i - 1] / b.c[i - 1 - W] - 1.0;
            if (j >= thr && jp < thr && (int)(legs.size() + born.size()) < cap)
                born.push_back(mk_leg(cur, p.r.wa, p.r.ws, p.r.wg, p.r.rc));
        }
        for (auto& l : born) legs.push_back(l);
    }
    double last = (w.xi - 1 >= w.ei) ? b.c[w.xi - 1] : w.epx;
    for (auto& lg : legs)
        if (lg.open && !lg.clipped)
            out.push_back({b.ts[w.xi - 1], (last / lg.le - 1.0) * 1e4 - p.rt_bp, epi});
}
static std::vector<Clip> sim_book(const Bars& b, const std::vector<Window>& ws,
                                  const SimParams& p, int W, double thr) {
    std::vector<Clip> out; int epi = 0;
    for (auto& w : ws) sim_window(b, w, p, epi++, W, thr, out);
    return out;
}

// ── EngineRun: same book through the REAL live header ───────────────────────
static std::vector<Clip> engine_book(const Bars& b, const std::vector<Window>& ws,
                                     const RCfg& r, double rt_bp) {
    std::vector<Clip> out; int epi = 0;
    for (auto& w : ws) {
        UpJumpLadderCompanion::Config c;
        c.parent_tag = "BT"; c.tag = "BT"; c.symbol = "bt";
        c.tight = {r.ta, r.ts_, r.tg, 0}; c.wide = {r.wa, r.ws, r.wg, 0};
        c.extra_base = { {2.0, 0, 0.50, 0}, {4.0, 0, 0.50, 0}, {6.0, 0, 0.50, 0} };  // live stack246
        c.reclip_pct = r.rc; c.cap = env_cap(8); c.cost_gate_bp = 0; c.confirm_bp = 0;
        c.be_floor = false; c.det_w = 0; c.tf_secs = 3600; c.round_trip_bp = rt_bp;
        UpJumpLadderCompanion eng(c);
        // NOTE: engine fed INDEX-based timestamps (i*TF) exactly like validate_ladder.cpp —
        // the validated mechanism counts stall in BAR STEPS (python bar=i); real-ts feeding
        // diverges across historical data gaps. Real ts kept separately for metric stamping.
        int64_t cur_real_ts = 0;
        eng.set_on_clip([&](const UpJumpLadderCompanion::ClipRecord& rec) {
            (void)rec; out.push_back({cur_real_ts, rec.net_bp_real, epi});
        });
        for (int i = w.ei; i < w.xi; i++) { cur_real_ts = b.ts[i]; eng.observe(true, w.epx, b.c[i], (int64_t)i * TF_MS); }
        int lastix = (w.xi - 1 >= w.ei) ? w.xi - 1 : w.ei;
        double lastpx = (w.xi - 1 >= w.ei) ? b.c[w.xi - 1] : w.epx;
        cur_real_ts = b.ts[lastix];
        eng.observe(false, w.epx, lastpx, (int64_t)lastix * TF_MS);
        epi++;
    }
    return out;
}

// ── STAGGERED-OPEN cells (S-2026-07-11): drive the REAL live header directly with
//    the stagger config. The engine IS ground truth here (net_bp_real), so no SimBook
//    replica is needed — the stagger mechanism lives only in the header. reclip OFF +
//    cap==#tiers (no self-funding ladder) so the BE-cascade "<=1 un-BE'd leg" guarantee
//    holds. arms[] = base tier arm% in open order (tight, wide, then extra_base). ──────
static std::vector<Clip> engine_book_stagger(const Bars& b, const std::vector<Window>& ws,
        const std::vector<double>& arms, int stagger_mode, int stagger_k, double rt_bp) {
    std::vector<Clip> out; int epi = 0;
    for (auto& w : ws) {
        UpJumpLadderCompanion::Config c;
        c.parent_tag = "BT"; c.tag = "BT"; c.symbol = "bt";
        c.tight = {arms[0], 0, 0.50, 0};
        c.wide  = {arms[1], 0, 0.50, 0};
        for (size_t k = 2; k < arms.size(); ++k) c.extra_base.push_back({arms[k], 0, 0.50, 0});
        c.reclip_pct = 0.0;                 // OFF (preserves the stagger guarantee)
        c.cap = (int)arms.size();           // == #base tiers -> no self-funding ladder
        c.cost_gate_bp = 0; c.confirm_bp = 0; c.be_floor = false;
        c.det_w = 0; c.tf_secs = 3600; c.round_trip_bp = rt_bp;
        c.stagger_mode = stagger_mode; c.stagger_k = stagger_k; c.stagger_be_bp = 20.0;
        if (getenv("LOSS_CUT")) c.loss_cut_bp = atof(getenv("LOSS_CUT"));   // cold-loss cut sweep
        UpJumpLadderCompanion eng(c);
        int64_t cur_real_ts = 0;
        eng.set_on_clip([&](const UpJumpLadderCompanion::ClipRecord& rec) { out.push_back({cur_real_ts, rec.net_bp_real, epi}); });
        for (int i = w.ei; i < w.xi; i++) {
            cur_real_ts = b.ts[i];
            // intra-bar: feed the bar LOW first (a within-bar tick) so the hard reversal cut
            // can fire at the stop, THEN the close (drives detection + giveback/reversal).
            if (c.loss_cut_bp > 0.0 && i < (int)b.l.size()) eng.stop_check_only(b.l[i], (int64_t)i * TF_MS);  // bar low tests the stop
            eng.observe(true, w.epx, b.c[i], (int64_t)i * TF_MS);
        }
        int lastix = (w.xi - 1 >= w.ei) ? w.xi - 1 : w.ei;
        double lastpx = (w.xi - 1 >= w.ei) ? b.c[w.xi - 1] : w.epx;
        cur_real_ts = b.ts[lastix];
        eng.observe(false, w.epx, lastpx, (int64_t)lastix * TF_MS);
        epi++;
    }
    return out;
}

// ── metrics ──────────────────────────────────────────────────────────────────
struct Met {
    int n = 0; double net = 0, pf = 0, h1 = 0, h2 = 0, y2022 = 0; int n2022 = 0;
    double exbest_clip = 0, exbest_epi = 0, best_epi = 0, best_epi_frac = 0;
    double maxdd_bp = 0;  // peak-to-trough of the cumulative net_bp curve (clip order)
    bool pass6 = false;
};
static int year_of(int64_t ms) { time_t s = (time_t)(ms / 1000); struct tm g; gmtime_r(&s, &g); return g.tm_year + 1900; }
static Met metrics(std::vector<Clip> rows, const std::vector<Window>& ws, const Bars& b,
                   double net_2x /*book net at 40bp full re-sim, pct*/) {
    Met m; if (rows.empty()) return m;
    std::sort(rows.begin(), rows.end(), [](const Clip& a, const Clip& c2) { return a.ts < c2.ts; });
    double gw = 0, gl = 0, cum = 0, peak = 0;
    std::map<int, double> epi_net;
    for (auto& r : rows) {
        m.net += r.net_bp / 100.0;
        if (r.net_bp > 0) gw += r.net_bp; else gl -= r.net_bp;
        epi_net[r.epi] += r.net_bp;
        cum += r.net_bp; if (cum > peak) peak = cum;
        if (peak - cum > m.maxdd_bp) m.maxdd_bp = peak - cum;
    }
    m.n = (int)rows.size();
    m.pf = gl > 0 ? gw / gl : (gw > 0 ? 999 : 0);
    int64_t mid = rows[rows.size() / 2].ts;
    for (auto& r : rows) { if (r.ts < mid) m.h1 += r.net_bp / 100.0; else m.h2 += r.net_bp / 100.0; }
    // bear-2022 by episode ENTRY year (honest attribution)
    for (auto& kv : epi_net) {
        int y = year_of(b.ts[ws[kv.first].ei]);
        if (y == 2022) { m.y2022 += kv.second / 100.0; m.n2022++; }
    }
    double best_clip = 0; for (auto& r : rows) best_clip = std::max(best_clip, r.net_bp);
    m.exbest_clip = m.net - best_clip / 100.0;
    for (auto& kv : epi_net) if (kv.second > m.best_epi) m.best_epi = kv.second;
    m.exbest_epi = m.net - m.best_epi / 100.0;
    m.best_epi_frac = (m.net != 0) ? (m.best_epi / 100.0) / m.net : 0;
    m.pass6 = (m.net > 0) && (m.pf >= 1.3) && (m.h1 > 0) && (m.h2 > 0)
              && (m.n2022 == 0 || m.y2022 > 0) && (m.exbest_epi > 0) && (net_2x > 0);
    return m;
}
static double book_net(const std::vector<Clip>& rows) {
    double s = 0; for (auto& r : rows) s += r.net_bp / 100.0; return s;
}

static std::map<std::string, RCfg> load_roster() {
    std::map<std::string, RCfg> roster; std::ifstream f("roster_cfg.csv"); std::string ln; std::getline(f, ln);
    while (std::getline(f, ln)) { auto v = split(ln); if (v.size() < 12) continue;
        RCfg c; c.W = std::stoi(v[1]); c.thr = std::stod(v[2]); c.cg = std::stod(v[3]);
        c.ta = std::stod(v[4]); c.ts_ = (int)std::stod(v[5]); c.tg = std::stod(v[6]);
        c.wa = std::stod(v[7]); c.ws = (int)std::stod(v[8]); c.wg = std::stod(v[9]);
        c.rc = std::stod(v[10]); c.cap = std::stoi(v[11]); roster[v[0]] = c; }
    return roster;
}
static const std::vector<std::string> LIVE_COINS =
    { "BTC", "ETH", "SOL", "DOGE", "BNB", "ADA", "TRX", "NEAR" };

// byte-exact gate: SimBook vs live engine on the LIVE config, every coin.
static void parity_gate(const std::map<std::string, Bars>& B, const std::map<std::string, RCfg>& roster,
                        const std::vector<std::string>& coins) {
    for (auto& coin : coins) {
        const Bars& b = B.at(coin); const RCfg& r = roster.at(coin);
        auto ws = parent(b, r.W, r.thr);
        SimParams p; p.r = r; p.cap_override = env_cap(8);
        auto sc = sim_book(b, ws, p, r.W, r.thr);
        auto ec = engine_book(b, ws, r, 20.0);
        if (sc.size() != ec.size()) { std::fprintf(stderr, "PARITY FAIL %s: n %zu vs %zu\n", coin.c_str(), sc.size(), ec.size()); exit(2); }
        for (size_t i = 0; i < sc.size(); i++)
            if (std::fabs(sc[i].net_bp - ec[i].net_bp) > 1e-6 || sc[i].epi != ec[i].epi) {
                std::fprintf(stderr, "PARITY FAIL %s clip %zu: %.6f vs %.6f\n", coin.c_str(), i, sc[i].net_bp, ec[i].net_bp); exit(2); }
        std::printf("[PARITY] %-4s SimBook == live engine, %zu clips byte-exact\n", coin.c_str(), sc.size());
    }
}

int main(int argc, char** argv) {
    std::string mode = argc > 1 ? argv[1] : "live";
    std::vector<std::string> coins = LIVE_COINS;
    if (argc > 2) { coins.clear(); for (int i = 2; i < argc; i++) coins.push_back(argv[i]); }
    auto roster = load_roster();
    std::map<std::string, Bars> B;
    for (auto& c : coins) B[c] = load(c);

    if (mode == "anchor") {
        // Reproduce upjump_concurrent_arms_2026-07-07.txt roster sums (tolerance = extra days).
        // Uses the OLD python clip stamping (parent-exit ts) + 200d-regime split for like-for-like.
        for (bool stack : {false, true}) {
            int cap = stack ? 8 : 5; double tot = 0; int n = 0;
            for (auto& coin : coins) {
                const Bars& b = B[coin]; const RCfg& r = roster[coin];
                auto ws = parent(b, r.W, r.thr);
                SimParams p; p.r = r; p.stack246 = stack; p.cap_override = cap;
                auto rows = sim_book(b, ws, p, r.W, r.thr);
                tot += book_net(rows); n += (int)rows.size();
            }
            std::printf("ANCHOR %-22s n=%6d net=%+8.0f%%   (07-07 published: %s)\n",
                stack ? "roster+stack246 cap8" : "2tier cap5 (roster)", n, tot,
                stack ? "n=10895 net=+18360%" : "n=5477 net=+10283%");
        }
        parity_gate(B, roster, coins);
        return 0;
    }

    if (mode == "live") {
        parity_gate(B, roster, coins);
        std::printf("\nPER-COIN LIVE CONFIG (roster tiers + stack246 + cap8, reclip5%%, RT20bp), honest metric set\n");
        std::printf("%-5s %5s %8s %6s %8s %8s %9s %10s %10s %8s %8s %6s\n",
            "coin", "n", "net%", "PF", "H1", "H2", "y2022", "exbestEpi", "bestEpi%", "2xcost", "maxDDbp", "all6");
        for (auto& coin : coins) {
            const Bars& b = B[coin]; const RCfg& r = roster[coin];
            auto ws = parent(b, r.W, r.thr);
            SimParams p; p.r = r; p.cap_override = env_cap(8);
            auto rows = sim_book(b, ws, p, r.W, r.thr);
            SimParams p2 = p; p2.rt_bp = 40.0;
            double n2x = book_net(sim_book(b, ws, p2, r.W, r.thr));
            Met m = metrics(rows, ws, b, n2x);
            std::printf("%-5s %5d %+8.0f %6.2f %+8.0f %+8.0f %+7.0f/%d %+10.0f %9.0f%% %+8.0f %8.0f %6s\n",
                coin.c_str(), m.n, m.net, m.pf, m.h1, m.h2, m.y2022, m.n2022, m.exbest_epi,
                m.best_epi_frac * 100, n2x, m.maxdd_bp, m.pass6 ? "PASS" : "FAIL");
        }
        return 0;
    }

    if (mode == "years") {
        // per-coin, per-EPISODE-ENTRY-year net (honest attribution) under the live config.
        std::printf("PER-YEAR NET%% (episode entry-year attribution, live config)\n");
        std::printf("%-5s", "coin");
        for (int y = 2021; y <= 2026; y++) std::printf(" %8d", y);
        std::printf("\n");
        for (auto& coin : coins) {
            const Bars& b = B[coin]; const RCfg& r = roster[coin];
            auto ws = parent(b, r.W, r.thr);
            SimParams p; p.r = r; p.cap_override = env_cap(8);
            auto rows = sim_book(b, ws, p, r.W, r.thr);
            std::map<int, double> epi_net; for (auto& c : rows) epi_net[c.epi] += c.net_bp / 100.0;
            std::map<int, double> ynet;
            for (auto& kv : epi_net) ynet[year_of(b.ts[ws[kv.first].ei])] += kv.second;
            std::printf("%-5s", coin.c_str());
            for (int y = 2021; y <= 2026; y++) std::printf(" %+8.0f", ynet.count(y) ? ynet[y] : 0.0);
            std::printf("\n");
        }
        return 0;
    }

    if (mode == "caps") {
        std::printf("CAP SWEEP (live tiers+stack246; ladder depth cap), net%% @20bp | @40bp | pass6\n");
        std::printf("%-5s %14s %14s %14s %14s\n", "coin", "cap5", "cap8(live)", "cap10", "cap12");
        for (auto& coin : coins) {
            const Bars& b = B[coin]; const RCfg& r = roster[coin];
            auto ws = parent(b, r.W, r.thr);
            std::printf("%-5s", coin.c_str());
            for (int cap : {5, 8, 10, 12}) {
                SimParams p; p.r = r; p.cap_override = cap;
                auto rows = sim_book(b, ws, p, r.W, r.thr);
                SimParams p2 = p; p2.rt_bp = 40.0;
                double n2x = book_net(sim_book(b, ws, p2, r.W, r.thr));
                Met m = metrics(rows, ws, b, n2x);
                std::printf(" %+7.0f|%s", m.net, m.pass6 ? "P" : "F");
            }
            std::printf("\n");
        }
        return 0;
    }

    if (mode == "jump") {
        std::printf("JUMP-SIZE MONOTONICITY: episode net (mean %% per window) by entry-jump tercile (j/thr)\n");
        std::printf("%-5s %22s %22s %22s  monotone?\n", "coin", "T1(small)", "T2", "T3(big)");
        for (auto& coin : coins) {
            const Bars& b = B[coin]; const RCfg& r = roster[coin];
            auto ws = parent(b, r.W, r.thr);
            SimParams p; p.r = r; p.cap_override = env_cap(8);
            auto rows = sim_book(b, ws, p, r.W, r.thr);
            std::map<int, double> epi_net;
            for (auto& c : rows) epi_net[c.epi] += c.net_bp / 100.0;
            std::vector<std::pair<double, double>> jn;   // (j/thr, episode net)
            for (size_t e = 0; e < ws.size(); e++) jn.push_back({ws[e].jump / r.thr, epi_net.count((int)e) ? epi_net[(int)e] : 0.0});
            std::sort(jn.begin(), jn.end());
            size_t t = jn.size() / 3; double m1 = 0, m2 = 0, m3 = 0; size_t n1 = t, n2 = t, n3 = jn.size() - 2 * t;
            for (size_t i = 0; i < jn.size(); i++) { if (i < t) m1 += jn[i].second; else if (i < 2 * t) m2 += jn[i].second; else m3 += jn[i].second; }
            m1 /= std::max<size_t>(n1, 1); m2 /= std::max<size_t>(n2, 1); m3 /= std::max<size_t>(n3, 1);
            std::printf("%-5s %14.1f (n=%3zu) %14.1f (n=%3zu) %14.1f (n=%3zu)  %s\n",
                coin.c_str(), m1, n1, m2, n2, m3, n3,
                (m3 > m2 && m2 > m1) ? "YES" : (m3 > m1 ? "weak" : "NO"));
        }
        return 0;
    }

    if (mode == "secondjump") {
        std::printf("SECOND-JUMP LEG (extra WIDE leg on fresh mid-window re-trigger), vs live\n");
        std::printf("%-5s %16s %16s %10s\n", "coin", "live net%|pass", "2ndjump net%|pass", "delta");
        for (auto& coin : coins) {
            const Bars& b = B[coin]; const RCfg& r = roster[coin];
            auto ws = parent(b, r.W, r.thr);
            SimParams p; p.r = r; p.cap_override = env_cap(8);
            auto base = sim_book(b, ws, p, r.W, r.thr);
            SimParams pv = p; pv.second_jump_leg = true;
            auto var = sim_book(b, ws, pv, r.W, r.thr);
            SimParams p2 = pv; p2.rt_bp = 40.0;
            double n2x = book_net(sim_book(b, ws, p2, r.W, r.thr));
            Met mb = metrics(base, ws, b, 1), mv = metrics(var, ws, b, n2x);
            std::printf("%-5s %+11.0f|%s %+12.0f|%s %+10.0f\n", coin.c_str(),
                mb.net, mb.pf >= 1.3 ? "P" : "F", mv.net, mv.pass6 ? "P" : "F", mv.net - mb.net);
        }
        return 0;
    }

    if (mode == "thr") {
        // per-coin W/thr retune on 2024-01-01 -> now, vs shipped. Plateau check.
        int64_t T0 = 1704067200000LL + 31536000000LL;   // 2024-01-01 UTC ms
        std::printf("THR RETUNE 2024->now (live tiers+stack246+cap8), net%% per (W,thr); * = shipped\n");
        for (auto& coin : coins) {
            const Bars& b = B[coin]; const RCfg& r = roster[coin];
            int i0 = 0; while (i0 < b.N && b.ts[i0] < T0) i0++;
            std::printf("%-5s (shipped W=%d thr=%.0f%%)\n", coin.c_str(), r.W, r.thr * 100);
            for (int W : {4, 6, 8}) {
                std::printf("  W=%d:", W);
                for (double thr : {0.03, 0.05, 0.08, 0.10, 0.12, 0.15}) {
                    auto ws = parent(b, W, thr, i0 + W);
                    SimParams p; p.r = r; p.cap_override = env_cap(8);
                    auto rows = sim_book(b, ws, p, W, thr);
                    double net = book_net(rows);
                    std::printf(" %s%.0f%%:%+.0f", (W == r.W && std::fabs(thr - r.thr) < 1e-9) ? "*" : "", thr * 100, net);
                }
                std::printf("\n");
            }
        }
        return 0;
    }

    if (mode == "random") {
        // random-entry control: 20 seeds, same episode count+durations, non-overlapping.
        std::printf("RANDOM-ENTRY CONTROL (20 seeds, same episode count+durations, live config)\n");
        std::printf("%-5s %10s %12s %10s %8s\n", "coin", "actual%", "rand mean%", "rand sd", "z");
        for (auto& coin : coins) {
            const Bars& b = B[coin]; const RCfg& r = roster[coin];
            auto ws = parent(b, r.W, r.thr);
            SimParams p; p.r = r; p.cap_override = env_cap(8);
            double actual = book_net(sim_book(b, ws, p, r.W, r.thr));
            std::vector<int> durs; for (auto& w : ws) durs.push_back(w.xi - w.ei);
            std::vector<double> nets;
            for (int seed = 0; seed < 20; seed++) {
                std::mt19937 rng(1000 + seed);
                std::vector<int> d = durs; std::shuffle(d.begin(), d.end(), rng);
                std::vector<std::pair<int, int>> placed; std::vector<Window> rws;
                for (int dur : d) {
                    for (int tries = 0; tries < 200; tries++) {
                        int lo = r.W + 1, hi = b.N - 2 - dur; if (hi <= lo) break;
                        int ei = lo + (int)(rng() % (uint64_t)(hi - lo));
                        bool ok = true;
                        for (auto& pl : placed) if (ei < pl.second && ei + dur > pl.first) { ok = false; break; }
                        if (ok) { placed.push_back({ei, ei + dur}); rws.push_back({ei, ei + dur, b.o[ei], r.thr}); break; }
                    }
                }
                nets.push_back(book_net(sim_book(b, rws, p, r.W, r.thr)));
            }
            double mu = 0; for (double v : nets) mu += v; mu /= nets.size();
            double sd = 0; for (double v : nets) sd += (v - mu) * (v - mu); sd = std::sqrt(sd / nets.size());
            std::printf("%-5s %+10.0f %+12.0f %10.0f %8.1f\n", coin.c_str(), actual, mu, sd, sd > 0 ? (actual - mu) / sd : 0);
        }
        return 0;
    }

    if (mode == "ext") {
        // COIN-EXTENSION check (committed form of the 07-08 study): for each coin
        // (arg list, e.g. AAVE OP or any new *_1h.csv dropped into data/), sweep the
        // documented parent menu W{4,6,8} x thr{5,8,12}%, take best-net parent whose
        // PARENT book passes (net>0, PF>1, both halves>0), then run the MODAL live
        // companion tiers (tight a3/s0/g50, wide a8/s0/g50, stack246, cap8) through
        // the honest all-6. No per-coin tier tuning (no fresh overfit).
        for (auto& coin : coins) {
            const Bars& b = B[coin];
            double bestnet = -1e18; int bW = 0; double bthr = 0;
            for (int W : {4, 6, 8}) for (double thr : {0.05, 0.08, 0.12}) {
                auto ws = parent(b, W, thr);
                if (ws.size() < 10) continue;
                double net = 0, gw = 0, gl = 0, h1 = 0, h2 = 0;
                size_t half = ws.size() / 2;
                for (size_t k = 0; k < ws.size(); k++) {
                    double xpx = b.c[ws[k].xi - 1];
                    double n = (xpx / ws[k].epx - 1.0) * 1e4 - 20.0;
                    net += n; if (n > 0) gw += n; else gl -= n;
                    if (k < half) h1 += n; else h2 += n;
                }
                bool ok = net > 0 && (gl <= 0 || gw / gl > 1.0) && h1 > 0 && h2 > 0;
                if (ok && net > bestnet) { bestnet = net; bW = W; bthr = thr; }
            }
            if (bW == 0) { std::printf("%-5s EXT: no parent W/thr passes -> NOT a candidate\n", coin.c_str()); continue; }
            auto ws = parent(b, bW, bthr);
            RCfg r; r.W = bW; r.thr = bthr; r.ta = 3; r.ts_ = 0; r.tg = 0.5; r.wa = 8; r.ws = 0; r.wg = 0.5; r.rc = 0.05;
            SimParams p; p.r = r; p.cap_override = env_cap(8);
            auto rows = sim_book(b, ws, p, r.W, r.thr);
            SimParams p2 = p; p2.rt_bp = 40.0;
            double n2x = book_net(sim_book(b, ws, p2, r.W, r.thr));
            Met m = metrics(rows, ws, b, n2x);
            std::printf("%-5s EXT parent %dh/%+.0f%% (%zu windows): n=%d net=%+.0f%% PF=%.2f H1=%+.0f H2=%+.0f "
                        "y2022=%+.0f/%d exbestEpi=%+.0f 2x=%+.0f -> %s\n",
                coin.c_str(), bW, bthr * 100, ws.size(), m.n, m.net, m.pf, m.h1, m.h2,
                m.y2022, m.n2022, m.exbest_epi, n2x, m.pass6 ? "CANDIDATE (needs random-control + operator review)" : "FAIL all-6");
        }
        return 0;
    }

    if (mode == "early") {
        // EARLIER-ARM SWEEP (operator "arm too late; enter the minute it goes positive").
        // Vary ONLY the self-detect window/threshold to earlier values; keep the NO-FLOOR
        // giveback ladder tiers (roster arm/stall/gb/reclip/cap) — the only real-fill-viable
        // trail family (S-2026-07-07f/v). Real column = SimBook net_bp (== live net_bp_real,
        // parity-gated). Honest all-6 metric set. Full history 2021->2026-07.
        // UJW_LOWARM=1 also floors the tier arms (tight 0.2%/wide 0.5%) = literal "arm at BE".
        bool lowarm = getenv("UJW_LOWARM") && atoi(getenv("UJW_LOWARM")) != 0;
        parity_gate(B, roster, coins);
        std::printf("\nEARLY-ARM SWEEP  detect W{1,2,4}h x thr{0.3,0.5,1,2%%}  (no-floor ladder, roster tiers%s, RT20bp)\n",
                    lowarm ? " + LOWARM tight0.2/wide0.5" : "");
        std::printf("  real column (net_bp_real, parity-gated); honest all-6 = net>0,PF>=1.3,H1>0,H2>0,y2022>=0,exBestEpi>0,2xcost>0\n");
        std::printf("%-5s %5s %6s %8s %6s %8s %8s %8s %9s %8s %6s\n",
            "coin", "det", "n", "net%", "PF", "H1", "H2", "y2022", "exBestEpi", "2xcost", "all6");
        const int Ws[] = {1, 2, 4};
        const double thrs[] = {0.003, 0.005, 0.01, 0.02, 0.03, 0.04};
        for (auto& coin : coins) {
            const Bars& b = B[coin]; RCfg r0 = roster[coin];
            // reference: shipped roster row
            {
                auto ws = parent(b, r0.W, r0.thr);
                SimParams p; p.r = r0; p.cap_override = env_cap(8);
                auto rows = sim_book(b, ws, p, r0.W, r0.thr);
                SimParams p2 = p; p2.rt_bp = 40.0; double n2x = book_net(sim_book(b, ws, p2, r0.W, r0.thr));
                Met m = metrics(rows, ws, b, n2x);
                std::printf("%-5s %2dh/%2.0f%% %6d %+8.0f %6.2f %+8.0f %+8.0f %+6.0f/%d %+9.0f %+8.0f %6s  <-shipped\n",
                    coin.c_str(), r0.W, r0.thr * 100, m.n, m.net, m.pf, m.h1, m.h2, m.y2022, m.n2022, m.exbest_epi, n2x, m.pass6 ? "PASS" : "FAIL");
            }
            for (int W : Ws) for (double thr : thrs) {
                RCfg r = r0; r.W = W; r.thr = thr;
                if (lowarm) { r.ta = 0.2; r.wa = 0.5; }
                auto ws = parent(b, W, thr);
                if (ws.size() < 10) continue;
                SimParams p; p.r = r; p.cap_override = env_cap(8);
                auto rows = sim_book(b, ws, p, W, thr);
                SimParams p2 = p; p2.rt_bp = 40.0; double n2x = book_net(sim_book(b, ws, p2, W, thr));
                Met m = metrics(rows, ws, b, n2x);
                std::printf("%-5s %2dh/%3.1f%% %6d %+8.0f %6.2f %+8.0f %+8.0f %+6.0f/%d %+9.0f %+8.0f %6s\n",
                    coin.c_str(), W, thr * 100, m.n, m.net, m.pf, m.h1, m.h2, m.y2022, m.n2022, m.exbest_epi, n2x, m.pass6 ? "PASS" : "FAIL");
            }
        }
        return 0;
    }

    if (mode == "stagger") {
        // Two staggered-open cells (S-2026-07-11, operator full roster), driven through
        // the REAL live header. Long-only, no 200DMA. Corrected long-only gate: net>0,
        // PF>=1.3, both WF halves>0, 2x-cost>0 (2022-bear NOT gated — long-only spot can't
        // short a crash). reclip OFF, cap==#tiers (no ladder). RT 20bp / 2x = 40bp full re-sim.
        std::printf("STAGGERED-OPEN CELLS (real engine header, net_bp_real; reclip OFF, cap=#tiers no-ladder)\n");
        std::printf("  long-only gate: net>0, PF>=1.3, both WF halves>0, 2x-cost>0 (2022-bear NOT gated)\n");
        std::printf("%-5s %-11s %6s %8s %6s %8s %8s %9s %10s %8s %8s %6s\n",
            "coin", "stagger", "n", "net%", "PF", "H1", "H2", "y2022", "exBestEpi", "2xcost", "maxDDbp", "gate");
        struct SC { std::string coin; int W; double thr; std::vector<double> arms; int mode; int k; std::string name; };
        std::vector<SC> cells = {
            {"ETH", 1, 0.02, {0.2, 2, 3, 4, 6, 8},  1, 0, "BE-CASC-N6"},          // {BE,+2,+3,+4,+6,+8}
            {"BTC", 2, 0.04, {3, 4, 6, 8, 10, 12},  1, 0, "BE-CASC-N6"},          // arm>=3% tiers
            {"BNB", 1, 0.03, {3, 4, 6, 8, 10, 12, 14, 16}, 1, 0, "BE-CASC-N8"},   // N=8 arm ladder
            {"SOL", 1, 0.05, {0.2, 2, 3, 4, 6, 8, 10, 12}, 1, 0, "BE-CASC-N8"},   // N=8 {BE..+12}
        };
        for (auto& sc : cells) {
            if (!B.count(sc.coin)) B[sc.coin] = load(sc.coin);
            const Bars& b = B[sc.coin];
            auto ws = parent(b, sc.W, sc.thr);
            // baseline: SAME tiers, NO stagger (all legs open at window entry) — shows the DD/net ratio.
            for (int md : {0, sc.mode}) {
                int kk = (md == 0) ? 0 : sc.k;
                auto rows = engine_book_stagger(b, ws, sc.arms, md, kk, 20.0);
                double n2x = book_net(engine_book_stagger(b, ws, sc.arms, md, kk, 40.0));
                Met m = metrics(rows, ws, b, n2x);
                bool gate = (m.net > 0) && (m.pf >= 1.3) && (m.h1 > 0) && (m.h2 > 0) && (n2x > 0);
                std::string nm = (md == 0) ? "all-open" : sc.name;
                std::printf("%-5s %-11s %6d %+8.0f %6.2f %+8.0f %+8.0f %+6.0f/%d %+10.0f %+8.0f %8.0f %6s\n",
                    sc.coin.c_str(), nm.c_str(), m.n, m.net, m.pf, m.h1, m.h2, m.y2022, m.n2022,
                    m.exbest_epi, n2x, m.maxdd_bp, gate ? "PASS" : "FAIL");
            }
        }
        return 0;
    }

    if (mode == "grid") {
        // THRESHOLD-COMPARISON GRID (S-2026-07-11, operator): per coin, 4 shadow cells at
        // thr {2,3,4,5%}, SAME BE-cascade mimic + detect window, so live real-fills pick the
        // winner. Same corrected long-only gate. Each cell driven through THIS header.
        std::printf("THRESHOLD GRID — per coin x thr{2,3,4,5%%}, BE-CASCADE mimic (net_bp_real, reclip OFF)\n");
        std::printf("%-5s %-7s %5s %6s %8s %6s %8s %8s %8s %8s %8s %5s\n",
            "coin", "family", "thr", "n", "net%", "PF", "H1", "H2", "2xcost", "maxDDbp", "y2022", "gate");
        struct GC { std::string coin; int W; std::vector<double> arms; std::string fam; };
        std::vector<GC> gcoins = {
            {"ETH", 1, {0.2,2,3,4,6,8},          "BE-N6"},
            {"BTC", 2, {3,4,6,8,10,12},          "a3-N6"},
            {"BNB", 1, {3,4,6,8,10,12,14,16},    "a3-N8"},
            {"SOL", 1, {0.2,2,3,4,6,8,10,12},    "BE-N8"},
            {"DOGE",4, {0.2,2,3,4,6,8},          "BE-N6"},
            {"ADA", 1, {0.2,2,3,4,6,8},          "BE-N6"},
            {"XRP", 1, {0.2,2,3,4,6,8},          "BE-N6"},
            {"TRX", 1, {0.2,2,3,4,6,8},          "BE-N6"},
            // S-2026-07-12 universe up-jump winners — BE-cascade mimic eval at the parent's
            // WINNING window (W in bars; run with UJW_TF=1d so W = days). BCH W=48, rest W=24.
            {"NEAR",24, {0.2,2,3,4,6,8},         "BE-N6"},
            {"AVAX",24, {0.2,2,3,4,6,8},         "BE-N6"},
            {"LINK",24, {0.2,2,3,4,6,8},         "BE-N6"},
            {"BCH", 48, {0.2,2,3,4,6,8},         "BE-N6"},
            {"UNI", 24, {0.2,2,3,4,6,8},         "BE-N6"},
            {"LDO", 24, {0.2,2,3,4,6,8},         "BE-N6"},
        };
        for (auto& gc : gcoins) {
            if (!B.count(gc.coin)) B[gc.coin] = load(gc.coin);
            const Bars& b = B[gc.coin];
            for (double thr : {0.02, 0.03, 0.04, 0.05}) {
                auto ws = parent(b, gc.W, thr);
                if (ws.size() < 5) { std::printf("%-5s %-7s %4.0f%%   (too few windows)\n", gc.coin.c_str(), gc.fam.c_str(), thr*100); continue; }
                auto rows = engine_book_stagger(b, ws, gc.arms, 1, 0, 20.0);
                double n2x = book_net(engine_book_stagger(b, ws, gc.arms, 1, 0, 40.0));
                Met m = metrics(rows, ws, b, n2x);
                bool gate = (m.net > 0) && (m.pf >= 1.3) && (m.h1 > 0) && (m.h2 > 0) && (n2x > 0);
                std::printf("%-5s %-7s %4.0f%% %6d %+8.0f %6.2f %+8.0f %+8.0f %+8.0f %8.0f %+6.0f %5s\n",
                    gc.coin.c_str(), gc.fam.c_str(), thr*100, m.n, m.net, m.pf, m.h1, m.h2, n2x, m.maxdd_bp, m.y2022,
                    gate ? "PASS" : "-");
            }
        }
        return 0;
    }

    if (mode == "xsgrid") {
        // CROSS-ASSET BE-CASCADE GRID (S-2026-07-12b): port test of the crypto up-jump
        // BE-cascade mimic onto GOLD + STOCK INDICES (operator request,
        // SESSION_HANDOFF_2026-07-12a). Same live-header mechanism (engine_book_stagger
        // = the REAL UpJumpLadderCompanion), long-only, no DMA gates.
        // Per-symbol REAL round-trip cost: XAU ~5bp RT (IBKR 2*0.015%+spread),
        // index CFD/fut ~3bp — crypto's 20bp would strawman-kill the port.
        // Gate row = OOS episodes entering >=2023 (fair long-only window, bear omitted);
        // FULL period printed alongside so the bleed is visible, never hidden.
        // Parent ride-WIDE shown side-by-side: ADDITIVE books, never a dominance test
        // (CompanionDominanceError).
        std::printf("XS GRID — gold+indices BE-cascade mimic (live header, real costs), UJW_TF=%s\n",
                    getenv("UJW_TF") ? getenv("UJW_TF") : "1h");
        std::printf("%-5s %3s %4s | %4s %8s %6s | %4s %8s %6s %8s %8s %8s %8s %8s | %4s %8s %8s %6s %8s %5s\n",
            "sym", "W", "thr", "nwF", "parF%", "ppfF", "nF", "netF%", "pfF", "H1", "H2", "2xF", "y22", "ddbp",
            "nwO", "parO%", "netO%", "pfO", "2xO", "gate");
        struct XS { std::string sym; double rt; };
        std::vector<XS> syms = { {"XAU", 5.0}, {"SPX", 3.0}, {"DJ30", 3.0}, {"NDX", 3.0} };
        std::vector<double> arms = {0.2, 2, 3, 4, 6, 8};   // BE-N6 (crypto winner family)
        for (auto& s : syms) {
            if (!B.count(s.sym)) B[s.sym] = load(s.sym);
            const Bars& b = B[s.sym]; if (!b.N) continue;
            for (int W : {5, 10, 24}) {
                for (double thr : {0.02, 0.03, 0.04, 0.05}) {
                    auto ws = parent(b, W, thr);
                    if (ws.size() < 5) continue;
                    // parent ride-WIDE book: entry next-open epx, exit next-open
                    auto par_book = [&](const std::vector<Window>& v, double& pf) {
                        double pw = 0, pl = 0, net = 0;
                        for (auto& w : v) {
                            int x = std::min(w.xi, b.N - 1);
                            double r_ = (b.o[x] / w.epx - 1.0) * 100.0 - s.rt / 100.0;
                            net += r_; if (r_ > 0) pw += r_; else pl -= r_;
                        }
                        pf = pl > 0 ? pw / pl : (pw > 0 ? 999 : 0);
                        return net;
                    };
                    double ppfF = 0; double parF = par_book(ws, ppfF);
                    auto rowsF = engine_book_stagger(b, ws, arms, 1, 0, s.rt);
                    double n2xF = book_net(engine_book_stagger(b, ws, arms, 1, 0, 2 * s.rt));
                    Met mF = metrics(rowsF, ws, b, n2xF);
                    // OOS gate subset: episodes ENTERING >= 2023
                    std::vector<Window> wsO;
                    for (auto& w : ws) if (year_of(b.ts[w.ei]) >= 2023) wsO.push_back(w);
                    double netO = 0, pfO = 0, n2xO = 0, parO = 0, ppfO = 0; int nO = 0; bool gate = false;
                    if (wsO.size() >= 5) {
                        parO = par_book(wsO, ppfO);
                        auto rowsO = engine_book_stagger(b, wsO, arms, 1, 0, s.rt);
                        n2xO = book_net(engine_book_stagger(b, wsO, arms, 1, 0, 2 * s.rt));
                        Met mO = metrics(rowsO, wsO, b, n2xO);
                        netO = mO.net; pfO = mO.pf; nO = mO.n;
                        gate = (mO.net > 0) && (mO.pf >= 1.3) && (mO.h1 > 0) && (mO.h2 > 0) && (n2xO > 0);
                    }
                    std::printf("%-5s %3d %3.0f%% | %4zu %+8.1f %6.2f | %4d %+8.1f %6.2f %+8.1f %+8.1f %+8.1f %+8.1f %8.0f | %4d %+8.1f %+8.1f %6.2f %+8.1f %5s\n",
                        s.sym.c_str(), W, thr * 100, ws.size(), parF, ppfF,
                        mF.n, mF.net, mF.pf, mF.h1, mF.h2, n2xF, mF.y2022, mF.maxdd_bp,
                        nO, parO, netO, pfO, n2xO, gate ? "PASS" : "-");
                }
            }
        }
        return 0;
    }

    if (mode == "xsrandom") {
        // Beta control for xsgrid candidate cells: same episode count+durations placed
        // at RANDOM entries (non-overlap, 20 seeds), OOS>=2023 window set, through the
        // same live-header stagger book. Distinguishes up-jump edge from long-only beta
        // (indices quadrupled 2016-26 — any long book prints without this check).
        struct XC { std::string sym; int W; double thr, rt; };
        std::vector<XC> cells = { {"XAU",10,0.02,5.0}, {"SPX",10,0.03,3.0},
                                  {"DJ30",10,0.04,3.0}, {"NDX",10,0.02,3.0} };
        std::vector<double> arms = {0.2, 2, 3, 4, 6, 8};
        std::printf("%-5s %10s %12s %10s %8s\n", "sym", "actual%", "random_mu%", "sd", "z");
        for (auto& c : cells) {
            if (!B.count(c.sym)) B[c.sym] = load(c.sym);
            const Bars& b = B[c.sym]; if (!b.N) continue;
            auto wsAll = parent(b, c.W, c.thr);
            std::vector<Window> ws;
            for (auto& w : wsAll) if (year_of(b.ts[w.ei]) >= 2023) ws.push_back(w);
            if (ws.size() < 5) continue;
            double actual = book_net(engine_book_stagger(b, ws, arms, 1, 0, c.rt));
            std::vector<int> durs; for (auto& w : ws) durs.push_back(w.xi - w.ei);
            int lo0 = c.W + 1;
            while (lo0 < b.N && year_of(b.ts[lo0]) < 2023) lo0++;   // random placement inside the SAME OOS window
            std::vector<double> nets;
            for (int seed = 0; seed < 20; seed++) {
                std::mt19937 rng(1000 + seed);
                std::vector<int> d = durs; std::shuffle(d.begin(), d.end(), rng);
                std::vector<std::pair<int, int>> placed; std::vector<Window> rws;
                for (int dur : d) {
                    for (int tries = 0; tries < 200; tries++) {
                        int lo = lo0, hi = b.N - 2 - dur; if (hi <= lo) break;
                        int ei = lo + (int)(rng() % (uint64_t)(hi - lo));
                        bool ok = true;
                        for (auto& pl : placed) if (ei < pl.second && ei + dur > pl.first) { ok = false; break; }
                        if (ok) { placed.push_back({ei, ei + dur}); rws.push_back({ei, ei + dur, b.o[ei], c.thr}); break; }
                    }
                }
                nets.push_back(book_net(engine_book_stagger(b, rws, arms, 1, 0, c.rt)));
            }
            double mu = 0; for (double v : nets) mu += v; mu /= nets.size();
            double sd = 0; for (double v : nets) sd += (v - mu) * (v - mu); sd = std::sqrt(sd / nets.size());
            std::printf("%-5s %+10.1f %+12.1f %10.1f %8.1f\n", c.sym.c_str(), actual, mu, sd, sd > 0 ? (actual - mu) / sd : 0);
        }
        return 0;
    }


    if (mode == "thrfloor") {
        // THRESHOLD-FLOOR STUDY (S-2026-07-13, operator): how LOW can each coin's up-jump
        // trigger go before chop eats it — and does a BRACKET CONFIRM (enter only after price
        // pushes +b% ABOVE the trigger close within TTL bars) keep low thresholds out of chop?
        // Long-only spot, BE-N6 cascade via the REAL live header, 20bp RT, 2x = full re-sim.
        // Gate (crypto rule): net+ && PF>=1.3 && both WF halves+ && 2x+ ; y2022 SHOWN not gated.
        double bo = getenv("TF_BRK") ? atof(getenv("TF_BRK")) : 0.0;   // confirm offset (0 = off)
        int cttl  = getenv("TF_TTL") ? atoi(getenv("TF_TTL")) : 12;
        std::printf("THR-FLOOR — BE-cascade mimic, %s, RT 20bp\n",
                    bo > 0 ? "BRACKET-CONFIRM on" : "plain trigger");
        std::printf("%-5s %2s %5s %5s | %5s %5s | %6s %8s %6s %8s %8s %8s %8s %+8s %5s\n",
            "coin", "W", "thr", "brk", "trig", "skip", "n", "net%", "PF", "H1", "H2", "2xcost", "ddbp", "y22", "gate");
        struct TC { std::string coin; int W; };
        std::vector<TC> tcs = { {"BTC",2},{"ETH",1},{"SOL",1},{"BNB",1},{"DOGE",4},{"ADA",1},{"XRP",1},{"TRX",1},
            // S-2026-07-13 operator: run the SAME low-thr study on ALL traded crypto, not just the 8 grid coins.
            {"NEAR",1},{"AVAX",1},{"LINK",1},{"BCH",2},{"UNI",1},{"OP",1},{"XLM",1},{"GRT",1},{"AAVE",1} };
        std::vector<double> arms = {0.2, 2, 3, 4, 6, 8};
        for (auto& tc : tcs) {
            if (!B.count(tc.coin)) B[tc.coin] = load(tc.coin);
            const Bars& b = B[tc.coin]; if (!b.N) continue;
            for (double thr : {0.005, 0.0075, 0.01, 0.015, 0.02, 0.03}) {
                std::vector<Window> ws; int ntrig = 0, nskip = 0;
                if (bo <= 0) {
                    ws = parent(b, tc.W, thr);
                    ntrig = (int)ws.size();
                } else {
                    // bracket-confirm: trigger -> pend buy-stop at c*(1+bo), TTL bars; unfilled = chop-skipped
                    bool pos = false; int i0 = tc.W;
                    for (int i = i0; i < b.N - 2; i++) {
                        double j = b.c[i] / b.c[i - tc.W] - 1.0;
                        if (!pos && j >= thr) {
                            ntrig++;
                            double lvl = b.c[i] * (1.0 + bo); int ei = -1;
                            for (int k = i + 1; k <= std::min(i + cttl, b.N - 2); k++)
                                if (b.c[k] >= lvl) { ei = k + 1; break; }
                            if (ei < 0) { nskip++; continue; }
                            pos = true;
                            int x = -1;
                            for (int k = ei; k < b.N - 1; k++) {
                                double jx = b.c[k] / b.c[k - tc.W] - 1.0;
                                if (jx <= -thr) { x = k + 1; break; }
                            }
                            if (x < 0) x = b.N - 1;
                            ws.push_back({ei, x, b.o[ei], j});
                            i = x; pos = false;
                        }
                    }
                }
                if (ws.size() < 5) { std::printf("%-5s %2d %4.2f%% %4.1f | (too few windows: %zu)\n", tc.coin.c_str(), tc.W, thr*100, bo*100, ws.size()); continue; }
                auto rows = engine_book_stagger(b, ws, arms, 1, 0, 20.0);
                double n2x = book_net(engine_book_stagger(b, ws, arms, 1, 0, 40.0));
                Met m = metrics(rows, ws, b, n2x);
                bool gate = (m.net > 0) && (m.pf >= 1.3) && (m.h1 > 0) && (m.h2 > 0) && (n2x > 0);
                std::printf("%-5s %2d %4.2f%% %4.1f | %5d %5d | %6d %+8.0f %6.2f %+8.0f %+8.0f %+8.0f %8.0f %+8.0f %5s\n",
                    tc.coin.c_str(), tc.W, thr*100, bo*100, ntrig, nskip,
                    m.n, m.net, m.pf, m.h1, m.h2, n2x, m.maxdd_bp, m.y2022, gate ? "PASS" : "-");
            }
        }
        return 0;
    }


    if (mode == "corrgate") {
        // CH-03 CORRELATION-GATE BACKTEST (audit 2026-07-13). Question: does suppressing an
        // alt up-jump entry WHEN BTC is in a hard 24h move (|btc24h|>THR) AND the alt is moving
        // the same direction >50% of BTC's magnitude — the live gate's exact rule — IMPROVE the
        // book? Faithful: same live-header BE-cascade stagger book, same 20bp RT. For each coin
        // we run the book on ALL up-jump windows vs windows MINUS the suppressed set, and compare.
        // Enable the gate ONLY if suppression is a net improvement.
        const double BTC_THR = getenv("CG_BTC") ? atof(getenv("CG_BTC")) : 0.05;  // BTC 24h move
        std::printf("CORR-GATE BT — suppress alt up-jump when |BTC 24h|>%.0f%% & alt same-dir >50%% of BTC. 20bp RT.\n", BTC_THR*100);
        std::printf("%-5s %2s %5s | %6s %8s %6s | %6s %8s %6s | %8s %6s %6s\n",
            "coin","W","thr","nAll","netAll%","pfAll","nKeep","netKeep%","pfKeep","suppr_n","suppr%","verdict");
        // BTC 1h series -> ts(ms) -> close, for 24h lookback (24 bars).
        if (!B.count("BTC")) B["BTC"] = load("BTC");
        const Bars& btc = B["BTC"];
        std::map<int64_t,double> btc_close; for (int i=0;i<btc.N;i++) btc_close[btc.ts[i]]=btc.c[i];
        auto btc_24h = [&](int64_t ts_ms)->double{
            int64_t prev = ts_ms - 24LL*3600*1000;
            auto a=btc_close.find(ts_ms), p=btc_close.find(prev);
            if (a==btc_close.end()||p==btc_close.end()||p->second<=0) return 0.0;
            return a->second/p->second - 1.0;
        };
        struct TC { std::string coin; int W; };
        std::vector<TC> tcs={{"ETH",1},{"SOL",1},{"BNB",1},{"DOGE",4},{"ADA",1},{"XRP",1},{"TRX",1}};
        std::vector<double> arms={0.2,2,3,4,6,8};
        for (auto& tc:tcs){
            if(!B.count(tc.coin))B[tc.coin]=load(tc.coin);
            const Bars& b=B[tc.coin]; if(!b.N)continue;
            for(double thr:{0.02,0.03}){
                auto all=parent(b,tc.W,thr);
                if(all.size()<8)continue;
                // split: suppressed if BTC 24h hot AND alt same-dir >50% of BTC at entry
                std::vector<Window> keep; int suppr=0;
                for(auto& w:all){
                    int64_t ets=b.ts[w.ei];
                    double bm=btc_24h(ets);
                    // alt 24h return at entry
                    int ai=w.ei; int pj=ai; while(pj>0 && b.ts[ai]-b.ts[pj]<24LL*3600*1000) pj--;
                    double am=(pj>=0&&b.c[pj]>0)? b.c[ai]/b.c[pj]-1.0 : 0.0;
                    bool hot = std::fabs(bm)>BTC_THR && ((bm>0&&am>bm*0.5)||(bm<0&&am<bm*0.5));
                    if(hot) suppr++; else keep.push_back(w);
                }
                auto rowsA=engine_book_stagger(b,all,arms,1,0,20.0);
                double n2xA=book_net(engine_book_stagger(b,all,arms,1,0,40.0));
                Met mA=metrics(rowsA,all,b,n2xA);
                double netK=0,pfK=0; int nK=0;
                if(keep.size()>=5){
                    auto rowsK=engine_book_stagger(b,keep,arms,1,0,20.0);
                    double n2xK=book_net(engine_book_stagger(b,keep,arms,1,0,40.0));
                    Met mK=metrics(rowsK,keep,b,n2xK); netK=mK.net; pfK=mK.pf; nK=mK.n;
                }
                const char* verdict = (keep.size()>=5 && pfK>mA.pf && netK>mA.net*0.7) ? "GATE+" :
                                      (suppr==0) ? "n/a(0supp)" : "GATE-";
                std::printf("%-5s %2d %4.0f%% | %6zu %+8.0f %6.2f | %6zu %+8.0f %6.2f | %8d %5.0f%% %6s\n",
                    tc.coin.c_str(),tc.W,thr*100, all.size(),mA.net,mA.pf,
                    keep.size(),netK,pfK, suppr, all.size()?100.0*suppr/all.size():0, verdict);
            }
        }
        return 0;
    }


    if (mode == "befloor") {
        // BE-FLOOR vs NO-FLOOR real-column test (operator 2026-07-13, after UNI-UJH -146bp).
        // The live grid/UJH cells are NO-FLOOR (T1 live from entry -> cold losses). The operator's
        // spec: only trade AFTER BE (be_floor: leg FLAT until +be_bp, floored stop -> can't book
        // a cold loss). Config = validated 35266b6 (be_floor=true, be_bp=20, tight trail 20 / wide
        // 150). Faithful: REAL live header, self-detect, REAL column (net_bp_real) — never the
        // clamped model column. Decisive metric: # NEGATIVE clips (must be ~0 for be_floor).
        std::printf("BEFLOOR vs NOFLOOR — REAL column (net_bp_real), self-detect, per coin\n");
        std::printf("%-5s %2s %5s | %8s %6s %6s %6s | %8s %6s %6s %6s\n",
            "coin","W","thr","NF_net%","NF_PF","NF_n","NF_neg","BF_net%","BF_PF","BF_n","BF_neg");
        struct TC { std::string coin; int W; };
        std::vector<TC> tcs = { {"BTC",2},{"ETH",1},{"SOL",1},{"BNB",1},{"DOGE",4},{"ADA",1},{"XRP",1},{"TRX",1},
            {"NEAR",1},{"AVAX",1},{"LINK",1},{"BCH",2},{"UNI",1},{"OP",1},{"XLM",1},{"GRT",1},{"AAVE",1} };
        std::vector<double> arms = {0.2, 2, 3, 4, 6, 8};
        auto run_cell = [&](const Bars& b, int W, double thr, bool be_floor,
                            double& net, double& pf, int& n, int& nneg){
            std::vector<double> nets;
            if (!be_floor) {
                auto ws = parent(b, W, thr);
                auto rows = engine_book_stagger(b, ws, arms, 1, 0, 20.0);
                for (auto& r : rows) nets.push_back(r.net_bp);
            } else {
                UpJumpLadderCompanion::Config c;
                c.parent_tag="BT"; c.tag="BF"; c.symbol="bt";
                c.tight = {0,0,0,20.0}; c.wide = {0,0,0,150.0};   // trail_bp only (be_floor)
                c.be_floor = true; c.be_bp = 20.0;
                c.det_w = W; c.det_thr = thr; c.tf_secs = 3600; c.round_trip_bp = 20.0;
                c.reclip_pct = 0.0; c.cap = 8;
                UpJumpLadderCompanion eng(c);
                eng.set_on_clip([&](const UpJumpLadderCompanion::ClipRecord& rec){ nets.push_back(rec.net_bp_real); });
                for (int i=0;i<b.N;i++) eng.observe(true, 0.0, b.c[i], (int64_t)i*TF_MS);
                eng.observe(false, 0.0, b.N? b.c[b.N-1]:0.0, (int64_t)(b.N?b.N-1:0)*TF_MS);
            }
            double gw=0,gl=0; net=0; n=(int)nets.size(); nneg=0;
            for (double v: nets){ net += v/100.0; if(v>0)gw+=v; else {gl-=v; if(v<0)nneg++;} }
            pf = gl>0? gw/gl : (gw>0?999:0);
        };
        for (auto& tc: tcs){
            if(!B.count(tc.coin))B[tc.coin]=load(tc.coin);
            const Bars& b=B[tc.coin]; if(!b.N)continue;
            for (double thr : {0.005, 0.02}) {
                double nfn,nfp,bfn,bfp; int nfN,nfNeg,bfN,bfNeg;
                run_cell(b,tc.W,thr,false,nfn,nfp,nfN,nfNeg);
                run_cell(b,tc.W,thr,true, bfn,bfp,bfN,bfNeg);
                std::printf("%-5s %2d %4.1f%% | %+8.0f %6.2f %6d %6d | %+8.0f %6.2f %6d %6d\n",
                    tc.coin.c_str(),tc.W,thr*100, nfn,nfp,nfN,nfNeg, bfn,bfp,bfN,bfNeg);
            }
        }
        return 0;
    }


    if (mode == "confirmcut") {
        // CONFIRMED-ENTRY sweep (operator 2026-07-13b). Does Option-B confirm_bp ELIMINATE the
        // REVERSAL_CUT / buy-the-top loss cluster while KEEPING the edge? SAME BE-cascade book
        // as coldcut (= the current live gold up-jump behaviour), fixed cold-loss cut, but sweep
        // confirm_bp incl. 0 (=immediate entry = current live). A leg stays FLAT (books nothing,
        // pays no cost) until fav>=confirm_bp from window entry; never-confirmed legs cost ~0.
        // Drives the REAL live header (net_bp_real); captures clip REASON + leg LABEL so we can
        // count REVERSAL_CUT legs, all-neg legs, and NEVER-OPENED (never-confirmed) legs.
        // Envs: CC_COIN(GOLD) CC_W(2) CC_THR(0.005) CC_RT(5) CC_CUT(30=cold-loss cut,0=off)
        //       CC_CONFIRM(comma list, default 0,20,30,50,80)  CC_SHORT / CC_FADE (dir variants).
        std::string coin = getenv("CC_COIN") ? getenv("CC_COIN") : "GOLD";
        int W = getenv("CC_W") ? atoi(getenv("CC_W")) : 2;
        double _thr = getenv("CC_THR") ? atof(getenv("CC_THR")) : 0.005;
        double _rt  = getenv("CC_RT")  ? atof(getenv("CC_RT"))  : 5.0;
        double _cut = getenv("CC_CUT") ? atof(getenv("CC_CUT")) : 30.0;
        std::vector<double> confs;
        { const char* ce=getenv("CC_CONFIRM"); std::string cl=ce?ce:"0,20,30,50,80";
          std::stringstream ss(cl); std::string t; while(std::getline(ss,t,',')) if(!t.empty()) confs.push_back(atof(t.c_str())); }
        std::vector<double> arms={0.2,2,3,4,6,8};   // BE-N6 (crypto winner family / live gold mimic)
        if(!B.count(coin))B[coin]=load(coin);
        const Bars& b=B[coin]; if(!b.N){std::fprintf(stderr,"no data for %s\n",coin.c_str());return 1;}
        // direction/window construction identical to coldcut (LONG up-jump, or SHORT mirror, or FADE)
        Bars bb=b;
        if (getenv("CC_SHORT")) { double base=b.c.size()?b.c[0]*2.0:0.0;
            for(int i=0;i<bb.N;i++){bb.o[i]=base-b.o[i];bb.c[i]=base-b.c[i];double lo=base-b.h[i],hi=base-b.l[i];bb.l[i]=lo;bb.h[i]=hi;} }
        const Bars& bx = getenv("CC_SHORT")?bb:b;
        std::vector<Window> ws;
        if (getenv("CC_FADE")) {
            bool pos=false;int ei=0;double epx=0,jent=0;
            for(int i=W;i<bx.N;i++){double j=bx.c[i]/bx.c[i-W]-1.0;
                if(!pos&&j<=-_thr){int e=i+1;if(e>=bx.N)continue;pos=true;ei=e;epx=bx.o[e];jent=j;}
                else if(pos&&j>=_thr){int x=i+1;if(x>=bx.N)x=bx.N-1;ws.push_back({ei,x,epx,jent});pos=false;}}
            if(pos)ws.push_back({ei,bx.N-1,epx,jent});
        } else ws=parent(bx,W,_thr);
        // CC_FROMYEAR: 2022 is FULLY IRRELEVANT for long-only spot (operator 2026-07-13, final) —
        // exclude pre-fromyear windows from EVALUATION entirely (entry-year attribution).
        int fromyear = getenv("CC_FROMYEAR") ? atoi(getenv("CC_FROMYEAR")) : 0;
        if (fromyear > 0) { std::vector<Window> fw; for (auto& w : ws) if (year_of(bx.ts[w.ei]) >= fromyear) fw.push_back(w); ws = fw; }
        const char* dir = getenv("CC_SHORT")?"SHORT":(getenv("CC_FADE")?"FADE":"LONG");

        std::printf("CONFIRMED-ENTRY sweep — %s %s W=%d thr=%.2f%% RT=%.0fbp cut=%.0fbp  BE-cascade book, REAL column\n",
            coin.c_str(),dir,W,_thr*100,_rt,_cut);
        std::printf("  windows=%zu  arms BE-N6 {0.2,2,3,4,6,8} (6 base tiers/window), reclip OFF, cap=#tiers, no ladder\n", ws.size());
        std::printf("%-8s | %5s %9s %6s %8s | %5s %9s | %5s %9s | %6s | %9s %9s | %8s %10s | %9s %8s\n",
            "confirm","n","net%","PF","worst_bp","nRev","sumRev%","nNeg","sumNeg%","nvrOpen","H1%","H2%","y2022%","y23-26%","exbestEpi","bestEpi%");

        struct Rec{int64_t ts;double net;std::string reason,label;int epi;};
        for (double CONF : confs) {
            std::vector<Rec> rows; int total_legs=0, opened_legs=0;
            for (size_t wi=0; wi<ws.size(); wi++){
                const Window& w=ws[wi];
                UpJumpLadderCompanion::Config c;
                c.parent_tag="BT";c.tag="BT";c.symbol="bt";
                c.tight={arms[0],0,0.50,0}; c.wide={arms[1],0,0.50,0};
                for(size_t k=2;k<arms.size();k++)c.extra_base.push_back({arms[k],0,0.50,0});
                c.reclip_pct=0.0; c.cap=(int)arms.size(); c.cost_gate_bp=0;
                c.confirm_bp=CONF;                 // <-- the swept lever (Option-B confirmed entry)
                c.be_floor=false; c.det_w=0; c.tf_secs=3600; c.round_trip_bp=_rt;
                c.stagger_mode=1; c.stagger_k=0; c.stagger_be_bp=20.0;
                c.loss_cut_bp=_cut;                // fixed cold-loss cut (30bp default)
                UpJumpLadderCompanion eng(c);
                int64_t cur_ts=0; std::set<std::string> opened_labels;
                eng.set_on_clip([&](const UpJumpLadderCompanion::ClipRecord& rec){
                    std::string lbl = rec.tag.size()>3 ? rec.tag.substr(3) : rec.tag;  // strip "BT-"
                    rows.push_back({cur_ts,rec.net_bp_real,rec.reason,lbl,(int)wi});
                    opened_labels.insert(lbl);
                });
                for(int i=w.ei;i<w.xi;i++){
                    cur_ts=bx.ts[i];
                    if(_cut>0.0 && i<(int)bx.l.size()) eng.stop_check_only(bx.l[i],(int64_t)i*TF_MS); // bar low tests the stop
                    eng.observe(true,w.epx,bx.c[i],(int64_t)i*TF_MS);
                }
                int lastix=(w.xi-1>=w.ei)?w.xi-1:w.ei;
                double lastpx=(w.xi-1>=w.ei)?bx.c[w.xi-1]:w.epx;
                cur_ts=bx.ts[lastix];
                eng.observe(false,w.epx,lastpx,(int64_t)lastix*TF_MS);
                total_legs += (int)arms.size();            // 6 base tiers created per window
                opened_legs += (int)opened_labels.size();  // distinct tiers that emitted >=1 clip (opened)
            }
            double net=0,gw=0,gl=0,worst=0,negsum=0,rev_sum=0; int neg=0,rev_n=0;
            for(auto&r:rows){ net+=r.net/100.0; if(r.net>0)gw+=r.net; else{gl-=r.net;neg++;negsum+=r.net/100.0;}
                if(r.net<worst)worst=r.net;
                if(r.reason=="REVERSAL_CUT"){rev_n++;rev_sum+=r.net/100.0;} }
            double pf=gl>0?gw/gl:(gw>0?999:0);
            std::vector<Rec> so=rows; std::sort(so.begin(),so.end(),[](const Rec&a,const Rec&c){return a.ts<c.ts;});
            double h1=0,h2=0,y22=0,y2326=0;
            for(size_t k=0;k<so.size();k++){ if(k<so.size()/2)h1+=so[k].net/100.0; else h2+=so[k].net/100.0;
                if(year_of(so[k].ts)<=2022)y22+=so[k].net/100.0; else y2326+=so[k].net/100.0; }
            int never = total_legs - opened_legs;
            std::map<int,double> epin; for(auto&r:rows) epin[r.epi]+=r.net/100.0;
            double bestepi=0; for(auto&kv:epin) if(kv.second>bestepi)bestepi=kv.second;
            std::printf("%6.0fbp | %5d %+9.0f %6.2f %+8.1f | %5d %+9.1f | %5d %+9.1f | %6d | %+9.0f %+9.0f | %+8.0f %+10.0f | %+9.0f %+8.0f\n",
                CONF,(int)rows.size(),net,pf,worst,rev_n,rev_sum,neg,negsum,never,h1,h2,y22,y2326,net-bestepi,bestepi);
        }
        return 0;
    }

    if (mode == "mimicg") {
        // MIMIC-FLOOR g-SWEEP (S-2026-07-15, operator: unify every mimic to ONE exit =
        // BE-floored tight-giveback trail; find the SMALLEST g that keeps EACH cell's OWN
        // book net-positive after real cost, both WF halves). Single managed leg (cap=1),
        // confirmed entry (confirm_bp), per-tick BE floor + HWM trail by g — drives the REAL
        // engine mimic_floor path (parity by construction). Long-only gate (omit 2022):
        //   PASS = net>0 & PF>=1.3 & H1>0 & H2>0  AND  2x-cost net>0 & PF>=1.3.
        // Envs: CC_COIN CC_W CC_THR CC_RT(20) CC_CUT(50) CC_CONFIRM(20) CC_FROMYEAR(2023)
        //       MF_G (comma list, default 1.0,0.75,0.6,0.5,0.4,0.3,0.25,0.2,0.15,0.1)
        std::string coin = getenv("CC_COIN")?getenv("CC_COIN"):"GOLD";
        int W = getenv("CC_W")?atoi(getenv("CC_W")):1;
        double _thr=getenv("CC_THR")?atof(getenv("CC_THR")):0.04;
        double _rt =getenv("CC_RT") ?atof(getenv("CC_RT")) :20.0;
        double _cut=getenv("CC_CUT")?atof(getenv("CC_CUT")):50.0;
        double _cf =getenv("CC_CONFIRM")?atof(getenv("CC_CONFIRM")):20.0;
        double _rc =getenv("CC_RECLIP")?atof(getenv("CC_RECLIP")):0.0;   // re-enter on +rc continuation past prior peak (operator spec #4)
        std::vector<double> gs; { const char*ge=getenv("MF_G"); std::string gl=ge?ge:"1.0,0.75,0.6,0.5,0.4,0.3,0.25,0.2,0.15,0.1";
            std::stringstream ss(gl); std::string t; while(std::getline(ss,t,',')) if(!t.empty()) gs.push_back(atof(t.c_str())); }
        if(!B.count(coin))B[coin]=load(coin);
        const Bars&b=B[coin]; if(!b.N){std::fprintf(stderr,"no data %s\n",coin.c_str());return 1;}
        std::vector<Window> ws=parent(b,W,_thr);
        int fromyear=getenv("CC_FROMYEAR")?atoi(getenv("CC_FROMYEAR")):2023;
        if(fromyear>0){std::vector<Window>fw;for(auto&w:ws)if(year_of(b.ts[w.ei])>=fromyear)fw.push_back(w);ws=fw;}
        // CC_TRENDGATE=K (1h bars, 0/unset=off): up-only STRUCTURAL filter (operator ask #2,
        // "the most likely methodology to rescue chop-losers" — NO 200DMA, NO moving average).
        // Arm a window only when price at entry is above price K bars earlier (higher-high
        // trend proxy). Suppresses up-jumps that fire inside a down/chop leg (the whipsaw source).
        int tgate=getenv("CC_TRENDGATE")?atoi(getenv("CC_TRENDGATE")):0;
        if(tgate>0){std::vector<Window>tw;for(auto&w:ws){int ref=w.ei-1-tgate; if(ref>=0 && b.c[w.ei-1]>b.c[ref]) tw.push_back(w);} ws=tw;}
        std::printf("MIMIC-FLOOR g-sweep — %s LONG W=%d thr=%.2f%% RT=%.0fbp cut=%.0fbp confirm=%.0fbp reclip=%.1f%% trendgate=%d  windows=%zu (fromyear=%d)\n",
            coin.c_str(),W,_thr*100,_rt,_cut,_cf,_rc*100,tgate,ws.size(),fromyear);
        std::printf("%-5s | %5s %9s %6s %8s | %5s %9s | %9s %9s | %9s | %s\n",
            "g","n","net%","PF","worst_bp","nNeg","sumNeg%","H1%","H2%","bestEpi%","GATE(base;2x)");
        struct Rec{int64_t ts;double net;int epi;};
        auto run=[&](double g,double rt){
            std::vector<Rec> rows;
            for(size_t wi=0;wi<ws.size();wi++){const Window&w=ws[wi];
                UpJumpLadderCompanion::Config c; c.parent_tag="BT";c.tag="BT";c.symbol="bt";
                c.tight={0.2,0,0.0,0}; c.reclip_pct=_rc; c.cap=1; c.cost_gate_bp=0;
                c.confirm_bp=_cf; c.be_floor=false; c.det_w=0; c.tf_secs=3600; c.round_trip_bp=rt;
                c.loss_cut_bp=_cut; c.mimic_floor=true; c.mimic_giveback=g;
                UpJumpLadderCompanion eng(c); int64_t cur_ts=0;
                eng.set_on_clip([&](const UpJumpLadderCompanion::ClipRecord&rc){rows.push_back({cur_ts,rc.net_bp_real,(int)wi});});
                for(int i=w.ei;i<w.xi;i++){cur_ts=b.ts[i];
                    if(i<(int)b.l.size())eng.stop_check_only(b.l[i],(int64_t)i*TF_MS);
                    eng.observe(true,w.epx,b.c[i],(int64_t)i*TF_MS);}
                int lix=(w.xi-1>=w.ei)?w.xi-1:w.ei; double lpx=(w.xi-1>=w.ei)?b.c[w.xi-1]:w.epx;
                cur_ts=b.ts[lix]; eng.observe(false,w.epx,lpx,(int64_t)lix*TF_MS);
            }
            double net=0,gw=0,gl=0,worst=0,negsum=0;int neg=0;
            for(auto&r:rows){net+=r.net/100.0;if(r.net>0)gw+=r.net;else{gl-=r.net;neg++;negsum+=r.net/100.0;}if(r.net<worst)worst=r.net;}
            double pf=gl>0?gw/gl:(gw>0?999:0);
            std::vector<Rec>so=rows;std::sort(so.begin(),so.end(),[](const Rec&a,const Rec&c){return a.ts<c.ts;});
            double h1=0,h2=0;for(size_t k=0;k<so.size();k++){if(k<so.size()/2)h1+=so[k].net/100.0;else h2+=so[k].net/100.0;}
            std::map<int,double>epin;for(auto&r:rows)epin[r.epi]+=r.net/100.0;double bestepi=0;for(auto&kv:epin)if(kv.second>bestepi)bestepi=kv.second;
            struct R{int n;double net,pf,worst;int neg;double negsum,h1,h2,bestepi;};
            return R{(int)rows.size(),net,pf,worst,neg,negsum,h1,h2,bestepi};
        };
        for(double g:gs){
            auto r=run(g,_rt); auto r2=run(g,_rt*2.0);
            bool gate = r.net>0&&r.pf>=1.3&&r.h1>0&&r.h2>0 && r2.net>0&&r2.pf>=1.3;
            std::printf("%5.2f | %5d %+9.0f %6.2f %+8.1f | %5d %+9.1f | %+9.0f %+9.0f | %+9.0f | %s [2x net%+.0f pf%.2f]\n",
                g,r.n,r.net,r.pf,r.worst,r.neg,r.negsum,r.h1,r.h2,r.bestepi, gate?"PASS":"fail", r2.net,r2.pf);
        }
        return 0;
    }

    if (mode == "mimicstag") {
        // 4x STAGGERED FLOORED LADDER (S-2026-07-16, operator: "once we get to BE ... open at
        // least 4x mimics, stagger them ... never go negative on a mimic"). Same detector windows
        // as mimicg, but drives the extended engine: N legs each with an ESCALATING per-tier
        // confirm (BE / +1% / +2% / ...), mimic_stagger=true, cap==#tiers. Each leg floors at its
        // OWN fill -> distinct additive positions, post-arm never-negative. Long-only gate (omit
        // 2022): PASS = net>0 & PF>=1.3 & H1>0 & H2>0 AND 2x-cost net>0 & PF>=1.3.
        // Proof column: FLOOR_TRAIL clips (armed) must be >=0 (that IS the never-negative claim);
        // the tail lives only in pre-arm PREBE_CUT/ENGINE_EXIT flushes.
        // Envs: CC_COIN CC_W CC_THR CC_RT(20) CC_CUT(0) CC_RECLIP(0.05) CC_FROMYEAR(2023)
        //       CC_CONFIRMS (comma bp list, default "20,120,220,320" = BE/+1/+2/+3%)
        //       MF_G (comma g list, default "1.0,0.75,0.5")
        std::string coin = getenv("CC_COIN")?getenv("CC_COIN"):"BNB";
        int W = getenv("CC_W")?atoi(getenv("CC_W")):1;
        double _thr=getenv("CC_THR")?atof(getenv("CC_THR")):0.04;
        double _rt =getenv("CC_RT") ?atof(getenv("CC_RT")) :20.0;
        double _cut=getenv("CC_CUT")?atof(getenv("CC_CUT")):0.0;      // pre-arm cut (0 = findings default: cut churns)
        double _rc =getenv("CC_RECLIP")?atof(getenv("CC_RECLIP")):0.05;
        std::vector<double> confs; { const char*ce=getenv("CC_CONFIRMS"); std::string cl=ce?ce:"20,120,220,320";
            std::stringstream ss(cl); std::string t; while(std::getline(ss,t,',')) if(!t.empty()) confs.push_back(atof(t.c_str())); }
        std::vector<double> gs; { const char*ge=getenv("MF_G"); std::string gl=ge?ge:"1.0,0.75,0.5";
            std::stringstream ss(gl); std::string t; while(std::getline(ss,t,',')) if(!t.empty()) gs.push_back(atof(t.c_str())); }
        if(!B.count(coin))B[coin]=load(coin);
        const Bars&b=B[coin]; if(!b.N){std::fprintf(stderr,"no data %s\n",coin.c_str());return 1;}
        std::vector<Window> ws=parent(b,W,_thr);
        int fromyear=getenv("CC_FROMYEAR")?atoi(getenv("CC_FROMYEAR")):2023;
        if(fromyear>0){std::vector<Window>fw;for(auto&w:ws)if(year_of(b.ts[w.ei])>=fromyear)fw.push_back(w);ws=fw;}
        std::printf("STAGGERED FLOORED %zux LADDER — %s LONG W=%d thr=%.2f%% RT=%.0fbp cut=%.0fbp reclip=%.1f%% confirms=[",
            confs.size(),coin.c_str(),W,_thr*100,_rt,_cut,_rc*100);
        for(size_t k=0;k<confs.size();k++)std::printf("%s%.0f",k?",":"",confs[k]);
        std::printf("]bp windows=%zu (fromyear=%d)\n",ws.size(),fromyear);
        std::printf("%-5s | %5s %9s %6s %8s | %5s %9s | %9s %9s | %9s %10s | %s\n",
            "g","n","net%","PF","worst_bp","nNeg","sumNeg%","H1%","H2%","floorNet%","floorMinBp","GATE(base;2x)");
        struct Rec{int64_t ts;double net;int epi;int leg;bool floored;};
        auto legidx=[&](const std::string&tag)->int{ // -T1/-T2/-S1/-S2 -> 0..N
            auto p=tag.rfind('-'); if(p==std::string::npos)return 0; std::string s=tag.substr(p+1);
            if(s=="T1")return 0; if(s=="T2")return 1; if(!s.empty()&&s[0]=='S')return 1+atoi(s.c_str()+1); return 0; };
        auto run=[&](double g,double rt){
            std::vector<Rec> rows;
            for(size_t wi=0;wi<ws.size();wi++){const Window&w=ws[wi];
                UpJumpLadderCompanion::Config c; c.parent_tag="BT";c.tag="BT";c.symbol="bt";
                c.tight={0.2,0,0.0,0,confs[0]};
                if(confs.size()>1) c.wide={0.2,0,0.0,0,confs[1]};
                for(size_t k=2;k<confs.size();k++) c.extra_base.push_back({0.2,0,0.0,0,confs[k]});
                c.reclip_pct=_rc; c.cap=(int)confs.size(); c.cost_gate_bp=0;
                c.confirm_bp=confs[0]; c.be_floor=false; c.det_w=0; c.tf_secs=3600; c.round_trip_bp=rt;
                c.loss_cut_bp=_cut; c.mimic_floor=true; c.mimic_stagger=true; c.mimic_giveback=g;
                c.stagger_mode=0;   // per-leg confirm gates the opens (escalating entry), not advance_stagger_
                UpJumpLadderCompanion eng(c); int64_t cur_ts=0;
                eng.set_on_clip([&](const UpJumpLadderCompanion::ClipRecord&rc){
                    bool fl = std::string(rc.reason).find("FLOOR_TRAIL")!=std::string::npos;
                    rows.push_back({cur_ts,rc.net_bp_real,(int)wi,legidx(rc.tag),fl});});
                for(int i=w.ei;i<w.xi;i++){cur_ts=b.ts[i];
                    if(i<(int)b.l.size())eng.stop_check_only(b.l[i],(int64_t)i*TF_MS);
                    eng.observe(true,w.epx,b.c[i],(int64_t)i*TF_MS);}
                int lix=(w.xi-1>=w.ei)?w.xi-1:w.ei; double lpx=(w.xi-1>=w.ei)?b.c[w.xi-1]:w.epx;
                cur_ts=b.ts[lix]; eng.observe(false,w.epx,lpx,(int64_t)lix*TF_MS);
            }
            double net=0,gw=0,gl=0,worst=0,negsum=0,floornet=0,floormin=0;int neg=0;
            std::vector<double>legnet(confs.size(),0.0);std::vector<int>legn(confs.size(),0);
            for(auto&r:rows){net+=r.net/100.0;if(r.net>0)gw+=r.net;else{gl-=r.net;neg++;negsum+=r.net/100.0;}
                if(r.net<worst)worst=r.net;
                if(r.floored){floornet+=r.net/100.0; if(r.net<floormin)floormin=r.net;}
                if(r.leg>=0&&r.leg<(int)legnet.size()){legnet[r.leg]+=r.net/100.0;legn[r.leg]++;}}
            double pf=gl>0?gw/gl:(gw>0?999:0);
            std::vector<Rec>so=rows;std::sort(so.begin(),so.end(),[](const Rec&a,const Rec&c){return a.ts<c.ts;});
            double h1=0,h2=0;for(size_t k=0;k<so.size();k++){if(k<so.size()/2)h1+=so[k].net/100.0;else h2+=so[k].net/100.0;}
            struct R{int n;double net,pf,worst;int neg;double negsum,h1,h2,floornet,floormin;std::vector<double>legnet;std::vector<int>legn;};
            return R{(int)rows.size(),net,pf,worst,neg,negsum,h1,h2,floornet,floormin,legnet,legn};
        };
        for(double g:gs){
            auto r=run(g,_rt); auto r2=run(g,_rt*2.0);
            bool gate = r.net>0&&r.pf>=1.3&&r.h1>0&&r.h2>0 && r2.net>0&&r2.pf>=1.3;
            std::printf("%5.2f | %5d %+9.0f %6.2f %+8.1f | %5d %+9.1f | %+9.0f %+9.0f | %+9.0f %+10.1f | %s [2x net%+.0f pf%.2f]\n",
                g,r.n,r.net,r.pf,r.worst,r.neg,r.negsum,r.h1,r.h2,r.floornet,r.floormin, gate?"PASS":"fail", r2.net,r2.pf);
            std::printf("      per-leg net%%:");
            for(size_t k=0;k<r.legnet.size();k++)std::printf(" %s[c%.0f]=%+.0f(n%d)",k==0?"T1":(k==1?"T2":("S"+std::to_string(k-1)).c_str()),confs[k],r.legnet[k],r.legn[k]);
            std::printf("\n");
        }
        return 0;
    }

    if (mode == "confirmrand") {
        // RANDOM-ENTRY CONTROL for a confirmcut cell: same window count + durations placed
        // uniformly at random (non-overlapping), SAME UpJumpLadderCompanion config
        // (confirm_bp entry + cold cut + BE-N6 book, REAL column). 20 seeds.
        // Envs as confirmcut but CC_CONFIRM = single value (default 20). LONG only.
        std::string coin = getenv("CC_COIN") ? getenv("CC_COIN") : "GOLD";
        int W = getenv("CC_W") ? atoi(getenv("CC_W")) : 2;
        double _thr = getenv("CC_THR") ? atof(getenv("CC_THR")) : 0.005;
        double _rt  = getenv("CC_RT")  ? atof(getenv("CC_RT"))  : 5.0;
        double _cut = getenv("CC_CUT") ? atof(getenv("CC_CUT")) : 30.0;
        double CONF = getenv("CC_CONFIRM") ? atof(getenv("CC_CONFIRM")) : 20.0;
        if(!B.count(coin))B[coin]=load(coin);
        const Bars& b=B[coin]; if(!b.N){std::fprintf(stderr,"no data for %s\n",coin.c_str());return 1;}
        auto ws = parent(b,W,_thr);
        int fromyear = getenv("CC_FROMYEAR") ? atoi(getenv("CC_FROMYEAR")) : 0;
        int lo_bound = W+1;
        if (fromyear > 0) {
            std::vector<Window> fw; for (auto& w : ws) if (year_of(b.ts[w.ei]) >= fromyear) fw.push_back(w); ws = fw;
            while (lo_bound < b.N && year_of(b.ts[lo_bound]) < fromyear) lo_bound++;   // random placement inside the same window
        }
        std::vector<double> arms={0.2,2,3,4,6,8};
        auto run_ws=[&](const std::vector<Window>& wl)->double{
            double net=0;
            for (const Window& w : wl){
                UpJumpLadderCompanion::Config c;
                c.parent_tag="BT";c.tag="BT";c.symbol="bt";
                c.tight={arms[0],0,0.50,0}; c.wide={arms[1],0,0.50,0};
                for(size_t k=2;k<arms.size();k++)c.extra_base.push_back({arms[k],0,0.50,0});
                c.reclip_pct=0.0; c.cap=(int)arms.size(); c.cost_gate_bp=0;
                c.confirm_bp=CONF; c.be_floor=false; c.det_w=0; c.tf_secs=3600; c.round_trip_bp=_rt;
                c.stagger_mode=1; c.stagger_k=0; c.stagger_be_bp=20.0; c.loss_cut_bp=_cut;
                UpJumpLadderCompanion eng(c);
                eng.set_on_clip([&](const UpJumpLadderCompanion::ClipRecord& rec){ net+=rec.net_bp_real/100.0; });
                for(int i=w.ei;i<w.xi;i++){
                    if(_cut>0.0 && i<(int)b.l.size()) eng.stop_check_only(b.l[i],(int64_t)i*TF_MS);
                    eng.observe(true,w.epx,b.c[i],(int64_t)i*TF_MS);
                }
                int lastix=(w.xi-1>=w.ei)?w.xi-1:w.ei;
                double lastpx=(w.xi-1>=w.ei)?b.c[w.xi-1]:w.epx;
                eng.observe(false,w.epx,lastpx,(int64_t)lastix*TF_MS);
            }
            return net;
        };
        double actual = run_ws(ws);
        std::vector<int> durs; for(auto&w:ws)durs.push_back(w.xi-w.ei);
        std::vector<double> nets;
        for(int seed=0;seed<20;seed++){
            std::mt19937 rng(1000+seed);
            std::vector<int> d=durs; std::shuffle(d.begin(),d.end(),rng);
            std::vector<std::pair<int,int>> placed; std::vector<Window> rws;
            for(int dur:d){
                for(int tries=0;tries<200;tries++){
                    int lo=lo_bound, hi=b.N-2-dur; if(hi<=lo)break;
                    int ei=lo+(int)(rng()%(uint64_t)(hi-lo));
                    bool ok=true;
                    for(auto&pl:placed) if(ei<pl.second&&ei+dur>pl.first){ok=false;break;}
                    if(ok){placed.push_back({ei,ei+dur});rws.push_back({ei,ei+dur,b.o[ei],_thr});break;}
                }
            }
            nets.push_back(run_ws(rws));
        }
        double mu=0;for(double v:nets)mu+=v;mu/=nets.size();
        double sd=0;for(double v:nets)sd+=(v-mu)*(v-mu);sd=std::sqrt(sd/nets.size());
        std::printf("CONFIRMRAND %s W=%d thr=%.2f%% conf=%.0fbp cut=%.0f RT=%.0f: actual=%+.0f%%  rand_mu=%+.0f%%  sd=%.0f  z=%+.1f\n",
            coin.c_str(),W,_thr*100,CONF,_cut,_rt,actual,mu,sd,sd>0?(actual-mu)/sd:0);
        return 0;
    }

    if (mode == "coldcut") {
        // COLD-LOSS-CUT sweep (operator 2026-07-13). No-floor ladder + a bounded stop on UNARMED
        // legs. Shows the REAL column (net_bp_real), worst single clip, #neg clips, at cut levels.
        // Goal: cap the -146bp tail while keeping the book net-positive.
        std::printf("COLD-CUT sweep — no-floor ladder + bounded unarmed-leg stop, REAL column\n");
        std::printf("%-5s %5s | %5s %8s %6s %6s %8s\n","coin","cut","n","net%","PF","neg","worst_bp");
        struct TC{std::string c;int W;}; std::vector<TC> tcs;
        { const char* ws=getenv("CC_W"); std::string wl=ws?ws:"12,24,48";
          std::stringstream ss(wl); std::string t; while(std::getline(ss,t,',')) if(!t.empty()) tcs.push_back({getenv("CC_COIN")?getenv("CC_COIN"):"XAU", atoi(t.c_str())}); }
        std::vector<double> arms={0.2,2,3,4,6,8};
        for (auto& tc:tcs){
            if(!B.count(tc.c))B[tc.c]=load(tc.c); const Bars& b=B[tc.c]; if(!b.N)continue;
            Bars bb = b;
            if (getenv("CC_SHORT")) { for (int _i=0;_i<bb.N;_i++){ double t=bb.o[_i]; (void)t; } // mirror price so down-jump==up-jump
                double base = b.c.size()? b.c[0]*2.0 : 0.0;
                for (int _i=0;_i<bb.N;_i++){ bb.o[_i]=base-b.o[_i]; bb.c[_i]=base-b.c[_i]; double lo=base-b.h[_i], hi=base-b.l[_i]; bb.l[_i]=lo; bb.h[_i]=hi; } }
            const Bars& bx = getenv("CC_SHORT") ? bb : b;
            double _thr=getenv("CC_THR")?atof(getenv("CC_THR")):0.005;
            std::vector<Window> ws;
            if (getenv("CC_FADE")) {
                // MEAN-REVERSION: enter LONG on a DOWN-jump (j<=-thr), exit when it recovers (j>=+thr).
                bool pos=false; int ei=0; double epx=0,jent=0;
                for (int i=tc.W;i<bx.N;i++){ double j=bx.c[i]/bx.c[i-tc.W]-1.0;
                    if(!pos && j<=-_thr){int e=i+1; if(e>=bx.N)continue; pos=true; ei=e; epx=bx.o[e]; jent=j;}
                    else if(pos && j>=_thr){int x=i+1; if(x>=bx.N)x=bx.N-1; ws.push_back({ei,x,epx,jent}); pos=false;} }
                if(pos) ws.push_back({ei,bx.N-1,epx,jent});
            } else ws=parent(bx,tc.W,_thr);
            double _rt = getenv("CC_RT") ? atof(getenv("CC_RT")) : 20.0;   // per-asset real RT cost
            for (const char* cut : {"800","20","30","40","50","70"}) {      // 800 = effectively-off baseline
                setenv("LOSS_CUT",cut,1);
                auto rows=engine_book_stagger(bx,ws,arms,1,0,_rt);
                double net=0,gw=0,gl=0,worst=0; int neg=0;
                for(auto& r:rows){net+=r.net_bp/100.0; if(r.net_bp>0)gw+=r.net_bp; else{gl-=r.net_bp;neg++;} if(r.net_bp<worst)worst=r.net_bp;}
                double pf=gl>0?gw/gl:(gw>0?999:0);
                // WF halves by clip order + by calendar (2022 vs 2023-26) via ts
                std::vector<Clip> so=rows; std::sort(so.begin(),so.end(),[](const Clip&a,const Clip&c){return a.ts<c.ts;});
                double h1=0,h2=0,y22=0,y2326=0;
                for(size_t k=0;k<so.size();k++){ if(k<so.size()/2)h1+=so[k].net_bp/100.0; else h2+=so[k].net_bp/100.0;
                    if(year_of(so[k].ts)<=2022)y22+=so[k].net_bp/100.0; else y2326+=so[k].net_bp/100.0; }
                std::printf("%-5s %4sbp | %5zu %+8.0f %6.2f %6d %+8.0f | H1%+7.0f H2%+7.0f | y22%+7.0f y23-26%+7.0f\n",
                    tc.c.c_str(),cut,rows.size(),net,pf,neg,worst,h1,h2,y22,y2326);
            }
            std::printf("\n");
        }
        unsetenv("LOSS_CUT");
        return 0;
    }

    if (mode == "ccrandom") {
        // BETA / RANDOM-ENTRY CONTROL for the coldcut BE-cascade result (operator 2026-07-13,
        // standing crypto-mechanism-port mandate). Mirrors the coldcut config BYTE-FOR-BYTE
        // (same coin/CC_SHORT mirror/CC_FADE, same W, thr, RT, LOSS_CUT, same BE-cascade arms,
        // same live-header stagger book) but REPLACES the up-jump entry TRIGGER with RANDOM
        // entries: same NUMBER of episodes and same per-episode DURATIONS, placed at random
        // non-overlapping bar timestamps, seed-varied. Everything downstream (book, cut, cost,
        // holding) is identical. If the REAL up-jump net does not sit FAR in the right tail of
        // the random cloud, the "edge" is just long-only beta/exposure -> BETA-ONLY / DEAD.
        //   Envs: CC_COIN, CC_W, CC_THR, CC_RT, CC_CUT(=LOSS_CUT bp, default 30),
        //         CC_SHORT (down-jump mirror), CC_FADE (mean-reversion entry), CC_SEEDS(>=200).
        std::string coin = getenv("CC_COIN") ? getenv("CC_COIN") : "XAU";
        int W = getenv("CC_W") ? atoi(getenv("CC_W")) : 2;
        double _thr = getenv("CC_THR") ? atof(getenv("CC_THR")) : 0.005;
        double _rt = getenv("CC_RT") ? atof(getenv("CC_RT")) : 20.0;
        const char* cut = getenv("CC_CUT") ? getenv("CC_CUT") : "30";
        int NSEED = getenv("CC_SEEDS") ? atoi(getenv("CC_SEEDS")) : 200;
        setenv("LOSS_CUT", cut, 1);
        std::vector<double> arms = {0.2, 2, 3, 4, 6, 8};

        if (!B.count(coin)) B[coin] = load(coin);
        const Bars& b = B[coin]; if (!b.N) { std::fprintf(stderr, "no data for %s\n", coin.c_str()); return 1; }
        // CC_SHORT price mirror (exactly as coldcut): down-jump becomes up-jump on mirrored bars.
        Bars bb = b;
        if (getenv("CC_SHORT")) {
            double base = b.c.size() ? b.c[0] * 2.0 : 0.0;
            for (int i = 0; i < bb.N; i++) { bb.o[i] = base - b.o[i]; bb.c[i] = base - b.c[i];
                double lo = base - b.h[i], hi = base - b.l[i]; bb.l[i] = lo; bb.h[i] = hi; }
        }
        const Bars& bx = getenv("CC_SHORT") ? bb : b;

        // REAL windows: identical construction to coldcut (up-jump, or CC_FADE mean-reversion).
        std::vector<Window> ws;
        if (getenv("CC_FADE")) {
            bool pos = false; int ei = 0; double epx = 0, jent = 0;
            for (int i = W; i < bx.N; i++) { double j = bx.c[i] / bx.c[i - W] - 1.0;
                if (!pos && j <= -_thr) { int e = i + 1; if (e >= bx.N) continue; pos = true; ei = e; epx = bx.o[e]; jent = j; }
                else if (pos && j >= _thr) { int x = i + 1; if (x >= bx.N) x = bx.N - 1; ws.push_back({ei, x, epx, jent}); pos = false; } }
            if (pos) ws.push_back({ei, bx.N - 1, epx, jent});
        } else ws = parent(bx, W, _thr);

        double actual = book_net(engine_book_stagger(bx, ws, arms, 1, 0, _rt));
        int nreal = 0; { auto rr = engine_book_stagger(bx, ws, arms, 1, 0, _rt); nreal = (int)rr.size(); }
        std::vector<int> durs; for (auto& w : ws) durs.push_back(w.xi - w.ei);

        std::vector<double> nets;
        for (int seed = 0; seed < NSEED; seed++) {
            std::mt19937 rng(1000 + seed);
            std::vector<int> d = durs; std::shuffle(d.begin(), d.end(), rng);
            std::vector<std::pair<int, int>> placed; std::vector<Window> rws;
            for (int dur : d) {
                for (int tries = 0; tries < 200; tries++) {
                    int lo = W + 1, hi = bx.N - 2 - dur; if (hi <= lo) break;
                    int ei = lo + (int)(rng() % (uint64_t)(hi - lo));
                    bool ok = true;
                    for (auto& pl : placed) if (ei < pl.second && ei + dur > pl.first) { ok = false; break; }
                    if (ok) { placed.push_back({ei, ei + dur}); rws.push_back({ei, ei + dur, bx.o[ei], _thr}); break; }
                }
            }
            nets.push_back(book_net(engine_book_stagger(bx, rws, arms, 1, 0, _rt)));
        }
        unsetenv("LOSS_CUT");
        double mu = 0; for (double v : nets) mu += v; mu /= nets.size();
        double sd = 0; for (double v : nets) sd += (v - mu) * (v - mu); sd = std::sqrt(sd / nets.size());
        std::vector<double> sorted = nets; std::sort(sorted.begin(), sorted.end());
        double p95 = sorted[(size_t)(0.95 * (sorted.size() - 1))];
        int below = 0; for (double v : nets) if (v < actual) below++;
        double pct = 100.0 * below / nets.size();
        double z = sd > 0 ? (actual - mu) / sd : 0;
        const char* dir = getenv("CC_SHORT") ? "SHORT" : (getenv("CC_FADE") ? "FADE" : "LONG");
        std::fprintf(stderr, "%-6s %-5s cut=%-4sbp W=%d thr=%.2f%% RT=%.0fbp seeds=%d realN=%d realWin=%zu\n",
            coin.c_str(), dir, cut, W, _thr * 100, _rt, NSEED, nreal, ws.size());
        std::fprintf(stderr, "  real=%+.0f%%  rand_mu=%+.0f%%  rand_sd=%.0f%%  rand_95pct=%+.0f%%  z=%.2f  percentile=%.1f%%\n",
            actual, mu, sd, p95, z, pct);
        // machine-readable line on stdout (verbose engine CLIP logs go to stdout too; grep RESULT)
        std::printf("RESULT %s %s real=%.1f mu=%.1f sd=%.1f p95=%.1f z=%.3f pct=%.2f\n",
            coin.c_str(), dir, actual, mu, sd, p95, z, pct);
        return 0;
    }

    if (mode == "campaign") {
        // ── PARENT+MIMIC CAMPAIGN sim (operator directive 2026-07-13j §2.13) ──────────────
        // Two virtual lots per up-jump window:
        //   PARENT: confirmed entry (fav >= CP_CONFIRM bp from window entry px), structural
        //           stop, fee-BE arm, net-lock, HWM trail (CP_PTRAIL; 0 = ride-to-reversal,
        //           BE/lock floors stay on). Flush at window end (reversal j <= -thr).
        //   MIMIC:  CP_MFRAC of parent notional. Opens ONLY when ALL true: parent locked net
        //           covers mimic worst-case (funding equation: locked >= mfrac*(mstop+RT+slip+
        //           reserve)), parent MFE >= CP_MACT, fresh continuation (pullback >= CP_RESET
        //           bp from campaign HWM then close breaks HWM), no active mimic, < CP_MMAX
        //           mimics this campaign. Fast mgmt: stop CP_MSTOP, fee-BE arm +27bp, net-lock
        //           +50bp, HWM trail CP_MTRAIL from +60bp. Flushes with parent.
        // NEVER mimics a losing parent (funding gate implies locked net > 0).
        // Mimic judged STANDALONE (own book, mimic units) + combined (parent units).
        // Stops tested on bar LOW (pessimistic), exits at stop px. Costs charged per lot.
        // Envs: CP_COIN CP_W CP_THR CP_RT(20) CP_SLIP(3) CP_RESERVE(2) CP_CONFIRM(20)
        //       CP_FROMYEAR(2023) CP_MFRAC(0.25) CP_RESET(15) CP_MMAX(3)
        //       CP_PSTOP CP_PTRAIL CP_MACT CP_MSTOP CP_MTRAIL  (comma lists -> full grid)
        //       CP_MIMIC=0 parent-only rows.
        auto envs = [](const char* k, const char* d) { const char* e = getenv(k); return std::string(e ? e : d); };
        auto envd = [&](const char* k, double d) { const char* e = getenv(k); return e ? atof(e) : d; };
        auto list = [&](const char* k, const char* d) {
            std::vector<double> v; std::stringstream ss(envs(k, d)); std::string t;
            while (std::getline(ss, t, ',')) if (!t.empty()) v.push_back(atof(t.c_str())); return v; };
        std::string coin = argc > 2 ? argv[2] : envs("CP_COIN", "TRX");
        int W = (int)envd("CP_W", 8);
        double thr = envd("CP_THR", 0.035);
        double RT = envd("CP_RT", 20.0), SLIP = envd("CP_SLIP", 3.0), RES = envd("CP_RESERVE", 2.0);
        double CONF = envd("CP_CONFIRM", 20.0), MFRAC = envd("CP_MFRAC", 0.25);
        double RESET = envd("CP_RESET", 15.0); int MMAX = (int)envd("CP_MMAX", 3);
        int DELAY = (int)envd("CP_DELAY", 0);   // §2.12 robustness: enter N bars after confirm
        int H0 = -1, H1g = -1;                  // CP_HOURS=7-20 UTC: parent entry session gate
        { const char* he = getenv("CP_HOURS"); if (he) sscanf(he, "%d-%d", &H0, &H1g); }
        int fromyear = (int)envd("CP_FROMYEAR", 2023);
        bool mimic_on = envd("CP_MIMIC", 1) > 0;
        auto pstops = list("CP_PSTOP", "30,50,70");
        auto ptrails = list("CP_PTRAIL", "0,25,35,50,70");
        auto macts = list("CP_MACT", "60");
        auto mstops = list("CP_MSTOP", "16");
        auto mtrails = list("CP_MTRAIL", "22");

        if (!B.count(coin)) B[coin] = load(coin);
        const Bars& b = B[coin]; if (!b.N) { std::fprintf(stderr, "no data for %s\n", coin.c_str()); return 1; }
        auto ws = parent(b, W, thr);
        if (fromyear > 0) { std::vector<Window> fw; for (auto& w : ws) if (year_of(b.ts[w.ei]) >= fromyear) fw.push_back(w); ws = fw; }

        struct CClip { int64_t ts; double bp; bool is_mimic; int epi; };
        // one full campaign sim -> clips (parent bp in parent units, mimic bp in mimic units)
        auto run = [&](double pstop, double ptrail, double mact, double mstop, double mtrail,
                       double rt) -> std::vector<CClip> {
            std::vector<CClip> out;
            double fund_need = MFRAC * (mstop + rt + SLIP + RES);  // parent-unit bp mimic worst-case
            for (size_t wi = 0; wi < ws.size(); wi++) {
                const Window& w = ws[wi];
                bool popen = false, pdone = false; double pe = 0, phwm = 0, pstop_px = 0;
                bool mopen = false; double me = 0, mhwm = 0, mstop_px = 0; int mcount = 0;
                bool pulled = false;      // fresh-continuation state: pullback seen since HWM
                auto close_mimic = [&](int64_t ts, double px) {
                    out.push_back({ts, (px / me - 1.0) * 1e4 - rt, true, (int)wi}); mopen = false; };
                auto close_parent = [&](int64_t ts, double px) {
                    out.push_back({ts, (px / pe - 1.0) * 1e4 - rt, false, (int)wi}); popen = false; };
                for (int i = w.ei; i < w.xi; i++) {
                    double cl = b.c[i], lo = b.l[i]; int64_t ts = b.ts[i];
                    if (!popen) {
                        if (pdone) break;   // ONE parent entry per window — campaign over on parent exit
                        double fav = (cl / w.epx - 1.0) * 1e4;
                        int hr = (int)((b.ts[i] / 3600000) % 24);
                        if (H0 >= 0 && (hr < H0 || hr > H1g)) continue;   // outside session: no entry
                        if (fav >= CONF) {
                            if (DELAY > 0 && i + DELAY < w.xi) { cl = b.c[i + DELAY]; i += DELAY; }  // delayed-entry robustness
                            popen = true; pdone = true; pe = cl; phwm = cl; pstop_px = pe * (1.0 - pstop / 1e4); pulled = false; mcount = 0;
                        }
                        continue;
                    }
                    // 1) stops on bar low (mimic first: faster lot; then parent — parent stop ends campaign)
                    if (mopen && lo <= mstop_px) close_mimic(ts, mstop_px);
                    if (lo <= pstop_px) {
                        if (mopen) close_mimic(ts, std::min(cl, mstop_px > lo ? mstop_px : cl)); // campaign ends -> mimic flush
                        close_parent(ts, pstop_px);
                        continue;   // campaign over; no re-entry inside this window (parent lot is one entry)
                    }
                    // 2) close-driven updates
                    if (cl < phwm * (1.0 - RESET / 1e4)) pulled = true;
                    bool new_high = cl > phwm;
                    if (new_high) phwm = cl;
                    double pmfe = (phwm / pe - 1.0) * 1e4;
                    // parent ladder — geometry scales with the stop (spec anchor pstop=50:
                    // fee-BE arm +45, net-lock +90 (locks 0.4*stop net), trail active +100)
                    if (pmfe >= 0.9 * pstop) pstop_px = std::max(pstop_px, pe * (1.0 + (rt + 3.0) / 1e4));
                    if (pmfe >= 1.8 * pstop) pstop_px = std::max(pstop_px, pe * (1.0 + (rt + 0.4 * pstop) / 1e4));
                    if (ptrail > 0 && pmfe >= 2.0 * pstop) pstop_px = std::max(pstop_px, phwm * (1.0 - ptrail / 1e4));
                    // mimic ladder — spec anchor mstop=16: fee-BE +27, lock +50 (0.6*stop net), trail +60
                    if (mopen) {
                        if (cl > mhwm) mhwm = cl;
                        double mmfe = (mhwm / me - 1.0) * 1e4;
                        if (mmfe >= 1.7 * mstop) mstop_px = std::max(mstop_px, me * (1.0 + (rt + 3.0) / 1e4));
                        if (mmfe >= 3.1 * mstop) mstop_px = std::max(mstop_px, me * (1.0 + (rt + 0.6 * mstop) / 1e4));
                        if (mmfe >= 3.75 * mstop) mstop_px = std::max(mstop_px, mhwm * (1.0 - mtrail / 1e4));
                    }
                    // 3) mimic open: funded + activated + fresh continuation (pullback then new high)
                    if (mimic_on && !mopen && mcount < MMAX && new_high && pulled) {
                        double locked = (pstop_px / pe - 1.0) * 1e4 - rt;
                        if (pmfe >= mact && locked >= fund_need) {
                            mopen = true; me = cl; mhwm = cl; mstop_px = me * (1.0 - mstop / 1e4);
                            mcount++; pulled = false;
                        }
                    }
                    if (new_high) pulled = false;
                }
                // window end (parent reversal signal): flush both at last close
                int lastix = (w.xi - 1 >= w.ei) ? w.xi - 1 : w.ei;
                double lastpx = b.c[lastix];
                if (mopen) close_mimic(b.ts[lastix], lastpx);
                if (popen) close_parent(b.ts[lastix], lastpx);
            }
            return out;
        };

        auto summarize = [&](const std::vector<CClip>& rows, double& pnet, double& mnet, double& comb,
                             double& pf, int& pn, int& mn, double& worst, double& h1, double& h2,
                             double& medwin, double& mpf, double& mworst) {
            pnet = mnet = comb = worst = h1 = h2 = medwin = 0; pn = mn = 0; mpf = 0; mworst = 0;
            double gw = 0, gl = 0, mgw = 0, mgl = 0; std::vector<double> wins;
            std::vector<CClip> so = rows;
            std::sort(so.begin(), so.end(), [](const CClip& a, const CClip& c) { return a.ts < c.ts; });
            for (size_t k = 0; k < so.size(); k++) {
                const auto& r = so[k];
                double cb = r.is_mimic ? r.bp * MFRAC : r.bp;   // combined book in parent units
                comb += cb / 100.0;
                if (r.is_mimic) { mnet += r.bp / 100.0; mn++;
                    if (r.bp > 0) mgw += r.bp; else mgl -= r.bp;
                    if (r.bp < mworst) mworst = r.bp;
                } else { pnet += r.bp / 100.0; pn++; }
                if (cb > 0) { gw += cb; wins.push_back(r.bp); } else gl -= cb;
                if (cb < worst) worst = cb;
                if (k < so.size() / 2) h1 += cb / 100.0; else h2 += cb / 100.0;
            }
            pf = gl > 0 ? gw / gl : (gw > 0 ? 999 : 0);
            mpf = mgl > 0 ? mgw / mgl : (mgw > 0 ? 999 : 0);
            if (!wins.empty()) { std::sort(wins.begin(), wins.end()); medwin = wins[wins.size() / 2]; }
        };
        auto exbest = [&](const std::vector<CClip>& rows) {   // comb net minus best EPISODE (concentration gate)
            std::map<int, double> epi; double tot = 0, best = 0;
            for (auto& r : rows) { double cb = (r.is_mimic ? r.bp * MFRAC : r.bp) / 100.0; epi[r.epi] += cb; tot += cb; }
            for (auto& kv : epi) if (kv.second > best) best = kv.second;
            return tot - best;
        };

        // CP_RANDOM=N: beta/random-entry control — same window count + durations placed uniformly
        // at random (non-overlap), FIRST lever combo only, same session gate/confirm/costs. z vs cloud.
        if (int NSEED = (int)envd("CP_RANDOM", 0)) {
            double ps = pstops[0], pt = ptrails[0], ma = macts[0], mst = mstops[0], mt = mtrails[0];
            auto net_of = [&](const std::vector<CClip>& rows) {
                double s = 0; for (auto& r : rows) s += (r.is_mimic ? r.bp * MFRAC : r.bp) / 100.0; return s; };
            std::vector<Window> real_ws = ws;
            double actual = net_of(run(ps, pt, ma, mst, mt, RT));
            std::vector<int> durs; for (auto& w : real_ws) durs.push_back(w.xi - w.ei);
            int lo_bound = W + 1;
            if (fromyear > 0) while (lo_bound < b.N && year_of(b.ts[lo_bound]) < fromyear) lo_bound++;
            std::vector<double> nets;
            for (int seed = 0; seed < NSEED; seed++) {
                std::mt19937 rng(1000 + seed);
                std::vector<int> d = durs; std::shuffle(d.begin(), d.end(), rng);
                std::vector<std::pair<int,int>> placed; std::vector<Window> rws;
                for (int dur : d) for (int tries = 0; tries < 200; tries++) {
                    int lo = lo_bound, hi = b.N - 2 - dur; if (hi <= lo) break;
                    int ei = lo + (int)(rng() % (uint64_t)(hi - lo));
                    bool ok = true;
                    for (auto& pl : placed) if (ei < pl.second && ei + dur > pl.first) { ok = false; break; }
                    if (ok) { placed.push_back({ei, ei + dur}); rws.push_back({ei, ei + dur, b.o[ei], thr}); break; }
                }
                ws = rws; nets.push_back(net_of(run(ps, pt, ma, mst, mt, RT)));
            }
            ws = real_ws;
            double mu = 0; for (double v : nets) mu += v; mu /= nets.size();
            double sd = 0; for (double v : nets) sd += (v - mu) * (v - mu); sd = std::sqrt(sd / nets.size());
            int below = 0; for (double v : nets) if (v < actual) below++;
            std::printf("CAMPRAND %s W=%d thr=%.2f%% ps=%.0f pt=%.0f seeds=%d: actual=%+.0f%% rand_mu=%+.0f%% sd=%.0f z=%+.2f pct=%.1f%%\n",
                coin.c_str(), W, thr * 100, ps, pt, NSEED, actual, mu, sd, sd > 0 ? (actual - mu) / sd : 0,
                100.0 * below / nets.size());
            return 0;
        }

        std::printf("CAMPAIGN %s W=%d thr=%.1f%% conf=%.0fbp RT=%.0f slip=%.0f res=%.0f mfrac=%.2f reset=%.0f mmax=%d from%d windows=%zu%s\n",
            coin.c_str(), W, thr * 100, CONF, RT, SLIP, RES, MFRAC, RESET, MMAX, fromyear, ws.size(),
            mimic_on ? "" : "  [PARENT-ONLY]");
        std::printf("%5s %6s %5s %5s %5s | %4s %8s | %4s %8s %6s %7s | %8s %6s %8s | %8s %8s | %6s | %8s %8s\n",
            "pstop", "ptrail", "mact", "mstop", "mtrl", "pn", "pnet%", "mn", "mnet%", "mPF", "mworst", "comb%", "PF", "worst_bp", "H1%", "H2%", "medWin", "comb30%", "comb40%");  // +exBest col appended
        for (double ps : pstops) for (double pt : ptrails)
        for (double ma : macts) for (double mst : mstops) for (double mt : mtrails) {
            auto rows = run(ps, pt, ma, mst, mt, RT);
            double pnet, mnet, comb, pf, worst, h1, h2, medwin, mpf, mworst; int pn, mn;
            summarize(rows, pnet, mnet, comb, pf, pn, mn, worst, h1, h2, medwin, mpf, mworst);
            // stress re-sims: 30bp acceptance + 40bp (2x) — full re-sim, gate sees the cost
            auto r30 = run(ps, pt, ma, mst, mt, envd("CP_ST1", 30.0));
            auto r40 = run(ps, pt, ma, mst, mt, envd("CP_ST2", 40.0));
            double c30 = 0, c40 = 0;
            for (auto& r : r30) c30 += (r.is_mimic ? r.bp * MFRAC : r.bp) / 100.0;
            for (auto& r : r40) c40 += (r.is_mimic ? r.bp * MFRAC : r.bp) / 100.0;
            std::printf("%5.0f %6.0f %5.0f %5.0f %5.0f | %4d %+8.0f | %4d %+8.0f %6.2f %+7.0f | %+8.0f %6.2f %+8.0f | %+8.0f %+8.0f | %6.0f | %+8.0f %+8.0f | exB%+7.0f\n",
                ps, pt, ma, mst, mt, pn, pnet, mn, mnet, mpf, mworst, comb, pf, worst, h1, h2, medwin, c30, c40, exbest(rows));
        }
        return 0;
    }

    std::fprintf(stderr, "unknown mode %s\n", mode.c_str());
    return 1;
}
