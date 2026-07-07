#pragma once
// ─────────────────────────────────────────────────────────────────────────────
// UpJumpLadderCompanion — TIERED-2 + SELF-FUNDING LADDER clip book (S-2026-07-05b).
//
// Successor to UpJumpCompanionEngine (single-leg). Same STANDALONE ADDITIVE,
// observe-only, shadow contract — but every parent UPJUMP trade now runs a BOOK
// of independent clip legs:
//
//   • 2 BASE tiers from entry: a TIGHT tier (banks cost fast) + a WIDE tier
//     (rides far). ">= 2 engines per trade" is the operator floor.
//   • SELF-FUNDING LADDER: each time a leg banks a COST-COVERED clip (net_bp>0),
//     ONE more WIDE leg is opened at the clip price to ride the continuation,
//     up to `cap` concurrent legs total (2 base + up to cap-2 ladder). The clip
//     that funds it already paid its own cost (opt C — no free capital added).
//   • Each leg independently exits on STALL (N bars no new fav high) and/or
//     REVERSAL (giveback fraction of peak) — per-tier FREE lever (>=1 on) — and
//     RE-CLIPS (re-ENTERs at the current price) if the trend resumes.
//   • Optional HARD COST-COVER GATE (per-coin, e.g. AAVE): a leg may not bank a
//     clip whose gross does not clear RT cost; it keeps holding instead. The
//     parent-exit flush is ALWAYS marked-to-market (never abandoned) so no
//     underwater leg is hidden.
//
// HARD OPERATOR RULE — SEPARATE INDEPENDENT ENGINE. Never modifies / closes /
// moves / shrinks the parent. Judge STANDALONE (net>0, PF>1, WF both halves,
// bear>=0), NEVER vs-WIDE (feedback-companion-independent-engine).
//
// FAITHFUL byte-exact port of crypto_upjump_tiered_ladder_sweep.py (Leg + run_trade):
//   - fav / mfe / arm / reclip gauged from the leg's FIXED entry epx.
//   - clip gross_bp measured from the MOVING `le` (= epx, then reset to the clip
//     price on each reclip → "reclip = re-enter"). entry_px in the ClipRecord = le.
//   - ladder legs anchor epx=le=clip_price, WIDE params; newborn legs do NOT step
//     the bar they are born (added after the per-leg loop, exactly like python).
//   - flush open (not clipped) legs at the last observed price on parent exit.
//
// Cost 0.20% RT = 20bp (Binance spot taker). PAPER/SHADOW: emits its own
// ClipRecord ledger only, never places an order, never calls back into the parent.
// ─────────────────────────────────────────────────────────────────────────────
#include <string>
#include <cstdint>
#include <functional>
#include <cstdio>
#include <utility>
#include <vector>
#include <algorithm>

namespace chimera {

class UpJumpLadderCompanion {
public:
    struct Tier { double arm = 5.0; int stall = 0; double gb = 0.0; double trail_bp = 0.0; };  // 0 = that lever OFF; trail_bp used only in be_floor mode

