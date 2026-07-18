# BOUNDED CATCH-UP CERTIFICATION — `catchup_max_age_bars` — 2026-07-18

**Operator order:** "resume and do the bounded thing" — the bounded catch-up proposed after the
INJ lost-window incident (2026-07-18: INJ +1.76% 06:00Z H1 jump predated the 22:57Z zeroing
restart; warm-seed refilled the ring but left the detector flat → no window, no trades).

**VERDICT: CERTIFIED — wired at `catchup_max_age_bars=24` on all confirmed-entry mimic cells
(BECASC base/FAST/SLOW + UJ2W8 rescue + SWEET/REGIME `make_mimic_floor_cell`).**

## Mechanism (ChimeraCrypto `UpJumpLadderCompanion::seed_det_ring_hist`)
At warm-seed, replay the finalized closes with the live detector rule; RE-OPEN the window
(det_in_/det_entry_ only) iff ALL of:
- replayed detector still in-window (no `j <= -thr` since the jump);
- jump ≤ `catchup_max_age_bars` finalized bars old (the BOUND);
- NO close since the jump (pending included) reached `epx*(1+min tier confirm)` — an always-on
  book would still be FLAT waiting for confirm. Confirm crossed in downtime ⇒ SKIP (back-filling
  that open = the forbidden late-chase/backdated-entry class). **The INJ trigger case itself was
  +4.9% past confirm at restart ⇒ correctly SKIPPED even with catch-up — the recovery applies to
  the pre-confirm class only.**
- confirmed-entry mimic family only (`mimic_floor` + min confirm > 0 + `arming_allowed_()`;
  NEVER `jump_floor` — immediate entry at NOW price = chase — never retired `be_floor`;
  `confirm_bp=0` configs refused; in-flight/persisted-current state untouched).

Legs then init + open through the UNTOUCHED live confirm path (BE-ENTRY, `confirm_anchor_epx`
le=epx, floored-on-open) — a recovered window books the SAME confirm-cross entry an always-on
book would have booked. Foundation recipe unchanged.

## Harness
`catchup_outage_bt.cpp` — drives the REAL `UpJumpLadderCompanion` (incl. the real catch-up seed
code), honest 17f fills, live-config parity makers (BECASC/rescue/SWEET byte-parity incl.
anchored_reclip + stagger). Data: Binance 1h 2021/23→2026-07-17 corpus (integrity-gated files).
Coins: the FULL 35-coin mimic fleet (never-half-symbols). Cost 28bp measured class AND 2×=56bp.

## Results
**SURGICAL (single 2-bar outage over the jump close; faithful model of a live restart):**
- 28bp: 3,602 windows · 18,635 recovered clips · **0 mismatch** vs always-on (entry_px + entry/exit
  ts + net multiset-exact) → PASS
- 56bp: 3,602 windows · 18,596 recovered clips · **0 mismatch** → PASS
- N=0 control lost 653 of those windows outright (the INJ class); N>0 recovered them exactly.
- Early false "mismatches" (212) were a HARNESS artifact: map key collapsed multi-leg clips
  sharing (ets,xts); multiset matcher fixed it — engine clip lists were identical line-by-line.

**GRID (periodic outages stride 168, L∈{2,6,12,24,48} × N∈{24,48}, 1,450 gated rows/cost):**
- 28bp: catch-up vs no-catch-up avg **+11.0%/cell-run** (median +4.1%); absolute gate
  (net>0 & PF≥1.3) fails **0**; new-tail rows (C worst < min(A,B)−1bp) **2**.
- 56bp: avg **+8.2%** (median +2.5%); absolute fails **0**; new-tail rows **2** (same cell).
- Per-cell C-vs-B net deltas scatter (≈30% negative rows): no-catch-up B often re-enters a
  LATER shifted window (detector re-triggers) — window-selection variance, not a catch-up
  defect; C is the run faithful to certified always-on behavior.
- **Explained exception (the 2 rows):** BNB SWET L=24 books worst −1153bp vs A/B −873bp under
  repeated-outage stress (compounded divergence lets C arm an in-window jump A never entered —
  a different, still-floored entry). −1153bp is 2.6× inside BNB SWEET's certified worst-episode
  envelope (−3000bp; retire −6000×legs), and the cell's net is +29% BETTER with catch-up.
  Single-restart (surgical) equivalence is exact.

**Unit test** `ChimeraCrypto/tests/catchup_seed_test.cpp` — 11/11 PASS: arms only within bounds;
stays flat on: default-off, confirm-crossed (incl. pending bar), age>bound, historical reversal,
jump_floor, confirm_bp=0, rank_out/retired; catch-up leg opens ONLY at the live confirm cross.

## Bound choice
N=24 vs 48 near-identical benefit (+11.1% vs +10.9% avg at 28bp) → **24 bars (1 day)**, the
tighter bound. Seed fetch depth extended `det_w + catchup + 6` klines (main.cpp det-seed pass).

## Omega port
Same class applies to Omega companions (FxUpJumpLadderCompanion / GoldTrendMimicLadder — windows
also lost across deploy restarts). OWED: port + own cert, next session. Never wire uncertified.
