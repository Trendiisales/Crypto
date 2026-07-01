# IBKRCrypto — Full Report (real-cost re-test, 2026-06-24)

Faithful CRTP+array BT, daily 2017-2026 (4 bear regimes: 2018/2020-covid/2022/OOS),
next-open fills, **long+short (perp)**, vol-target 2%/day, walk-forward IS/OOS.
**Cost = REAL CME-via-IBKR**, fee-dominated by nano-contract notional (NOT Binance 6bps):
BTC-SQF 22bps · ETH-SQF 40bps · SOL-SQF 18bps · XRP-SQF 25bps · index-SQF 3bps ·
BTC-micro(MBT) 6bps · BTC-std 1bps. (Maintenance/TFA fee waived to Jun-2026; exact CME
SQF exchange fee still to confirm via Fee Finder/gateway — verdicts hold across the band.)

## Deployable roster @ real cost (OOS = walk-forward out-of-sample)
| instrument | route | strat (L/S, vt) | OOS PF | OOS DD | 2022-bear | grade |
|---|---|---|---|---|---|---|
| **ETH** | ETH SQF (perp) | TSMom50 | **2.39** | 25% | 1.10 | ★ LEAD — trend, survives 40bps |
| **BTC** | BTC SQF (perp) | TSMom50 | **1.55** | 49% | 1.53 | ★ trend, vol-capture (size down for DD) |
| **SPX** | S&P500 SQF | TSMom50 | **2.23** | 12% | — | ★ best risk-adj (index) |
| **NDX** | Nasdaq100 SQF | TSMom50 | 1.65 | 27% | — | ✅ index trend |
| **SOL** | SOL SQF (perp) | IBS mean-rev | 1.04 | 21% | 1.33 | ⚠️ only MR surviving on SQF (big notional) |
| BTC | **MBT micro** (dated) | IBS mean-rev | 1.17 | 30% | 1.08 | ✅ but dated (roll), not perp |
| XRP / DJ30 / ADA / DOGE | — | — | <1.1 OOS or bear-fail | — | — | ❌ skip |

## Why TREND, not mean-rev — and how we capture BTC's big moves
BTC makes large, fast directional dollar moves. The engine captures them via
**long+short trend (TSMom50) on the perp**, not mean-reversion:
- **Trend RIDES the fat tail** — enters in the move's direction, holds while it runs
  (n=73 trades, infrequent), so the few big winners dwarf cost. Mean-rev FADES the move
  (caps upside) and trades often (n=232) so the nano fee-wall eats it.
- **Long+short = capture up-legs AND down-legs.** 2022 bear PF 1.53 = the short side working
  — exactly the perp unlock the old spot-only book lacked.
- **Vol-target sizing** = size UP in calm, DOWN in the violent expansion. Rides the trend
  without a fixed stop blowing up on a whipsaw; caps DD.
- **The perp (SQF) is the right wrapper for this**: no monthly roll to break a multi-month
  trend, spot-tracking clean signal, 24/7 entry, shorts native. Daily TFA is the only carry
  (maintenance waived to Jun-2026).
Net: a long+short, vol-targeted trend engine on ETH/BTC SQF is how we monetise the
volatility. Mean-rev only works where the fee-wall is low (SOL-SQF big notional, or
BTC/ETH on micro/standard dated futures).

## Per-coin verdict (all we can access)
| coin | best @ real cost | verdict |
|---|---|---|
| ETH | TSMom50 SQF OOS 2.39 | DEPLOY trend |
| BTC | TSMom50 SQF OOS 1.55 (perp) / IBS on MBT 1.17 (dated) | DEPLOY trend perp; MR on micro |
| SOL | IBS SQF OOS 1.04 / bear 1.34 | marginal — watch, confirm fee |
| XRP | all <1 OOS | skip |
| LTC/BCH (IB spot ~15bps) | IBS covid 2.0/2.6 but OOS<1.05 | skip (recovery-only) |
| ADA/DOGE (no CME SQF) | TSMom OOS 2.41/1.13 but bear 0.54/2.22 mixed | not all-weather; not tradeable via CME anyway |
| SPX/NDX index SQF | TSMom50 OOS 2.23/1.65 | DEPLOY index trend |
| DJ30 / Russell | weak / no data | skip |

## Caveats (faithful-SCREEN, not deploy gate)
- Daily bars; SQF-on-spot proxy; cost = modeled real (exact CME SQF fee unconfirmed — 1 number).
- DDs are pre-final-sizing (vol-target applied; portfolio-level sizing/cap still to set).
- NDX-Donch OOS PF 4.34 dropped — n=3, noise.
- Next gate: real SQF L1 spread + exact fee (gateway) → confirm SOL-IBS + pin trend cost.

## EXACT cost (confirmed 2026-06-24)
CME SQF Globex transaction fee = **$0.10 member / $0.20 non-member per side** (XRP/SOL SQF
notice; applies to SQF class). Maintenance/TFA fee **WAIVED through Jun-2026**. + IBKR crypto-fut
commission ~$0.10-0.25/side. RT ≈ **$0.60-0.90/contract** → bps by notional: BTC-SQF ~14 ·
ETH-SQF ~28 · SOL-SQF ~11 · XRP-SQF ~16 · index-SQF ~2-3. (Lower than the conservative re-test —
helps the marginal legs.) Exact CME fee final-confirm via Fee Finder / gateway contractDetails.