    struct Config {
        std::string parent_tag;        // e.g. "BTC-UPJUMP-H1" (the leg we observe)
        std::string tag;               // e.g. "BTC-UPJUMP-CLIP" (our own ledger tag)
        std::string symbol;            // e.g. "btcusdt"
        Tier    tight;                 // base tier 1 (tight)
        Tier    wide;                  // base tier 2 (wide) — ALSO the ladder-leg params
        double  reclip_pct    = 0.05;  // re-enter when fav > prior_peak*(1+reclip_pct)
        int     cap           = 5;     // max concurrent legs (2 base + up to cap-2 ladder)
        double  cost_gate_bp  = 0.0;   // >0 = hard cost-cover clip gate (suppress sub-cost clips)
        double  confirm_bp    = 25.0;  // OPTION-B confirmed-entry: a leg stays FLAT (books nothing,
                                       // pays no cost) until fav>=confirm_bp; a never-confirmed leg
                                       // never opens (fixes the BTC -141.39bp never-positive flush).
        int64_t tf_secs       = 3600;  // H1
        double  round_trip_bp = 20.0;  // 0.20% RT Binance spot taker
        // ── BE-FLOOR mode (S-2026-07-05 resume, operator restated spec) ──────────
        // When true the leg exits are governed by a HARD BREAK-EVEN FLOOR instead of
        // giveback/stall + MTM flush. Faithful port of be_bptrail.py leg_book:
        //   • a leg stays FLAT until price clears +be_bp from its ref (== RT cost);
        //     it OPENS at that price (le), so net starts at 0 (the move paid the cost).
        //   • stop = max(le, hwm*(1 - trail_bp/1e4)); exit the instant cur<=stop.
        //     exit_px = stop >= le ALWAYS -> gross>=0 -> net = gross (NO 2nd RT charge,
        //     the +be_bp open move already paid it). NO underwater flush, EVER.
        //   • reclip: after an exit the ref becomes the exit price; the leg re-opens on
        //     the next +be_bp continuation. NO self-funding ladder (operator dropped it).
        // Result: net_bp >= 0 on EVERY clip BY CONSTRUCTION. Trigger (W/thr) lives in
        // the dedicated shadow detector engine the companion observes — parent untouched.
        bool    be_floor      = false;
        double  be_bp         = 20.0;  // gross bp move from ref required to open a leg (== cost)
        // Internal up-jump detector (be_floor only). >0 = the companion self-detects its
        // OWN long-event window from the price stream it already receives — it does NOT
        // read the live parent's position (independence: the parent is never retuned for
        // the companion's sake). Faithful to sw.parent(W,thr) on H1 closes; enter when
        // close/close[-W]-1 >= det_thr, exit when <= -det_thr. det_w=2/det_thr=0.01 =
        // the swept "2h/+1%" trigger. W=2 -> ~2h cold-start on restart (no warm-seed
        // needed; the companion's per-trade legs are ephemeral anyway).
        int     det_w         = 0;
        double  det_thr       = 0.0;
    };

    // Emitted on every clip / engine-exit. main.cpp persists to the companion ledger.
    struct ClipRecord {
        std::string tag, symbol, reason;   // STALL_CLIP / REVERSAL_CLIP / ENGINE_EXIT
        int64_t entry_ts_ms = 0, exit_ts_ms = 0;
        double  entry_px = 0.0, exit_px = 0.0;
        double  gross_bp = 0.0, net_bp = 0.0, mfe_pct = 0.0;
        // S-2026-07-07f HONEST REAL COLUMN (Omega befloor-family real-fill audit): the be_floor
        // model column books fill-at-floor + max(0,.) + net=gross (no cost) — an accounting
        // tautology (neg=0 by construction), proven -1.13M bp real vs +3.6M bp model on 2021-26
        // Binance H1 across the 10-coin roster. These fields carry the worse-of fill (the H1
        // close that tripped the stop) minus the 20bp RT cost. Judge the book on THESE.
        double  gross_bp_real = 0.0, net_bp_real = 0.0;
        int     bars_held = 0, clip_num = 0;
        bool    shadow = true;
    };
    using ClipCallback = std::function<void(const ClipRecord&)>;

    explicit UpJumpLadderCompanion(Config c) : cfg_(std::move(c)) {}

    void set_on_clip(ClipCallback cb) { on_clip_ = std::move(cb); }
    const Config& config() const { return cfg_; }
    bool  is_open() const { for (auto& l : legs_) if (l.open) return true; return false; }
    int   clips()   const { return clip_num_; }
    void  rehydrate(int clips_total, double bank_bp_total, double bank_bp_real_total = 0.0) {
        clip_num_ = clips_total; banked_bp_ = bank_bp_total; banked_bp_real_ = bank_bp_real_total; }
    bool  shadow_mode = true;

