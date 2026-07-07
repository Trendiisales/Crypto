// ADA late-arm giveback fix — candidate sweep (S-2026-07-07 operator item 3).
//
// Incident: ADA companion legs opened 7 bars before event end at the local top
// (mfe 0.00 after open), event ended, MTM flush -122.12bp real x2 tiers = -244.2bp.
// Candidates gate the leg OPEN (the +be_bp trip) on event context:
//   (a) open_cutoff_bars   — no NEW open if bars-since-event-entry > N
//   (b) open_expand_k      — open only if the event made a new high within K bars
//   (c) open_max_peak_gb   — no open if giveback from event peak MFE > x
//
// Baseline = EXACT live spec (chimera-direct main.cpp make_be_companion):
//   be_floor, be_bp=20, T1 trail 20bp / T2 trail 150bp, cap 2, no ladder/reclip,
//   internal self-detector det_w=2 det_thr=0.01 (2h/+1%), tf 3600, RT 20bp.
// Drives the UNMODIFIED live header (UpJumpLadderCompanionLive.hpp, scp'd from
// chimera-direct @98c92d6) for the baseline; the X-variant (gates, default OFF)
// must reproduce the baseline byte-exact before any candidate row is trusted.
//
// Judged on the HONEST REAL column (net_bp_real = worse-of H1-close fill − 20bp RT).
// Model column reported alongside (dual-column). All-6 standalone gate on REAL:
// net>0, PF>1, WF both halves>0, bear>=0 (200d SMA regime at leg open).
//
// Build: g++ -O2 -std=c++17 latearm_bt.cpp -o latearm_bt   (run from backtest/, stdout -> /dev/null)
#include "UpJumpLadderCompanionLive.hpp"
#include "UpJumpLadderCompanionX.hpp"
#include <cstdio>
#include <cstring>
#include <cstdarg>
#include <fstream>
#include <sstream>
#include <string>
#include <vector>
#include <map>
#include <cmath>
#include <algorithm>

static const char* COINS[] = {"BTC","ETH","SOL","BNB","ADA","TRX","DOGE","NEAR","AAVE","OP"};
static const int NCOIN = 10;
static const int SMA_BARS = 200 * 24;

struct Bars { std::vector<int64_t> ts; std::vector<double> o, h, l, c; std::vector<double> sma; };

static Bars load(const std::string& coin) {
    Bars b;
    std::ifstream f("data/" + coin + "USDT_1h.csv");
    std::string ln; std::getline(f, ln);
    while (std::getline(f, ln)) {
        std::stringstream ss(ln); std::string t; std::vector<std::string> v;
        while (std::getline(ss, t, ',')) v.push_back(t);
        if (v.size() < 5) continue;
        b.ts.push_back(std::stoll(v[0])); b.o.push_back(std::stod(v[1]));
        b.h.push_back(std::stod(v[2]));   b.l.push_back(std::stod(v[3]));
        b.c.push_back(std::stod(v[4]));
    }
    const int N = (int)b.c.size();
    b.sma.assign(N, 0.0); double run = 0.0;
    for (int i = 0; i < N; ++i) {
        run += b.c[i];
        if (i >= SMA_BARS) run -= b.c[i - SMA_BARS];
        if (i >= SMA_BARS - 1) b.sma[i] = run / SMA_BARS;
    }
    return b;
}

struct Clip { int64_t exit_ts; double model, real; bool bull; std::string reason, tag; int bars_held; };

struct Stats {
    int n = 0, nneg = 0;
    double net = 0, model = 0, gw = 0, gl = 0, h1 = 0, h2 = 0, bull = 0, bear = 0, worst = 0;
    bool pass = false;
};

