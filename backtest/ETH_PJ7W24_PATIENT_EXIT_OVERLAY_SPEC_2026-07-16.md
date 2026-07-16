# SPEC — ETH PJ7W24 wide/patient exit overlay (#2 profit-keeper)

**Date:** 2026-07-16 · **Author:** AI session (operator asked "spec #2")
**Status:** GATE RUN → **FAIL — DO NOT WIRE** (2026-07-16). ETH keeps live #1 ride-to-flip.
Full result: `ETH_PJ7W24_EXIT_OVERLAY_GATE_FINDINGS_2026-07-16.md` (harness `eth_pj7w24_exit_overlay_bt.cpp`).
The §5 gate below was RUN: overlay collapses the 2025 bull (+109→−11, fat-tail amputation via the
48-bar time-stop); only ck3 entry-filter is base-positive (+86) but fails the mandatory 2×-cost leg (−88).
**Rule class:** overlay that edits the ONE real ETH contract's exit → **PERMITTED** (not a separate-contract
clip; the mimic path is falsified — see `REAL_PARENT_MIMIC_FINDINGS_2026-07-16.md`). Long-only spot.
No 200DMA anywhere (`feedback-no-200dma-crypto`). Regime gate = breadth/ER, never 200DMA.

---

## 1. Objective
Keep more of an ETH up-move's profit on the **real position** than the currently-live exit does, WITHOUT
amputating the fat-tail runners (the failure mode that killed every tight clip/BE/mimic — 4 harnesses).
The lever is the WF-confirmed exit frontier from `UpMoveTrailLossMitigation.md`:
**wide trail 5.0×ATR + hard stop 3.0×ATR + PATIENT time-stop ~48 bars + selective cost-gate ck3.**
Basket result vs baseline: **bear leak −310% → −166% (halved), 2024-25 bull +227% → +460% (doubled)**,
WF-confirmed (Spearman 0.865 train→OOS; identical ranking on the independent 2026 bear).