    // ── one independent clip leg (faithful python Leg) ─────────────────────
    struct Leg {
        std::string label;      // "T1" / "T2" / "L1".. (tier / ladder id for the GUI)
        double  epx = 0.0;      // FIXED entry — fav/mfe/arm/reclip gauge
        double  le  = 0.0;      // MOVING leg entry — clip gross gauge (resets on reclip)
        double  arm = 5.0; int stall = 0; double gb = 0.0; double rc = 0.05; double cg = 0.0; double confirm = 0.0;
        bool    open = false, clipped = false;
        double  pk = 0.0, mfe = 0.0;
        int64_t ext_bar = 0, open_bar = 0, open_ts = 0;
        // BE-floor mode state
        double  trail_bp = 0.0;   // bp giveback-from-hwm (0 = ride to parent exit)
        double  hwm = 0.0;        // high-water price since open
        double  ref_px = 0.0;     // reference the +be_bp open gate is measured from (resets to exit px on reclip)
    };

    // Drive ONCE per completed parent bar (byte-exact vs python) OR per tick
    // (intra-bar; bar index = ts/H1 only advances hourly so STALL stays H1-quantised
    // and REVERSAL/RECLIP price gates fire the instant they trip). Reads the parent's
    // settled position only, never writes to it. Long-only (UPJUMP is always long).
    void observe(bool parent_in_pos, double parent_entry_px, double cur_px, int64_t ts_ms) {
        const int64_t bar = ts_ms / (cfg_.tf_secs * 1000);
        if (cfg_.be_floor) {
            if (cfg_.det_w > 0) { feed_selfdetect_(cur_px, ts_ms); return; }        // internal detector (live)
            observe_be_(parent_in_pos, parent_entry_px, cur_px, ts_ms, bar);         // external window (validation harness)
            return;
        }

        // Parent flat / no valid mark -> flush every open leg MTM, then reset.
        if (!parent_in_pos || parent_entry_px <= 0.0 || cur_px <= 0.0) {
            const double px = (cur_px > 0.0) ? cur_px : entry_ref_;
            for (auto& lg : legs_) flush_leg_(lg, px, ts_ms, bar);
            reset_session_();
            return;
        }

        // New parent trade -> flush any stragglers, reset, seed the 2 base legs.
        if (entry_ref_ != parent_entry_px) {
            for (auto& lg : legs_) flush_leg_(lg, cur_px, ts_ms, bar);
            reset_session_();
            entry_ref_ = parent_entry_px;
            init_base_legs_(parent_entry_px, ts_ms, bar);
        }

        // Step every leg; ladder-spawn on cost-covered clips (newborns added AFTER
        // the loop so they do not step the bar they are born — matches python).
        std::vector<Leg> spawn;
        for (auto& lg : legs_) {
            double gross; const char* reason;
            if (step_leg_(lg, bar, cur_px, gross, reason)) {
                const double net = gross - cfg_.round_trip_bp;
                emit_clip_(lg, cur_px, ts_ms, bar, gross, net, reason);
                if (net > 0.0 && (int)(legs_.size() + spawn.size()) < cfg_.cap)
                    spawn.push_back(make_leg_(next_ladder_label_(legs_.size() + spawn.size()),
                                              cur_px, cfg_.wide, ts_ms, bar, /*seed_open=*/false));
            }
        }
        for (auto& l : spawn) legs_.push_back(std::move(l));
    }

    // Rehydrate an OPEN book from a live parent on restart (S-2026-07-05). Per-session
    // leg state is ephemeral, so we re-seed the 2 BASE legs open+armed from the parent
    // peak (any pre-restart ladder legs are lost — their banked clips already persisted
    // to the durable log; open ladder legs would have been flushed anyway). Documented
    // reset, same philosophy as the single-leg engine's seed_open.
    void seed_open(double entry_px, int64_t entry_ts_ms, double peak_px, int64_t now_ms) {
        if (!legs_.empty() || entry_px <= 0.0) return;
        const int64_t ebar = entry_ts_ms / (cfg_.tf_secs * 1000);
        const int64_t nbar = now_ms      / (cfg_.tf_secs * 1000);
        const double  peak_mfe = (peak_px > entry_px) ? (peak_px / entry_px - 1.0) * 100.0 : 0.0;
        entry_ref_ = entry_px;
        cur_bar_   = nbar;   // anchor "now" so the first post-rehydrate snapshot() before any
                             // observe_be_/step reports bars_since_high = nbar - ext_bar(=nbar) = 0
                             // (was 0 - nbar = -495347: the desk CRYPTO COMPANIONS -495347 render bug)
        legs_.push_back(seed_leg_("T1", entry_px, cfg_.tight, entry_ts_ms, ebar, nbar, peak_mfe));
        legs_.push_back(seed_leg_("T2", entry_px, cfg_.wide,  entry_ts_ms, ebar, nbar, peak_mfe));
    }

