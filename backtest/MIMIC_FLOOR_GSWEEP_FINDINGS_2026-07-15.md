# MIMIC-FLOOR g-sweep findings — 2026-07-15 (session 15h resume)

Unify every confirmed-entry mimic to ONE exit = honest jump_floor floor + HWM trail-by-g,
on the ladder path (`mimic_floor` mode, UpJumpLadderCompanion). Faithful spec tested:
**confirm entry (20bp) + BE floor post-arm (le·(1+RT)) + HWM trail by g + reclip re-entry**,
NO pre-arm hard cut (loss_cut=0 — the cut is NOT in the operator's 5-point spec and its
epx-anchor blew up gap-confirms). Harness `upjump_earlyarm_bt mimicg` drives the REAL engine
(parity by construction). Gate (long-only, omit 2022): net>0 & PF≥1.3 & WF-H1>0 & WF-H2>0 &
2×-cost(net>0 & PF≥1.3). Data *_1h 2023-01→2026-07.

## Key mechanism findings
1. **Pre-arm loss_cut DESTROYS these cells** (churn like an entry-stop; INJ 127/183 neg).
   Removed (cut=0). Pre-arm loss now bounded only by the detector reversal (window end).
   [Engine still HAS an le-anchored pre-arm cut available if loss_cut>0; mimics ship cut=0.]
2. **Tightening the giveback HURTS** (classic trend-follow; vault-confirmed). Best g is HIGH
   (g=1.0 / 0.75) for most survivors; small g exits winners early. Operator's "smallest g"
   assumption is backtest-false for these trend cells.
3. **reclip (re-entry) is REQUIRED** — without it (my first run) every cell looked dead
   (strawman). With reclip the survivors pass. feedback-test-operator-spec-before-verdict.

## SWEET cells (11) — 5 SURVIVE, 6 DROP

| cell | W/thr | smallest-viable g | @that g: net% / PF / 2×[net,PF] | robust high-g | verdict |
|---|---|---|---|---|---|
| BNB  | 1/4.0% | 0.50 | +36 / 2.80 / [+19,1.60] | 0.75 | **KEEP** |
| UNI  | 1/3.5% | 1.00 | +104 / 1.78 / [+92,1.62] | 1.00 only | **KEEP** (g=1.0 only) |
| NEAR | 1/4.0% | 0.75 | +97 / 1.64 / [+68,1.42] | 1.00 | **KEEP** |
| TRX  | 8/3.5% | 0.40 | +53 / 1.76 / [+40,1.59] | 0.75 (PF6.8) | **KEEP** (best cell) |
| DOGE | 1/5.5% | 0.10 | +77 / 1.70 / [+62,1.51] | plateau 0.10-0.25 | **KEEP** (non-monotone) |
| UNI  | 2/4.0% | — | best g1.0 net+89 but H1=-3 (WF-H1<0); 2× marginal | — | **DROP** |
| AAVE | 3/4.5% | — | base ~PF1.4 but 2×-cost PF<1.3 every g | — | **DROP** |
| ADA  | 8/3.5% | — | PF≈1.0-1.1, 2×-cost net<0 every g | — | **DROP** |
| LINK | 8/4.5% | — | mostly net<0, 2×<0 every g | — | **DROP** |
| LDO  | 8/7.0% | — | mostly net<0 (weakest wire already) | — | **DROP** |
| INJ  | 24/5.5% | — | net<0 EVERY g (the −242 cell) | — | **DROP** |

## REGIME-BEMIMIC cells (5: NEAR/THETA/SUSHI/ADA/DOT) — NOT YET SWEPT
det_w=0, driven by external D1 REGIME_SWITCH parent (not self-detect up-jump). Needs a
parent-driven replay harness (none exists in Crypto/backtest). Pending operator call on
whether to build it or keep the regime mimics on the current ladder exit.

## Engine changes (built, parity-clean, NOT deployed)
- `include/core/UpJumpLadderCompanion.hpp`: `mimic_floor` + `mimic_giveback` config, per-leg
  `floored`/`stop_px`, single-leg init, per-tick BE-floored trail (`intrabar_mimic_floor_`,
  `mimic_ratchet_`, `book_mimic_stop_`), le-anchored pre-arm cut, floored legs excluded from
  the epx-anchored reversal cut, reclip resets the floor.
- Harness `Crypto/backtest/upjump_earlyarm_bt.cpp`: new `mimicg` mode (g-sweep, real engine).