static Stats stats(std::vector<Clip>& v) {
    Stats s;
    if (v.empty()) return s;
    std::vector<int64_t> ex; ex.reserve(v.size());
    for (auto& c : v) ex.push_back(c.exit_ts);
    std::sort(ex.begin(), ex.end());
    const int64_t mid = ex[ex.size() / 2];
    for (auto& c : v) {
        s.n++; s.net += c.real; s.model += c.model;
        if (c.real > 0) s.gw += c.real; else { s.gl -= c.real; s.nneg++; }
        if (c.exit_ts < mid) s.h1 += c.real; else s.h2 += c.real;
        if (c.bull) s.bull += c.real; else s.bear += c.real;
        if (c.real < s.worst) s.worst = c.real;
    }
    const double pf = s.gl > 0 ? s.gw / s.gl : (s.gw > 0 ? 999.0 : 0.0);
    s.pass = (s.net > 0 && pf > 1 && s.h1 > 0 && s.h2 > 0 && s.bear >= 0);
    return s;
}
static double pf_of(const Stats& s) { return s.gl > 0 ? s.gw / s.gl : (s.gw > 0 ? 999.0 : 0.0); }

// run one coin through ENG (live or X). Returns clips (regime tagged at leg-open bar).
template <typename ENG>
static std::vector<Clip> run_coin(const Bars& b, typename ENG::Config cfg) {
    std::vector<Clip> out;
    ENG eng(cfg);
    const Bars* bp = &b;
    eng.set_on_clip([&out, bp](const typename ENG::ClipRecord& r) {
        Clip c; c.exit_ts = r.exit_ts_ms; c.model = r.net_bp; c.real = r.net_bp_real;
        c.reason = r.reason; c.tag = r.tag; c.bars_held = r.bars_held;
        // regime at leg open: locate open bar by ts (bars are hourly, ts = open_time_ms)
        size_t i = (size_t)std::min<int64_t>((int64_t)bp->ts.size() - 1,
                     std::max<int64_t>(0, (r.entry_ts_ms - bp->ts.front()) / 3600000LL));
        c.bull = (bp->sma[i] > 0.0 && bp->c[i] > bp->sma[i]);
        out.push_back(c);
    });
    for (size_t i = 0; i < b.c.size(); ++i)
        eng.observe(true, 1.0, b.c[i], b.ts[i]);
    return out;
}

template <typename ENG>
static typename ENG::Config live_cfg(const std::string& coin) {
    typename ENG::Config c;
    c.parent_tag = coin + "-UPJUMP-H1"; c.tag = coin + "-UPJUMP-CLIP";
    c.symbol = coin;
    c.tight = {0, 0, 0, 20.0};   // Tier{arm,stall,gb,trail_bp}
    c.wide  = {0, 0, 0, 150.0};
    c.reclip_pct = 0.0; c.cap = 2;
    c.be_floor = true; c.be_bp = 20.0;
    c.det_w = 2; c.det_thr = 0.01;
    c.tf_secs = 3600; c.round_trip_bp = 20.0;
    return c;
}

struct Variant { std::string name; int cutoff; int expand; double maxgb;
                 double be_bp = 20.0; double t1_trail = 20.0; double t2_trail = 150.0; };

