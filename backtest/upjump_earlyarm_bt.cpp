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
#include <algorithm>
#include <random>

using chimera::UpJumpLadderCompanion;
static int env_cap(int dflt) {
    const char* e = getenv("UJW_CAP"); return e ? atoi(e) : dflt;
}
static const int64_t TF_MS = 3600LL * 1000;
static const int SMA_BARS = 200 * 24;

struct Bars { std::vector<int64_t> ts; std::vector<double> o, c, sma; int N = 0; };
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
        b.ts.push_back((int64_t)std::stoll(v[0])); b.o.push_back(std::stod(v[1])); b.c.push_back(std::stod(v[4])); }
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
        UpJumpLadderCompanion eng(c);
        int64_t cur_real_ts = 0;
        eng.set_on_clip([&](const UpJumpLadderCompanion::ClipRecord& rec) { out.push_back({cur_real_ts, rec.net_bp_real, epi}); });
        for (int i = w.ei; i < w.xi; i++) { cur_real_ts = b.ts[i]; eng.observe(true, w.epx, b.c[i], (int64_t)i * TF_MS); }
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

    std::fprintf(stderr, "unknown mode %s\n", mode.c_str());
    return 1;
}