    // ── live snapshots for the Omega desk CRYPTO COMPANIONS panel ──────────
    struct LiveSnap {
        std::string label;                // "" = book aggregate, else per-leg id
        bool   open = false, armed = false;
        double peak_mfe_pct = 0.0;
        int    bars_since_high = 0;
        int    clips = 0;                 // book-level (durable)
        double bank_bp = 0.0;             // book-level (durable) — MODEL column (reference only)
        double bank_bp_real = 0.0;        // book-level (durable) — HONEST real-fill column (fold THIS)
    };
    LiveSnap snapshot() const {           // book aggregate (back-compat)
        LiveSnap s; s.clips = clip_num_; s.bank_bp = banked_bp_; s.bank_bp_real = banked_bp_real_;
        for (const auto& lg : legs_) {
            if (!lg.open) continue;
            s.open = true;
            if (lg.mfe >= lg.arm) s.armed = true;
            if (lg.mfe > s.peak_mfe_pct) { s.peak_mfe_pct = lg.mfe; s.bars_since_high = (int)std::max<int64_t>(0, cur_bar_ - lg.ext_bar); }
        }
        return s;
    }
    std::vector<LiveSnap> leg_snapshots() const {   // per-leg (multi-leg GUI)
        std::vector<LiveSnap> v;
        for (const auto& lg : legs_) {
            if (!lg.open) continue;
            LiveSnap s; s.label = lg.label; s.open = true;
            s.armed = (lg.mfe >= lg.arm); s.peak_mfe_pct = lg.mfe;
            s.bars_since_high = (int)std::max<int64_t>(0, cur_bar_ - lg.ext_bar);
            v.push_back(s);
        }
        return v;
    }

private:
    // ── internal up-jump detector (be_floor self-detect; parent never read) ──
    // Aggregates H1 closes from the mark stream; a bar "closes" when the next bar's
    // first mark arrives. On each completed close: run sw.parent(det_w,det_thr) and
    // drive the leg book with the detector's own (in_event, entry_px) window.
    void feed_selfdetect_(double cur_px, int64_t ts_ms) {
        if (cur_px <= 0.0) return;
        const int64_t bar = ts_ms / (cfg_.tf_secs * 1000);
        if (det_bar_ < 0) { det_bar_ = bar; det_close_ = cur_px; return; }
        if (bar < det_bar_) return;                             // ignore stale/backward feeds (2nd driver, rehydrate replay)
        if (bar == det_bar_) { det_close_ = cur_px; return; }   // same bar -> update running close
        process_close_(det_close_, det_bar_);                   // prior bar finalized
        det_bar_ = bar; det_close_ = cur_px;
    }
    void process_close_(double close, int64_t closed_bar) {
        h1c_.push_back(close);
        if ((int)h1c_.size() > cfg_.det_w + 1) h1c_.erase(h1c_.begin());
        if ((int)h1c_.size() >= cfg_.det_w + 1) {               // have close[i] and close[i-W]
            const double past = h1c_.front();
            const double j = close / past - 1.0;
            if (!det_in_ && j >=  cfg_.det_thr) { det_in_ = true; det_entry_ = close; }  // enter (entry ~ next open)
            else if (det_in_ && j <= -cfg_.det_thr) { det_in_ = false; }                 // exit -> book flushes
        }
        const int64_t ts = closed_bar * cfg_.tf_secs * 1000;
        observe_be_(det_in_, det_entry_, close, ts, closed_bar);   // drive the BE-floor book
    }