// ── intrabar-stop sim (the "per-tick stop evaluation" bound, S-2026-07-07 item-3 follow-up) ──
// Replicates the live be_floor book exactly EXCEPT exits: instead of waiting for the H1
// CLOSE that tripped the stop (live det-mode behavior — the proven −1.13Mbp slip), the
// stop is checked against each bar's LOW using the stop computed from PRIOR bars' hwm
// (conservative: no same-bar ratchet), fill = min(bar open, stop) (gap-through honest).
// Detector + leg opens stay H1-close-driven (identical cadence to live). close_mode=true
// must reproduce the live-header baseline Σ (self-validation) before intrabar rows count.
struct BeSim {
    struct SLeg { bool open = false; double le = 0, hwm = 0, ref = 0, trail = 0; int64_t open_ts = 0; };
    static std::vector<Clip> run(const Bars& b, double be_bp, double t1, double t2,
                                 double rt_bp, bool close_mode) {
        std::vector<Clip> out;
        const int N = (int)b.c.size();
        std::vector<double> h1c;                       // det ring (w=2 -> 3 closes)
        bool det_in = false; double det_entry = 0;
        double ev_ref = 0;                             // current event entry px (leg session key)
        std::vector<SLeg> legs;
        auto book = [&](int i, double le, double fill, int64_t open_ts) {
            Clip c; c.exit_ts = b.ts[i];
            c.real = (fill / le - 1.0) * 1e4 - rt_bp;
            c.model = 0; c.reason = "SIM"; c.bars_held = 0;
            size_t oi = (size_t)std::min<int64_t>(N - 1, std::max<int64_t>(0, (open_ts - b.ts.front()) / 3600000LL));
            c.bull = (b.sma[oi] > 0.0 && b.c[oi] > b.sma[oi]);
            out.push_back(c);
        };
        // process bar i-1 close (det), then bar i is the "live" bar for intrabar exits
        for (int i = 1; i < N; ++i) {
            // 1) detector on the PRIOR bar's close (mirrors process_close_ cadence)
            const double close = b.c[i - 1];
            h1c.push_back(close);
            if ((int)h1c.size() > 3) h1c.erase(h1c.begin());
            if ((int)h1c.size() >= 3) {
                const double j = close / h1c.front() - 1.0;
                if (!det_in && j >= 0.01) { det_in = true; det_entry = close; }
                else if (det_in && j <= -0.01) det_in = false;
            }
            // 2) drive the book with (det_in, det_entry, close) exactly like observe_be_
            if (!det_in || det_entry <= 0.0) {
                for (auto& lg : legs) if (lg.open) book(i - 1, lg.le, close, lg.open_ts);   // flush MTM at close
                legs.clear(); ev_ref = 0;
            } else {
                if (ev_ref != det_entry) {
                    for (auto& lg : legs) if (lg.open) book(i - 1, lg.le, close, lg.open_ts);
                    legs.clear(); ev_ref = det_entry;
                    legs.push_back({false, 0, 0, det_entry, t1, 0});
                    legs.push_back({false, 0, 0, det_entry, t2, 0});
                }
                for (auto& lg : legs) {
                    if (!lg.open) {
                        if ((close / lg.ref - 1.0) * 1e4 < be_bp) continue;
                        lg.open = true; lg.le = close; lg.hwm = close; lg.open_ts = b.ts[i - 1];
                        continue;                                    // opened this close; no same-bar exit
                    }
                    if (close_mode) {
                        if (close > lg.hwm) { lg.hwm = close; continue; }
                        const double stop = std::max(lg.le, lg.hwm * (1.0 - lg.trail / 1e4));
                        if (close <= stop) { book(i - 1, lg.le, close, lg.open_ts); lg.ref = stop; lg.open = false; }
                    } else {
                        // intrabar: stop from PRIOR hwm vs THIS bar's low; fill min(open, stop)
                        const double stop = std::max(lg.le, lg.hwm * (1.0 - lg.trail / 1e4));
                        if (b.l[i - 1] <= stop) {
                            book(i - 1, lg.le, std::min(b.o[i - 1], stop), lg.open_ts);
                            lg.ref = stop; lg.open = false;
                        } else if (b.h[i - 1] > lg.hwm) lg.hwm = b.h[i - 1];
                    }
                }
            }
        }
        return out;
    }
};

