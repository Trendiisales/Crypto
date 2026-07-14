# UpJump 2% Spot Parent Engine — Backtest Findings (S-2026-07-14)

## Operator ask (verbatim intent)
"Independent engine, trade when it hits a 2% long, spot, we trade until it reverses
and then exit, that's it, make sure we cover BE. Use a bracket if you have to."

## What was tested (faithful, WITH floor — feedback-test-operator-spec-before-verdict)
Harness: `upjump2pct_be_bt.cpp` (this dir). Independent long-spot engine, judged STANDALONE.
- DETECT: close-over-close-W jump ≥ thr (thr=2% mandate lever, 52c0d31 lineage). Entry next 1h open.
- BRACKET: optional pre-BE hard stop s ∈ {none, 1%, 2%} (intrabar, gap-through honest).
- **BE-FLOOR: once close covers entry+20bp RT, stop rises to BE-incl-cost → trade cannot
  close negative after arming.** (Floor included — no floorless strawman.)
- EXIT: peak-giveback trail g ∈ {0.3, 0.5} OR literal g=1.0 (= reversal-only: exit when
  j ≤ −thr, "trade until it reverses, that's it").
- Re-entry only after a fresh jump (j must drop below thr once).
- Data: 19/19 coins integrity-gate PASS (AAVE ADA AVAX BCH BNB BTC DOGE ETH GRT LDO LINK
  LTC NEAR OP SOL TRX UNI XLM XRP, Binance 1h, 2021-01→2026-07; GRX file excluded = DAX
  mislabel ~24,000 px, not a coin). Costs 20bp RT; 2×-cost = full 40bp re-sim.
- Gate (long-only corrected): net>0 ∧ PF≥1.3 ∧ WF-H1>0 ∧ WF-H2>0 ∧ 2×cost>0. y2022 shown, not gated.

## VERDICT: NOT VIABLE — 0/19 robust at thr=2%, every window W ∈ {1..24}h, every exit style
- thr=2% portfolio: **net −219k…−711k bp across the whole config space** (48 configs), 0/19
  PASS with trail, 1/19 literal. Median PF 0.60–0.86.
- Immediate entry pays 20bp on every 2% blip → 300–1,600 trades/coin → gross edge ≈ +5bp/trade
  < cost. The cost wall, same physics as the dead 0.5% UJH family, just slower bleed.
- **Pre-BE bracket stop makes it WORSE everywhere** (s=1%: 50–68% of entries die on the stop;
  portfolio drops another −150k…−250k bp). Same mechanism as the disproven confirmed-entry:
  the stop churns entries that would have recovered.
- **BE-floor cannot rescue it**: floor only protects AFTER +20bp is covered. All the bleed is
  pre-BE — reversal-only worst single trades −10%…−32% (never covered BE, rode the fade down).
  Structure can still trade into a loss (the 07-13 objection, now with its own number).
- Sole PASS: **ETH W=1h/2% literal** (+13,242bp, PF1.33, WF both+, 2×cost +4,688, n=508).
  **ISOLATED RIDGE CELL** — W=2/3/4 fail, thr=1.5/2.5/3 fail. Same isolated-ridge pattern that
  FAILED the GOLD campaign BT-first gate 2026-07-13 (z=1.39 bull drift) → NOT built on one cell.
  (Corroboration note: 07-11 companion study also passed ETH+2%/1h — but in the LADDER mechanism.)

## What DOES hold at higher thresholds (context, not the ask)
thr=4%/W=1h reversal-only: portfolio +76k bp, 5/19 PASS at 1×cost — **dies at 2×cost** (+8k→−0.4k
per exit style). Not shippable under the standing 2×-cost gate. The validated jump family remains
the 07-11 robust-7 LADDER coins (SOL/DOGE/BNB/TRX/ETH/ADA/XRP at +3–6% detect) and the live
companion arm-2 BE-mimic (neg=0 by construction).

## Loss-protection verdict (feedback-engine-loss-protection-provision)
Provision was backtested (bracket stops s=1%/2% + BE-floor). Verdict: floor=yes/harmless,
pre-BE hard stop=harmful, but the engine is net-negative in every protected variant → engine
not built, so no live provision owed.

## Bracket anatomy (how it WOULD wire on Binance spot, if ever shipped)
1. Detect 2% jump on 1h close → MARKET BUY (taker 10bp).
2. Immediately place STOP_LOSS_LIMIT at pre-BE stop (or none = engine-managed reversal exit).
3. On first 1h close ≥ entry×1.002: cancel/replace stop to entry×1.002 (BE incl. RT) — the floor.
4. Each 1h close: cancel/replace stop to max(floor, entry×(1+MFE×(1−g))) — trail. (Binance spot
   also supports native trailingDelta on stop orders; cancel/replace on bar close is simpler and
   matches the backtest exactly.)
5. Reversal (j ≤ −2%) at close → cancel stop, MARKET SELL.
Engine = one small C++ loop on josgp1 (no python — feedback-no-python-working-system).

## PER-COIN LEVER MAP (operator follow-up, 2026-07-14: "pull every lever, where viable per coin")