    // ── BE-FLOOR mode (faithful port of be_bptrail.py leg_book) ──────────────
    // net_bp >= 0 on EVERY clip by construction; no ladder; reclip from exit px.
    void observe_be_(bool parent_in_pos, double parent_entry_px, double cur_px, int64_t ts_ms, int64_t bar) {
        cur_bar_ = bar;
        // Parent flat / bad mark -> flush every open leg (floored >=0), reset.
        if (!parent_in_pos || parent_entry_px <= 0.0 || cur_px <= 0.0) {
            const double px = (cur_px > 0.0) ? cur_px : last_be_px_;
            for (auto& lg : legs_) flush_be_(lg, px, ts_ms, bar);
            reset_session_();
            return;
        }
        // New parent trade -> flush stragglers, reset, seed 2 base legs (ref = parent entry).
        if (entry_ref_ != parent_entry_px) {
            for (auto& lg : legs_) flush_be_(lg, cur_px, ts_ms, bar);
            reset_session_();
            entry_ref_ = parent_entry_px;
            legs_.push_back(make_be_leg_("T1", parent_entry_px, cfg_.tight));
            legs_.push_back(make_be_leg_("T2", parent_entry_px, cfg_.wide));
        }
        last_be_px_ = cur_px;
        for (auto& lg : legs_) step_be_(lg, cur_px, ts_ms, bar);   // NO ladder spawn in be_floor
    }

    Leg make_be_leg_(std::string label, double ref_px, const Tier& t) {
        Leg l; l.label = std::move(label);
        l.ref_px = ref_px; l.trail_bp = t.trail_bp; l.arm = t.arm;
        l.open = false; l.clipped = false; l.le = 0.0; l.hwm = 0.0;
        return l;
    }

    // one H1-close step of a BE-floor leg. Books a clip (net>=0) when the trailing
    // stop (floored at entry) is hit; reclips from the exit price.
    void step_be_(Leg& lg, double cur, int64_t ts, int64_t bar) {
        if (!lg.open) {                                     // FLAT: wait for +be_bp from ref -> open here
            if ((cur / lg.ref_px - 1.0) * 1e4 < cfg_.be_bp) return;
            lg.open = true; lg.le = cur; lg.hwm = cur;
            lg.open_ts = ts; lg.open_bar = bar; lg.ext_bar = bar; lg.mfe = 0.0;
            return;                                         // opened this bar; stop==le, cur==le -> no exit yet
        }
        if (cur > lg.hwm) { lg.hwm = cur; lg.ext_bar = bar; }
        lg.mfe = (lg.hwm / lg.le - 1.0) * 100.0;            // peak % from entry (for the GUI snapshot)
        const double stop = (lg.trail_bp > 0.0)
            ? std::max(lg.le, lg.hwm * (1.0 - lg.trail_bp / 1e4))
            : lg.le;                                        // trail_bp==0 -> pure BE floor (ride to parent exit)
        if (cur <= stop) {
            const double gross = std::max(0.0, (stop / lg.le - 1.0) * 1e4);   // >=0 ALWAYS (MODEL column, reference only)
            const double gross_real = (cur / lg.le - 1.0) * 1e4;              // REAL: mechanism is H1-close-driven -> honest fill = the close that tripped it (worse-of)
            emit_be_clip_(lg, stop, ts, bar, gross, gross_real, "BE_TRAIL_CLIP");
            lg.ref_px = stop; lg.open = false; lg.clipped = false;            // reclip from exit px
            lg.le = 0.0; lg.hwm = 0.0;
        }
    }

    void flush_be_(Leg& lg, double px, int64_t ts, int64_t bar) {            // parent exit: floored, never underwater
        if (!lg.open || lg.le <= 0.0 || px <= 0.0) return;
        const double gross = std::max(0.0, (px / lg.le - 1.0) * 1e4);        // MODEL (clamped, reference only)
        const double gross_real = (px / lg.le - 1.0) * 1e4;                  // REAL: unclamped MTM at the flush px
        emit_be_clip_(lg, px, ts, bar, gross, gross_real, "ENGINE_EXIT");
        lg.open = false;
    }