int main() {
    std::map<std::string, Bars> data;
    for (int i = 0; i < NCOIN; ++i) data[COINS[i]] = load(COINS[i]);

    FILE* rep = fopen("latearm_report.txt", "w");
    auto P = [&](const char* fmt, ...) {
        va_list a; va_start(a, fmt); vfprintf(stderr, fmt, a); va_end(a);
        va_start(a, fmt); vfprintf(rep, fmt, a); va_end(a);
    };

    // ── fidelity: X gates-off must equal live header byte-exact, per coin ──
    P("=== FIDELITY: X(gates off) vs LIVE header ===\n");
    std::map<std::string, std::vector<Clip>> base;
    bool fid_ok = true;
    for (int i = 0; i < NCOIN; ++i) {
        const std::string coin = COINS[i];
        auto a = run_coin<chimera::UpJumpLadderCompanion>(data[coin], live_cfg<chimera::UpJumpLadderCompanion>(coin));
        auto x = run_coin<chimerax::UpJumpLadderCompanion>(data[coin], live_cfg<chimerax::UpJumpLadderCompanion>(coin));
        double sa = 0, sx = 0;
        for (auto& c : a) sa += c.real;
        for (auto& c : x) sx += c.real;
        const bool ok = (a.size() == x.size() && std::fabs(sa - sx) < 1e-6);
        if (!ok) fid_ok = false;
        P("  %-5s n=%zu/%zu real=%+.2f/%+.2f %s\n", coin.c_str(), a.size(), x.size(), sa, sx, ok ? "OK" : "MISMATCH");
        base[coin] = std::move(a);
    }
    if (!fid_ok) { P("FIDELITY FAIL — abort\n"); return 1; }

    // ── replicate the live ADA incident (exit_ts 1783378800000, real -122.12 x2) ──
    P("\n=== ADA incident replication (live ledger: ENGINE_EXIT real=-122.12 x2 @1783378800000) ===\n");
    for (auto& c : base["ADA"])
        if (llabs(c.exit_ts - 1783378800000LL) <= 3600000LL && c.reason == "ENGINE_EXIT")
            P("  %s %s real=%+.2f model=%+.2f bars=%d\n", c.tag.c_str(), c.reason.c_str(), c.real, c.model, c.bars_held);

    // ── variants ──
    std::vector<Variant> vars = {
        {"BASELINE(live)", 0, 0, 0.0},
        {"a:cutoff=2",  2, 0, 0.0}, {"a:cutoff=3",  3, 0, 0.0}, {"a:cutoff=4",  4, 0, 0.0},
        {"a:cutoff=6",  6, 0, 0.0}, {"a:cutoff=8",  8, 0, 0.0}, {"a:cutoff=12", 12, 0, 0.0},
        {"b:expand=1",  0, 1, 0.0}, {"b:expand=2",  0, 2, 0.0}, {"b:expand=3",  0, 3, 0.0},
        {"b:expand=4",  0, 4, 0.0}, {"b:expand=6",  0, 6, 0.0},
        {"c:maxgb=20%", 0, 0, 0.20}, {"c:maxgb=30%", 0, 0, 0.30}, {"c:maxgb=50%", 0, 0, 0.50},
        // supplementary structural levers (not item-3 candidates; answers "then what?")
        {"s:dropT1",       0, 0, 0.0, 20.0,  0.0, 150.0},   // wide runner only (t1_trail=0 -> pure BE floor? no: 0 = ride; set below)
        {"s:trail300",     0, 0, 0.0, 20.0,  0.0, 300.0},   // single leg, 3% trail
        {"s:trail500",     0, 0, 0.0, 20.0,  0.0, 500.0},   // single leg, 5% trail
        {"s:be100_t150",   0, 0, 0.0, 100.0, 0.0, 150.0},   // open needs +1%, single 1.5% trail
        {"s:be100_t300",   0, 0, 0.0, 100.0, 0.0, 300.0},
        {"s:be200_t500",   0, 0, 0.0, 200.0, 0.0, 500.0},   // open +2% (the proven arm-2), 5% trail
        {"s:be200_t500_c4",4, 0, 0.0, 200.0, 0.0, 500.0},   // + late-open cutoff 4 bars
    };

    P("\n=== SWEEP — REAL column (net = worse-of fill - 20bp RT); model shown for reference ===\n");
    P("%-15s %-5s %6s %9s %8s %6s %8s %8s %8s %8s %8s %6s  %s\n",
      "variant", "coin", "n", "netREAL", "model", "PF", "H1", "H2", "bull", "bear", "worst", "neg", "all6");

    for (auto& v : vars) {
        Stats roster; std::vector<Clip> all;
        std::map<std::string, Stats> per;
        for (int i = 0; i < NCOIN; ++i) {
            const std::string coin = COINS[i];
            std::vector<Clip> clips;
            if (v.cutoff == 0 && v.expand == 0 && v.maxgb == 0.0 &&
                v.be_bp == 20.0 && v.t1_trail == 20.0 && v.t2_trail == 150.0) clips = base[coin];
            else {
                auto cfg = live_cfg<chimerax::UpJumpLadderCompanion>(coin);
                cfg.open_cutoff_bars = v.cutoff; cfg.open_expand_k = v.expand; cfg.open_max_peak_gb = v.maxgb;
                cfg.be_bp = v.be_bp;
                if (v.t1_trail <= 0.0) { cfg.cap = 1; cfg.tight = {0, 0, 0, v.t2_trail}; cfg.wide = {0, 0, 0, v.t2_trail}; }
                else { cfg.tight = {0, 0, 0, v.t1_trail}; cfg.wide = {0, 0, 0, v.t2_trail}; }
                clips = run_coin<chimerax::UpJumpLadderCompanion>(data[coin], cfg);
            }
            per[coin] = stats(clips);
            for (auto& c : clips) all.push_back(c);
        }
        Stats tot = stats(all);
        for (int i = 0; i < NCOIN; ++i) {
            const Stats& s = per[COINS[i]];
            P("%-15s %-5s %6d %9.1f %8.1f %6.2f %8.1f %8.1f %8.1f %8.1f %8.1f %6d  %s\n",
              v.name.c_str(), COINS[i], s.n, s.net, s.model, pf_of(s), s.h1, s.h2, s.bull, s.bear, s.worst, s.nneg,
              s.pass ? "PASS" : "fail");
        }
        P("%-15s %-5s %6d %9.1f %8.1f %6.2f %8.1f %8.1f %8.1f %8.1f %8.1f %6d  %s   <-- ROSTER\n\n",
          v.name.c_str(), "ALL", tot.n, tot.net, tot.model, pf_of(tot), tot.h1, tot.h2, tot.bull, tot.bear,
          tot.worst, tot.nneg, tot.pass ? "PASS" : "fail");
    }
    // ── intrabar-stop bound (BeSim). Row 1 (close-mode) MUST reproduce the baseline ──
    P("=== INTRABAR-STOP BOUND (BeSim; y:selfcheck must equal BASELINE per coin) ===\n");
    struct YVar { std::string name; double be, t1, t2; bool close_mode; };
    std::vector<YVar> yvars = {
        {"y:selfcheck(close)", 20, 20, 150, true},
        {"y:intrabar(live)",   20, 20, 150, false},
        {"y:ib_be100_t300",   100,  0, 300, false},
        {"y:ib_be200_t500",   200,  0, 500, false},
    };
    for (auto& y : yvars) {
        std::vector<Clip> all;
        for (int i = 0; i < NCOIN; ++i) {
            const std::string coin = COINS[i];
            auto clips = BeSim::run(data[coin], y.be, y.t1 > 0 ? y.t1 : y.t2, y.t2, 20.0, y.close_mode);
            Stats s = stats(clips);
            P("%-19s %-5s %6d %9.1f %8s %6.2f %8.1f %8.1f %8.1f %8.1f %8.1f %6d  %s\n",
              y.name.c_str(), coin.c_str(), s.n, s.net, "-", pf_of(s), s.h1, s.h2, s.bull, s.bear, s.worst, s.nneg,
              s.pass ? "PASS" : "fail");
            for (auto& c : clips) all.push_back(c);
        }
        Stats tot = stats(all);
        P("%-19s %-5s %6d %9.1f %8s %6.2f %8.1f %8.1f %8.1f %8.1f %8.1f %6d  %s   <-- ROSTER\n\n",
          y.name.c_str(), "ALL", tot.n, tot.net, "-", pf_of(tot), tot.h1, tot.h2, tot.bull, tot.bear,
          tot.worst, tot.nneg, tot.pass ? "PASS" : "fail");
    }
    fclose(rep);
    fprintf(stderr, "report -> latearm_report.txt\n");
    return 0;
}
