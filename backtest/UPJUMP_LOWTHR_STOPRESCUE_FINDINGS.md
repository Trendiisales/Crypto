# UpJump LOW-THRESHOLD Stop-Rescue Sweep — ETH / AAVE / GRT / DOGE (S-2026-07-14av)

## Question
The 2026-07-14 stopsweep cull left 4 stop-compatible survivors (ETH, AAVE, GRT, DOGE —
the only coins whose wired jump-floor cells keep the gate with a hard pre-BE stop).
Uniform 2% immediate entry was 0/19 robust (pre-BE bleed, `UPJUMP2PCT_SPOT_PARENT_FINDINGS.md`),
but the fine stop grid (s=0.25–5%) was only ever swept at the WIRED thresholds (3–7%),
never at thr 1–2.5%. **Do lower up-jump thresholds (1.0–3.0%) become viable on these 4
coins when rescued by a per-coin tuned fine pre-BE stop + BE-floor?** This sweep closes
that gap.

## Method
Harness: `upjump2pct_be_bt.cpp` new `lowthr` mode (this commit). Mechanics EXACTLY the
wired jump-floor cells: immediate entry next 1h open on close-over-close-W jump ≥ thr;
hard pre-BE stop s (intrabar resting order, gap-through at open); BE-floor arms once a
close covers entry×(1+RT); trail = peak-giveback g of MFE (g=1.0 = ride-to-reversal);
reversal exit at j ≤ −thr; fresh-jump re-arm. Costs 20bp RT base; 2×-cost = full 40bp re-sim.

Grid (1,080 cells/coin): thr {1.0, 1.5, 2.0, 2.5, 3.0}% × W {1,2,3,4,6,8,12,24}h ×
s {0.25, 0.5, 0.75, 1, 1.5, 2, 3, 4, 5}% × g {0.3, 0.5, 1.0}.
Only thr ≤ 2.5% answers the ask; thr=3.0 rows are boundary context.

Data: `backtest/data/<COIN>USDT_1h.csv`, Binance 1h 2021-01-01 → 2026-07-12,
~48,425 rows/coin. Integrity pre-flight PASS ×4: 0 monotonicity violations, 0 ×1000
glitches, 7 hour-gaps each (exchange outages), sane price ranges.

## Gate (ALL must hold per cell)
net>0 ∧ PF≥1.3 ∧ WF-H1>0 ∧ WF-H2>0 ∧ 2×cost(40bp) re-sim net>0 ∧ n≥30 ∧
plateau (≥75% of thr/W neighbors net-positive at 1×) ∧ 2023-26-only net>0
(2022/2021 regime-artifact filter) ∧ top-1 trade ≤45% of net.
y2022 bleed SHOWN, never gated (long-only spot can't short a bear —
feedback-crypto-omit-2022-longonly).

## VERDICT: ALL-FAIL — 0 passing cells on all 4 coins, at thr≤2.5% AND at the thr=3.0 boundary

| coin | cells | net>0 at 1× | +basic (n,PF,WF) | +2023-26 | +top1≤45% | +plateau+2× | best passing thr≤2.5 |
|------|-------|-------------|------------------|----------|-----------|-------------|----------------------|
| ETH  | 1,080 | 73  | 1 | 0 | 0 | 0 | **NONE** |
| AAVE | 1,080 | 31  | 0 | 0 | 0 | 0 | **NONE** |
| GRT  | 1,080 | 2   | 0 | 0 | 0 | 0 | **NONE** |
| DOGE | 1,080 | 220 | 7 | 0 | 0 | 0 | **NONE** |

Best RAW-net cell per coin (all FAIL the gate — shown so nobody re-runs this):

| coin | cell | n | net bp | PF | WF H1/H2 | 2023-26 | top1 | kill reason |
|------|------|---|--------|----|----------|---------|------|-------------|
| ETH  | 2.5%/W1/s1.5/g1.0 | 340 | +7,923 | 1.33 | +7,706/+216 | **−634** | 65% | 2023-26 neg + top1 |
| AAVE | 3.0%/W1/s5/g0.3 (boundary) | 550 | +12,678 | **1.23** | +3,220/+9,458 | +6,051 | 15% | PF<1.3 |
| GRT  | 3.0%/W1/s5/g1.0 (boundary) | 560 | +6,108 | **1.09** | +4,336/+1,772 | +4,822 | 135% | PF + top1>100% |
| DOGE | 2.0%/W12/s5/g1.0 | 1,356 | +88,406 | 1.78 | +90,091/**−1,685** | **−6,132** | 69% | WF-H2 neg, 2021-only |

## Why the stop cannot rescue low thresholds (mechanism, not strawman)
Verified the kill replicates the real mechanism (feedback-verify-kill-replicates-mechanism):
- Harness cross-check: `stopsweep DOGE 12 3.0 1.0` reproduces the wired cell exactly
  (s=4% → +73,934bp, PASS on the wired-era gate) — the code is intact; the lowthr
  fails come from the stricter study gate + the lower thresholds, not a bug.
- At low thr the jump signal fires on noise; a fine stop CHURNS: ETH 2%/W1 s=0.25–1%
  stops out 50–85% of entries pre-BE (preBE bleed −25k…−36k bp), flipping the s=0
  ridge (+13,242) to −10,829…+3,416 with 2×cost ALWAYS negative. Same physics as the
  thr=2% parent verdict — the dip a stop kills is the dip that precedes the rider.
- The few raw-positive low-thr cells are 2021-bull artifacts: DOGE's +88k cell is
  +90k in H1 (2021-22) and NEGATIVE in H2 and in 2023-26. No cell on any coin has
  positive 2023-26 net once the basic gate is passed.

## Side-finding (SHOWN, no action mandated): live DOGE-PJ3W12 s=4 is 2021-concentrated
The live DOGE cell's config (thr=3/W12/s4/g1.0, in this grid at the boundary) passes
the wired-era gate but per-year splits: **2021 +91,271bp, 2022 −15,358, 2023-26 −1,979bp**
(2023 −5,676 / 2024 +3,203 / 2025 +598 / 2026 −104). Its whole edge is the 2021 bull.
The other 3 live cells are healthy on 2023-26: ETH +12,805bp, AAVE +3,723bp,
GRT +20,040bp. The wired gate did not include a 2023-26 recheck, so this is not a
gate breach — but the operator should see the number when next weighing DOGE-PJ3W12.

## Verdict for the operator
**No cell at thr ≤ 2.5% (nor at 3.0 outside the already-wired DOGE) passes the full
gate on any of the 4 coins. Nothing here justifies extending the immediate-entry
override below the wired thresholds.** The 2% question is now closed from both ends:
uniform (0/19), and stop-rescued per-coin on the most stop-friendly coins (0/4).
Viability continues to start at thr≈3-4% — exactly the wired roster.

Run: `./upjump2pct_be_bt lowthr` (build: `g++ -O2 -std=c++17 upjump2pct_be_bt.cpp -o upjump2pct_be_bt`,
runtime ~1.3s). Loss-protection provision: inherent — the stop IS the swept lever; no
engine built, no live provision owed (feedback-engine-loss-protection-provision).