## 2. Current live state — PJ7W24 exit (authoritative: `real_parent_mimic_bt.cpp` L120-132)
Live cell `chimera::UpJumpCompanionEngine` PJ7W24 (josgp1 `ChimeraCrypto`/`xsec-deploy`), ethusdt, cap=1:
```
det_w=24  det_thr=0.070            # entry: +7% jump in 24h window arms the cell
jump_floor=true  jf_giveback=1.0   # floor mode, giveback=1.0 (very wide — effectively ride-to-flip)
jf_prebe_stop_bp=400.0             # pre-BE disaster stop = 4%
                                   # exit: symmetric -7% window flip (ride-to-flip)
be_floor=false                     # this is the jump_floor form, not the be_floor mimic form
```
**What it already does well (= #1 profit-keeper, already live):** wide, rides to flip, does NOT clip the
tail. This is the correct base. #2 only ADDS three disaster/patience brakes it currently lacks.

## 3. Target exit levers (frontier, `UpMoveTrailLossMitigation.md` §min-loss + §WF-confirm)
| Lever | Value | Purpose |
|-------|-------|---------|
| Wide ATR trail | **5.0×ATR** | give-back band; wider than churn-trigger, protects tail |
| Post-arm hard stop | **3.0×ATR** | catch the rare disaster WITHOUT choking winners |
| Patient time-stop | **~48 bars (~2 days)** | exit a stalled position that's leaking — but PATIENT, not twitchy (ts24/tight = worse) |
| Entry cost-gate | **ck3** (ATR ≥ 3×round-trip) | fewer marginal trades = fewer bear losers |

## 4. Delta — what PJ7W24 lacks vs the frontier
1. **No post-arm hard stop.** Current has only a *pre-BE* 400bp stop; once BE is cleared it rides to flip
   with no disaster brake. Frontier adds a **3.0×ATR post-arm hard stop**. ← primary bear-leak fix.
2. **No time-stop.** Current rides to flip indefinitely; a stalled winner bleeds carry/opportunity.
   Frontier adds a **patient 48-bar time-stop**. ← the operator's "cut after a timeframe of negative",
   VINDICATED in the sweep (48 bars helps; 24/tight hurts).
3. **Trail width.** Current `jf_giveback=1.0` is already very wide (≈ride-to-flip); frontier trail=5.0×ATR
   is *ATR-scaled* rather than %-of-move. Likely near-equivalent, but the units differ → MUST be measured,
   not assumed equal (§5).

## 5. GATE — MUST pass before any wiring (the crux; do NOT skip)
The frontier is proven **but not on this target.** Two un-validated transfers:
- **Entry-type transfer:** frontier validated on **Donchian N-bar breakout** entry. PJ7W24 entry = **+7%/24h
  jump**. Different trigger → different trade population → exit levers must be re-confirmed on jump entries.
- **Basket→single-name transfer:** frontier is a **BASKET** edge — breadth only **8/15 coins OOS-positive**
  (`UpMoveTrailLossMitigation.md` L75); the doc explicitly says *"don't run single-name."* **ETH is NOT in
  the current live USE basket** (BTC/XRP/ATOM/NEAR, L130). ETH's own OOS number is unpublished.

**Therefore the frontier's +460/−166 does NOT transfer to ETH-PJ7W24 by assertion.** Required gate — run on
ETH's OWN tape, jump-entry (7%/24h), measured cost (28bp base / 56bp 2×, `CryptoCostLedger`):
1. **A/B on the real parent tape:** baseline PJ7W24 (giveback1.0 / prebe400 / ride-to-flip) vs
   +overlay (trail5/stop3/ts48/ck3). Harness = extend `real_parent_mimic_bt.cpp` (already replays the real
   PJ7W24 windows) with an exit-overlay branch — NOT a separate clip book.
2. **PASS bar (same standalone gate the mimic failed):** overlay net ≥ baseline net at **base AND 2× cost**;
   **both WF halves positive** (the mimic died on WF-H1 — the overlay must not); worst-window bounded;
   **2nd-bear (2026 May6→Jun5, −28.6% BTC) ranking holds.** Mechanism-verified (a kill/keep must replicate
   the real jump-entry, per `feedback-verify-kill-replicates-mechanism`).
3. **Every-loss-mitigation-destroys-edge tripwire:** confirm the overlay does NOT collapse the bull
   (the BE/tight-stop poison flips +1160%→−1015%). If bull net drops materially, the stop/ts is too tight →
   FAIL, do not wire. This is the single most important check.

If (2) fails → **ETH keeps the current live exit (#1)**; do not wire. This is a real possible outcome and is
acceptable — #1 already keeps profit.

## 6. Wiring (ONLY after §5 PASS) — josgp1 `ChimeraCrypto`/`xsec-deploy`
On the PJ7W24 cell in the live `UpJumpCompanionEngine` header:
```
# keep entry + pre-BE unchanged:  det_w=24 det_thr=0.070 jf_prebe_stop_bp=400
jf_giveback           = <from §5 winner>     # likely keep 1.0 or set ATR-trail equiv
post_arm_hard_stop_atr = 3.0                  # NEW field if absent on jump_floor path
time_stop_bars         = 48                   # NEW field (patient); bar = engine tf
cost_gate_atr_k        = 3.0                   # ck3 selective entry
```
- New fields (`post_arm_hard_stop_atr`, `time_stop_bars`) likely need adding to the jump_floor exit path →
  real eng work + unit test, not a config flip. atr_period=14 already present.
- **Call-site activation in the SAME commit** (Omega S63 rule analogue — no "field exists, check never runs").
- Ships via josgp1 `git pull --rebase origin xsec-deploy` + native rebuild (NOT the omega-new deploy path —
  crypto is a separate box/repo). Log a `[SEED]`/boot line confirming the overlay armed on PJ7W24.

## 7. Rules compliance
- ✅ **Overlay on ONE real contract's exit** — the permitted class; NOT a separate-contract clip (mimic dead).
- ✅ **Long-only spot**, no short. **No 200DMA.** Regime gate (if added) = breadth/ER only.
- ✅ **Backtested loss-protection verdict** required before merge (§5) — satisfies
  `feedback-engine-loss-protection-provision` (self-enforced; C++ audit gate doesn't cover crypto books).
- ✅ Judged on the real book's own merit, never "beats riding WIDE" — here the A/B *is* the fair test because
  it edits the same single contract's exit (the ONE case a dominance test is legitimate).

## 8. Risks / open
- **Most likely failure:** ts48/stop3 shaves the ETH fat tail on jump-entry (different pop than Donchian) →
  bull collapses → FAIL §5.3. Real possibility; that's why the gate exists.
- ETH single-name may simply not clear 2× cost even on the overlay (mimic didn't) → keep #1.
- Effort: ~1 harness extension (exit-overlay branch in real_parent_mimic_bt.cpp) + if PASS, ~2 new header
  fields + call-site + josgp1 deploy. Est. small, but the GATE run is the real cost.

## 9. Recommendation
Proceed to §5 gate first (harness only — zero live change, zero risk). Wire §6 ONLY on a clean PASS.
Do not ship the overlay on the strength of the basket/Donchian frontier alone — that's the exact
"assume it transfers" trap the standing discipline forbids.