    void emit_be_clip_(Leg& lg, double exit_px, int64_t ts, int64_t bar,
                       double gross, double gross_real, const char* reason) {
        const double net = gross;   // MODEL column: "cost already paid by +be_bp" fallacy — kept as reference, NEVER fold
        const double net_real = gross_real - cfg_.round_trip_bp;   // REAL: worse-of fill − RT cost (the honest book)
        ClipRecord r;
        r.tag = cfg_.tag + "-" + lg.label; r.symbol = cfg_.symbol; r.reason = reason;
        r.entry_ts_ms = lg.open_ts; r.exit_ts_ms = ts;
        r.entry_px = lg.le; r.exit_px = exit_px;
        r.gross_bp = gross; r.net_bp = net; r.mfe_pct = lg.mfe;
        r.gross_bp_real = gross_real; r.net_bp_real = net_real;
        r.bars_held = (int)(bar - lg.open_bar);
        r.clip_num = ++clip_num_;
        r.shadow = shadow_mode;
        banked_bp_ += net;
        banked_bp_real_ += net_real;
        if (on_clip_) on_clip_(r);
        std::printf("[CLIP][%s] %s real=%+.1fbp (model=%+.1fbp) gross_real=%+.1fbp mfe=%.2f%% bars=%d px %.6f->%.6f shadow=%d BEFLOOR\n",
            r.tag.c_str(), reason, net_real, net, gross_real, lg.mfe, r.bars_held, lg.le, exit_px, shadow_mode ? 1 : 0);
        std::fflush(stdout);
    }

    void reset_session_() { legs_.clear(); entry_ref_ = 0.0; }

    void init_base_legs_(double epx, int64_t ts, int64_t bar) {
        legs_.push_back(make_leg_("T1", epx, cfg_.tight, ts, bar, false));
        legs_.push_back(make_leg_("T2", epx, cfg_.wide,  ts, bar, false));
    }

    Leg make_leg_(std::string label, double epx, const Tier& t, int64_t /*ts*/, int64_t /*bar*/, bool /*seed*/) {
        Leg l; l.label = std::move(label);
        l.epx = epx; l.le = epx; l.arm = t.arm; l.stall = t.stall; l.gb = t.gb;
        l.rc = cfg_.reclip_pct; l.cg = cfg_.cost_gate_bp; l.confirm = cfg_.confirm_bp;
        return l;   // open=false until first step (matches python: open set on first observation)
    }

    Leg seed_leg_(std::string label, double epx, const Tier& t,
                  int64_t ts, int64_t ebar, int64_t nbar, double peak_mfe) {
        Leg l = make_leg_(std::move(label), epx, t, ts, ebar, false);
        l.open = true; l.open_ts = ts; l.open_bar = ebar;
        l.mfe = peak_mfe; l.ext_bar = nbar;   // stall fresh from restart (peak ts not persisted)
        return l;
    }

    std::string next_ladder_label_(size_t idx_after) {
        // legs 0,1 = T1,T2 ; ladder legs = L1,L2,... (idx_after is the size at spawn)
        return "L" + std::to_string((int)idx_after - 1);
    }

