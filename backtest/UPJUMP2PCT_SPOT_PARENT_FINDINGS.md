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