`percoin` mode added: 936 combos/coin = thr {2,2.5,3,3.5,4,4.5,5,5.5,6,7,8,10,12}% ×
W {1,2,3,4,6,8,12,24}h × pre-BE stop {0,1,2}% × giveback g {0.3,0.5,1.0=reversal-only}.
Gate = corrected long-only (net>0, PF≥1.3, WF-H1>0, WF-H2>0, 2×cost>0, n≥30) + PLATEAU
(≥75% of thr/W neighbors net-positive at 1× — kills isolated ridges; ETH W1/2% did NOT
return; ETH's plateau cell is W24/7%). Best cell per coin ranked by 2×-cost net.

### Result: 17/19 VIABLE at higher thresholds; LDO + LTC NO-CELL

| coin | W | thr% | s% | g | n | net bp | PF | 2× bp | worst | y2022 | top1/net |
|------|---|------|----|----|-----|---------|-------|---------|--------|---------|----------|
| AAVE | 1 | 4.0 | 0 | 1.0 | 227 | +20206 | 1.66 | +24127 | -2772 | -558 | 41% |
| ADA | 8 | 8.0 | 0 | 1.0 | 152 | +22779 | 2.90 | +19073 | -2854 | -993 | 36% |
| AVAX | 2 | 8.0 | 0 | 1.0 | 80 | +32515 | 3.58 | +42445 | -3231 | -1927 | 45% |
| BCH | 1 | 5.5 | 0 | 0.5 | 58 | +15341 | 4.73 | +14576 | -1306 | +587 | 65% |
| BNB | 12 | 7.0 | 0 | 1.0 | 120 | +19703 | 6.14 | +24323 | -1652 | -1652 | 87% |
| BTC | 1 | 4.0 | 0 | 1.0 | 30 | +7974 | 16.21 | +7914 | -524 | -524 | 90% |
| DOGE | 12 | 3.0 | 0 | 1.0 | 937 | +85030 | 1.93 | +64614 | -1814 | -13626 | 72% |
| ETH | 24 | 7.0 | 0 | 1.0 | 223 | +18498 | 2.01 | +19058 | -1571 | -506 | 51% |
| GRT | 1 | 5.0 | 0 | 1.0 | 133 | +23559 | 2.56 | +17109 | -4440 | +1061 | 59% |
| LDO | - | - | - | - | - | NO-CELL | | | | | |
| LINK | 1 | 6.0 | 2 | 1.0 | 32 | +19257 | 6.84 | +13212 | -220 | -1540 | 89% |
| LTC | - | - | - | - | - | NO-CELL | | | | | |
| NEAR | 1 | 5.5 | 0 | 1.0 | 101 | +16204 | 2.12 | +15888 | -3028 | -607 | 61% |
| OP | 1 | 4.0 | 0 | 0.3 | 196 | +16507 | 1.76 | +11010 | -2130 | +5918 | 18% |
| SOL | 24 | 8.0 | 0 | 1.0 | 365 | +18938 | 1.75 | +19427 | -1711 | -4904 | 41% |
| TRX | 2 | 4.5 | 0 | 1.0 | 100 | +18252 | 4.29 | +15046 | -1061 | -1457 | 27% |
| UNI | 3 | 8.0 | 0 | 0.3 | 95 | +8696 | 1.74 | +7729 | -3206 | -327 | 40% |
| XLM | 1 | 5.0 | 0 | 1.0 | 85 | +16789 | 2.51 | +22241 | -2691 | -13 | 65% |
| XRP | 8 | 12.0 | 0 | 0.3 | 65 | +15555 | 4.59 | +15295 | -1531 | +884 | 20% |

### Reading the map
- **The killer lever was the uniform 2% threshold.** Viability starts ≈3-4% and the winning
  shape is almost uniformly: immediate entry + BE-floor + ride-to-reversal, NO pre-BE bracket,
  NO trail (s=0, g=1.0 in 13/17; LINK alone prefers s=2%). Pre-BE brackets remain harmful —
  consistent with the thr=2% verdict.
- **Payoff concentration (trade-list eyeball, `trades` mode added):** g=1.0 cells scratch
  70-90% of trades at exactly BE (+0.0bp — floor working as designed) and earn from rare big
  riders. top1/net = single best trade's share of net.
  ROBUST tier (top1 ≤45%, n ≥65): OP 18%, XRP 20%, TRX 27%, ADA 36%, UNI 40%, SOL 41%,
  AAVE 41%, AVAX 45%. FRAGILE lottery tier (top1 ≥65% or n<35): BTC (90% = one trade,
  2024-08-05), LINK 89%, BNB 87%, DOGE 72%, BCH 65%, XLM 65%.
- **Earlier "thr=4% dies at 2×cost" line was PORTFOLIO-level at one uniform config**; per-coin
  tuned cells each pass their own 2×-cost re-sim. Not a contradiction.
- **Cross-check vs CryptoUpJumpBullCells (mimic/confirm ladder mechanism):** LTC dead in both.
  TRX strong in both. Coins certified NO-CELL under confirm (BTC/ETH/SOL/XRP/XLM/GRT/OP/BCH/AVAX)
  return VIABLE here — immediate entry catches the move the confirm bar missed; BE-floor turns
  would-be losers into scratches.
- **Loss-protection verdict:** inherent in the sweep (floor + bracket levers). Per-cell verdict:
  BE-floor yes; pre-BE hard stop harmful (except LINK s=2%).

### Status: STUDY ONLY — NOT WIRED
Immediate-entry class remains operator-forbidden-by-default
(feedback-no-immediate-entry-upjump-mimic-only). Any wiring needs explicit operator confirm
against this table, robust tier first.