## FINAL PORTFOLIO (inverse-vol, exact cost, daily MTM)
Correlation: BTC-TSMom↔ETH-TSMom 0.64 (one crypto-trend bet) · NDX↔SPX 0.80 (one index bet) ·
**SOL-IBS ↔ all = 0.02-0.05 (ORTHOGONAL)** · crypto↔index 0.08-0.14.
=> ~3 independent bets: crypto-trend, index-trend, SOL mean-rev.

| portfolio | net% | Sharpe | maxDD% |
|---|---|---|---|
| worst single leg (BTC-TSMom) | 867 | 0.68 | **190** |
| equal-weight blend | 186 | 0.82 | 49 |
| **inverse-vol blend** | 88 | 0.74 | **19.9** |
| inverse-vol, OOS 2023+ | 48 | 0.81 | **16.6** |
Inverse-vol weights: SOL-IBS .35 · SPX .28 · NDX .23 · ETH .09 · BTC .06.
Blending orthogonal legs cuts maxDD ~10x (190%->20%). Curve: eq/PORTFOLIO_invvol.csv.

## Refinement
- NDX≈SPX (0.80) -> keep ONE (SPX, better Sharpe/DD). BTC≈ETH (0.64) -> size as one crypto-trend sleeve.
- Final book = [crypto-trend (BTC+ETH)] + [index-trend (SPX)] + [SOL-IBS orthogonal] @ inverse-vol/risk-parity, target portfolio DD ~15-20%.
- Index legs earn their place via DD-reduction (low vol, uncorrelated to crypto), not raw return.

## IB ACCESS — check + activate (operator-side; gateway not externally reachable by design)
NEEDED: (1) market data + (2) trading permission, BOTH on the account.
1. **Market data**: Client Portal -> Settings -> Market Data Subscriptions -> add
   **"CME Real-Time (NP, L1)"** (~$11/mo; covers CME Globex futures L1 incl crypto + index SQF).
2. **Trading permission**: Client Portal -> Settings -> Account Settings -> Trading Permissions ->
   **Futures** -> enable + the **crypto/digital-asset futures** product line (IB gates crypto
   derivatives behind an extra permission + suitability questionnaire). SQF are FUTURES (not spot/
   Paxos) -> need Futures + crypto-futures sub-permission, NOT the spot-crypto permission.
3. **Verify live**: in TWS pull symbol BTC / secType FUT / exchange CME -> SQF expiry shows live
   bid/ask = entitled; "requires subscription"/greyed = not. Confirm on the LIVE account (port 4001).

## SHADOW cost model — ALL charges + slippage (itemized, per contract)
| component | per side | basis |
|---|---|---|
| IBKR commission | ~$0.25 | nano crypto-future, fixed low end |
| CME exchange fee | $0.20 | non-member SQF (member $0.10) |
| CME/NFA regulatory | ~$0.02 | per contract |
| **fixed sub-total** | **~$0.47/side → $0.94 RT** | |
| spread cross | ~1-2bps | half-spread ×2 |
| slippage (adverse fill) | modeled 3-7bps | thin nano book |
=> loaded RT bps by notional: **BTC ~18 · ETH ~35 · SOL ~14 · index ~4**. TFA/maintenance WAIVED to Jun-2026.

## LAST-6M (huge swings) — full charges + slippage — WHICH ENGINES WORK
| engine | 6m PF | 6m DD | OOS PF | OOS DD | verdict |
|---|---|---|---|---|---|
| ETH-TSMom (trend) | 2.59 | 15.5% | 2.43 | 24% | ★ WORKS — rides swings |
| BTC-TSMom (trend) | 1.58 | 14.8% | 1.58 | 48% | ★ WORKS |
| NDX-TSMom (trend) | 1.51 | 12.6% | 1.64 | 27% | ✅ WORKS (index) |
| BTC-IBS (mean-rev) | 1.17 | 10.6% | 1.01 | 34% | ✅ marginal-OK |
| SOL-IBS (mean-rev) | 1.16 | 7.8% | 1.07 | 20% | ✅ WORKS (low DD) |
| SOL/XRP-TSMom, SPX, ETH-IBS, XRP-IBS | <1.1 | — | — | — | ✗ skip |
Slippage barely dented the winners (trend = low turnover). 6m DDs 8-15% (vol-target controlling the swings).

## Bull/bear protection -> max profit, min DD
1. **LONG+SHORT (perp) = the regime protection.** Bull: trend goes long, rides up. Bear: trend goes
   SHORT, rides down (2022 PF 1.5+; long-only 2022 was -65/-90%). Never forced long into a bear.
   TSMom's sign IS the regime call -> self-switching, no separate bull/bear flag.
2. **Vol-target sizing = DD control.** Size DOWN when realized vol spikes (crash/expansion), UP in calm.
   Keeps 6m DD to 8-15% through the big swings.
3. **Orthogonal portfolio blend = min DD.** crypto-trend (BTC/ETH) + index-trend (NDX) + SOL-IBS
   (rho ~0.04 to all) -> inverse-vol blend cuts worst-leg maxDD 190% -> ~20%.
4. **Max profit = trend rides the fat tails** (the big BTC/ETH moves); the few big winners drive return
   (ETH-TSMom 6m 2.59). Mean-rev adds uncorrelated singles between trends.
Net stance: long+short vol-targeted trend (bull AND bear capture) + orthogonal MR diversifier,
blended inverse-vol -> max-profit from the swings, min-DD from sizing + diversification + the short side.
