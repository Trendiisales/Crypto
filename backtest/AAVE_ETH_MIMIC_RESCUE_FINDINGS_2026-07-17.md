# AAVE / ETH floored-mimic RESCUE — self-trigger sweep — 2026-07-17

**Operator order:** AAVE-PJ4W1 / ETH-PJ7W24 floored BE-mimic conversions were certified NO-GO
(`AAVE_GRT_PJ_MIMIC_FINDINGS_2026-07-17.md`) — "find a solution that's viable" for AAVE and ETH.
**VERDICT: VIABLE — both coins.** The NO-GO was specific to the PARENT-DRIVEN PJ-trigger form
(AAVE 1h/+4%, ETH 24h/+7% parents). The unswept lever space — SELF-TRIGGERING mimic_floor cells
(SWEET pattern, no parent) on the 17c BE-floor-on-open FOUNDATION recipe — passes the full
standalone gate across a DEEP plateau on both coins. Findings only; nothing wired (operator decides).

## What was already culled (not re-tested — new basis stated)
- **PJ parent-driven mimics**: AAVE-PJ4W1 / ETH-PJ7W24 / GRT — NO-GO at every g×lc×cost
  (07-16/07-17 docs). AAVE/ETH PJ cells themselves REMOVED S-17s (imm-entry ban).
- **SWEET sweep 07-15** (confirm=20bp, NO anchor, reclip=5%): AAVE 3/4.5% DROPPED (2×-cost PF<1.3
  every g). ETH had no SWEET self-trigger cell in that 11.
- **Immediate-entry lowthr stop-rescue** (thr 1–3%, s-grid, confirm=0): ALL-FAIL 0/4 coins —
  but that family is now FORBIDDEN anyway (no immediate entry).
- **NEW BASIS here** (never swept on AAVE/ETH): self-trigger det(W,thr) × mimic_floor ×
  **foundation recipe confirm=60bp (≥2× measured RT) + confirm_anchor_epx (le=epx, floored ON
  OPEN) + reclip=0**, g-sweep, single-leg and 4-leg, at thr 2–7% × W 1–48h. The 07-15 SWEET drop
  used confirm20/no-anchor/reclip5% — a different (pre-foundation) mechanism.

## Harness + regression + data
- `aave_eth_mimic_rescue_bt.cpp` (this commit) — derived from `eth_ujmimic_15_becascade_bt.cpp`;
  drives the **REAL live `UpJumpLadderCompanion`** (ChimeraCrypto HEAD 38e89b8, includes the 17f
  honesty fix — `book_mimic_stop_` books the ACTUAL fill, real tails, nNeg>0 everywhere).
  Env: `UM_COIN UM_START UM_THR UM_W UM_G UM_LEGS UM_CONFIRM UM_ANCHOR UM_RT`.
- **Regression (mandated):** reproduced the certified BECASC ETH W4/g1.0/4-leg row EXACTLY on
  n (3397) and worst (−1319.7bp); net is lower than the 07-16 doc (+1076 vs +2281) because that
  cert predates the 17f honest-fill fix — expected and desired. Harness trusted.
- **Data:** certified `data/AAVEUSDT_1h.csv` / `data/ETHUSDT_1h.csv` (48,426 rows, 2021→2026-07-12);
  `integrity_gate.py` PASS 2/2 clean this session.
- **Cost:** 28bp measured class (`CryptoCostLedger::safe_cost_bps` floor — no bookdepth for AAVE;
  ETH depth-measured 28bp), stress **2× = 56bp**. Never hand-quoted.

## Gate (standalone — never vs parent/WIDE)
n≥30 & net>0 & PF≥1.3 & WF-H1>0 & WF-H2>0, at 28bp AND 2×=56bp. Window ≥2023 (2022 omitted,
long-only). Plateau required (neighbors pass), top-1 clip concentration checked.

## Grid result — the pass region is BROAD, not a knife-edge
Full grid per coin: W {1,4,8,12,24,48} × thr {2,3,4,5,7}% × g {0.2,0.3,0.4,0.5,0.6,0.75,0.85,1.0}
× legs {1,4}, confirm=60/anchor=1/reclip=0/lc=0:

| coin | cells | PASS | fail structure (mechanism-coherent) |
|---|---|---|---|
| AAVE | 480 | **419** | fails cluster at thr 5–7% low-W (noise triggers, incl. the PJ4-like W1/4–7%) + W48 high-g |
| ETH  | 480 | **364** | fails cluster at thr 4–7% W≤12 (the PJ7-like region) — **thr=2% passes at EVERY W and EVERY g, both legs** |

## WIRE-CANDIDATE cells (tightest-certified g per profit-lock; leg=1 = new standalone cell)