    // faithful python Leg.step — returns true + gross_bp + reason on a booked clip.
    bool step_leg_(Leg& lg, int64_t bar, double cur, double& gross_out, const char*& reason_out) {
        const double fav = (cur - lg.epx) / lg.epx * 100.0;
        cur_bar_ = bar;
        if (lg.clipped) {
            if (lg.rc > 0.0 && lg.pk > 0.0 && fav > lg.pk * (1.0 + lg.rc)) {
                lg.clipped = false; lg.le = cur;      // RECLIP = re-enter at current price
            } else return false;
        }
        if (!lg.open) {
            if (lg.confirm > 0.0 && fav * 100.0 < lg.confirm) return false;   // OPTION-B: not yet confirmed -> stay flat, book nothing
            lg.open = true; lg.open_ts = bar * cfg_.tf_secs * 1000; lg.open_bar = bar;
            if (lg.confirm > 0.0) lg.le = cur;                                 // le set to the confirm price (matches python)
            lg.mfe = fav; lg.ext_bar = bar;
        }
        if (fav > lg.mfe + 1e-9) { lg.mfe = fav; lg.ext_bar = bar; }
        const bool armed = lg.mfe >= lg.arm;
        const int  stall = (int)(bar - lg.ext_bar);
        if (armed && lg.stall > 0 && stall >= lg.stall)                 return clip_leg_(lg, cur, gross_out, reason_out, "STALL_CLIP");
        if (armed && lg.gb > 0.0 && fav <= lg.mfe * (1.0 - lg.gb))      return clip_leg_(lg, cur, gross_out, reason_out, "REVERSAL_CLIP");
        return false;
    }

    // faithful python Leg._clip — HARD COST-COVER gate suppresses sub-cost clips.
    bool clip_leg_(Leg& lg, double cur, double& gross_out, const char*& reason_out, const char* reason) {
        const double gross = (cur / lg.le - 1.0) * 1e4;
        if (lg.cg > 0.0 && gross < lg.cg) return false;   // cost not covered -> keep holding
        lg.pk = lg.mfe; lg.clipped = true;
        gross_out = gross; reason_out = reason;
        return true;
    }

    void flush_leg_(Leg& lg, double px, int64_t ts, int64_t bar) {   // always MTM (no abandon)
        if (!lg.open || lg.clipped) return;
        const double gross = (lg.le > 0.0) ? (px / lg.le - 1.0) * 1e4 : 0.0;
        emit_clip_(lg, px, ts, bar, gross, gross - cfg_.round_trip_bp, "ENGINE_EXIT");
        lg.open = false;
    }

    void emit_clip_(Leg& lg, double exit_px, int64_t ts, int64_t bar,
                    double gross, double net, const char* reason) {
        ClipRecord r;
        r.tag = cfg_.tag + "-" + lg.label; r.symbol = cfg_.symbol; r.reason = reason;
        r.entry_ts_ms = lg.open_ts; r.exit_ts_ms = ts;
        r.entry_px = lg.le; r.exit_px = exit_px;
        r.gross_bp = gross; r.net_bp = net; r.mfe_pct = lg.mfe;
        r.gross_bp_real = gross; r.net_bp_real = net;   // ladder mode fills MTM at cur with cost debited -> model == real
        r.bars_held = (int)(bar - lg.open_bar);
        r.clip_num = ++clip_num_;
        r.shadow = shadow_mode;
        banked_bp_ += net;
        banked_bp_real_ += net;
        if (on_clip_) on_clip_(r);
        std::printf("[CLIP][%s] %s net=%+.1fbp gross=%+.1fbp mfe=%.2f%% bars=%d px %.6f->%.6f shadow=%d\n",
            r.tag.c_str(), reason, net, gross, lg.mfe, r.bars_held, lg.le, exit_px, shadow_mode ? 1 : 0);
        std::fflush(stdout);
    }

    Config        cfg_;
    ClipCallback  on_clip_;
    std::vector<Leg> legs_;
    double  entry_ref_ = 0.0;
    int     clip_num_  = 0;
    double  banked_bp_ = 0.0;        // MODEL column (reference only — see gross_bp_real note)
    double  banked_bp_real_ = 0.0;   // HONEST real-fill column — the number that may fold into PnL
    int64_t cur_bar_   = 0;   // last bar seen (for snapshot bars_since_high)
    double  last_be_px_ = 0.0; // last mark seen in be_floor mode (flush fallback)
    // internal detector state (be_floor self-detect)
    std::vector<double> h1c_;      // ring of last det_w+1 H1 closes
    int64_t det_bar_   = -1;       // current H1 bar being aggregated
    double  det_close_ = 0.0;      // running close of det_bar_
    bool    det_in_    = false;    // in a detected long event
    double  det_entry_ = 0.0;      // event entry ref (~ next open)
};

} // namespace chimera