**AAVE — W=8 (8h window) / thr=+2.0% / g=0.20 / confirm=60bp anchored / reclip=0 / 1 leg:**
n=478, **net +415% PF 5.52**, worst −366.5bp (honest gap tail, nNeg=155), WF **+193/+223**,
top1 4%, years 23/24/25/26 = +61/+148/+149/+57 (all +).
**2× cost explicit: net +283% PF 2.95**, worst −394.5, WF +127/+156. **PASS both costs.**

**ETH — W=8 / thr=+2.0% / g=0.20 / confirm=60bp anchored / reclip=0 / 1 leg:**
n=265, **net +196% PF 8.77**, worst −197.7bp (nNeg=51), WF **+92/+104**, top1 10%,
years = +43/+46/+76/+31 (all +).
**2× cost explicit: net +122% PF 3.59**, worst −225.7, WF +55/+67. **PASS both costs.**

### Plateau evidence (all PASS, no knife-edge)
- AAVE: every cell in W {4,8,12,24} × thr {2,3}% × g {0.2–0.6} passes (40/40); g up to 1.0 also
  passes at thr 2% W4–12. n 257–523 per cell.
- ETH: every cell at thr 2% passes for ALL W {1,4,8,12,24,48} × ALL g × both legs; W {8,12,24} ×
  thr {2,3} × g {0.2–0.6} = 30/30 PASS except the known W12/thr3 g≥0.5 pocket. n 141–267.
- g=0.20 is simultaneously the TIGHTEST and the (near-)best cell — unlike the trend cells, the
  floor-on-open design prefers a tight lock; profit-lock (g<1.0 tightest-certified) satisfied
  with margin. Neighbors g=0.3/0.4 pass on both coins.

### Robustness legs
- **confirm=100bp** (covers ≥1.8× even the 56bp stress cost): ALL 80 candidate-neighborhood cells
  still pass, PF improves (AAVE W8/2%/g0.2 → +549% PF 11.6; ETH → +291% PF 26.3) — the confirm
  lever is not fragile at 60.
- **Full-window 2021→ context** (shown, not gated): candidates stay positive in BOTH bear years
  (AAVE 2022 +162..+206%; ETH 2022 +78..+95%) — the floor-on-open survives the bear that killed
  the immediate-entry family.
- **4-leg BE-cascade variant**: also passes across the same region (it is essentially the wired
  BECASC family at a different thr); the deliverable cell is the SINGLE-leg standalone.
- **PJ-analog self-trigger rows** (AAVE W1/4%, ETH W24/7%): skipped as candidates per order.
  For the record the self-trigger floored form mostly passes even there, but n=35–46 (< the
  BECASC-grade sample), top1 up to 61% concentration, and AAVE W1/4%/g1.0 fails — exactly the
  fragility the NO-GO flagged. The PJ NO-GO stands for the parent-driven form; do not revisit.

## Existing certified coverage (unchanged by this study)
Both coins already carry **UJ15-BECASC** (make_becascade_cell: det W=4/thr 1.5%, g0.5,
confirm60/anchor, cap8 BE-cascade, all-22 PASS, SHADOW). Re-verified HERE at HONEST fills on the
≥2023 window: ETH +3058% PF 10.41 (n=2306, worst −573.6) / AAVE +6808% PF 10.41 (n=3953, worst
−577.0) — both PASS incl. 2×. (The original all-22 cert predates 17f; this is the honest-fill
confirmation.) The rescue cells are ADDITIVE coverage at a different trigger (2%/8h vs 1.5%/4h),
lower turnover (~0.2/day vs ~1.8/day), single-leg.

## Honest caveats (BACKTEST_TRUTH)
1. Worst clips are REAL gap-through tails (AAVE −366..−394bp, ETH −198..−226bp) — the floor is a
   design property, not an execution guarantee (17f framing). nNeg 20–50% of clips (small −32bp-
   class stop-outs + slip).
2. 1h close-driven detector + intrabar low stop check — same basis as the certified BECASC family.
3. At the 56bp stress cost, confirm=60 is only ~1.07×RT; cells still pass, and the confirm=100 leg
   passes everywhere with margin if the operator wants 2×-cost-covering confirm under stress.
4. Sizing/notional is the operator's call (project-revisit-lot-sizes).

## Runs
```
clang++ -std=c++17 -O2 -I/Users/jo/ChimeraCrypto/include aave_eth_mimic_rescue_bt.cpp -o aave_eth_mimic_rescue_bt
UM_COIN=AAVE UM_CONFIRM=60 UM_ANCHOR=1 ./aave_eth_mimic_rescue_bt          # full grid, >=2023
UM_COIN=ETH  UM_CONFIRM=60 UM_ANCHOR=1 UM_THR=0.02 UM_W=8 UM_G=0.2 UM_LEGS=1 ./aave_eth_mimic_rescue_bt
```
