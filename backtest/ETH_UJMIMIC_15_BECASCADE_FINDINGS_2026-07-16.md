# ETH self-trigger BE-cascade mimic (+1.5%, restack-at-BE) — STANDALONE PASS (2026-07-16)

**Verdict: VIABLE. The operator's EXACT spec passes the standalone gate — decisively, robustly, both
WF halves, at 2× cost, positive EVERY year including both bears.** Prior "ETH mimic NO-GO" verdicts
tested a DIFFERENT spec (7% single-leg mimic; or the #2 parent-exit overlay). This is the operator's
spec, faithfully, and it is net-positive on its own additive book.

## Operator spec (2026-07-16, verbatim intent)
"If the overall price goes up by 1.5% enter a trade, lock it in; as soon as we get to BE open ANOTHER
mimic trade; the mimic exits when the price reverses; the mimic has NO effect on the overall trade."
→ a SELF-TRIGGERING, BE-floored, BE-cascade restacking, reversal-exit long book. Independent/additive
(feedback-companion-independent-engine). Judged STANDALONE — its OWN book after ITS OWN cost, never vs
a parent.

## Faithful expression (REAL live UpJumpLadderCompanion, net_bp_real, parity by construction)
`det_w=W det_thr=0.015` (+1.5% trigger, self-detect) · `mimic_floor=true` (BE floor le*(1+RT),
never-neg after arm = "lock in") · `mimic_stagger + stagger_mode=1` BE_CASCADE (release next leg the
moment the current reaches BE = "open another at BE"; at most ONE un-BE'd leg at a time) ·
`mimic_giveback=g` (reversal exit; g=1.0 = reversal-only, literal spec) · `reclip=0` (cascade guarantee)
· L legs. Harness `eth_ujmimic_15_becascade_bt.cpp`. ETH 1h 48,425 bars 2021→2026. MEASURED cost 28/56bp.

## Result — 4-leg PASSES every config; 2-leg fails 2×-cost PF
| W | g | legs | n | net% | PF | 2× net% | worst_bp | H1/H2 | every-year+ | GATE |
|---|---|------|---|------|----|---------|----------|-------|-------------|------|
| 4 | 1.0 | 4 | 3397 | +2281 | 2.73 | +1500 | −1319 | +1310/+971 | ✅ incl 22/26 | **PASS** |
| 4 | 0.5 | 4 | 3397 | +2425 | 2.84 | +1531 | −1319 | +1412/+1013 | ✅ | **PASS** |
| 24| 0.5 | 4 | 2109 | +1507 | 2.98 | +1015 | −1100 | +794/+714 | ✅ | **PASS** |
| — | — | 2 | ~1500 | +360..+586 | 1.6-1.9 | +122..+242 | — | — | — | fail (2× PF<1.3) |

**All 24 four-leg rows PASS** (W∈{1,4,12,24}×g∈{1.0,0.75,0.5}) = broad plateau, NOT a spike = robust.
2-leg ("just one more") is real but marginal; the EDGE is the DEEPER pyramid — 4 floored legs riding
the fat-tail runs, each locked at BE, while the cascade holds at most one un-BE'd leg at a time.

## The never-negative "lock-in" guarantee — proven empirically
Pre-BE cut sweep on W4/g0.5/4-leg: worst clip tracks the cut EXACTLY —
`lc=0 → −1319 · lc=500 → −528 · lc=300 → −328 · lc=150 → −178`.
→ every deep negative is a PRE-BE leg (cappable at will). Armed (BE'd) legs never appear below the
floor = the guarantee holds by construction (engine `floored` flag: post-arm exit can't go negative).
Edge survives the cut: **lc=300 → +2369% PF2.84, 2× +1453, worst −3.28%/leg, all years +, PASS.**

## Honest caveats (BACKTEST_TRUTH)
1. **Concurrent notional / sizing:** 4 legs = up to 4× notional deployed (staggered, ≤1 un-BE'd at a
   time so at-risk capital is ~1 leg). net% is Σ per-leg. PF IMPROVES with stacking (1.45→2.84) so it
   is a structural edge, not just leverage — but real $ size is the operator's lot-size call
   (project-revisit-lot-sizes). 
2. **High turnover:** ~1.7 clips/day (3397 over 5.5y). Each a real Binance spot round-trip; cost incl.
3. **Pre-BE bleed** (sumNeg ~−1300%) is the admission price of the fat-tail; bounded per-leg by the
   reversal exit and cappable via loss_cut (recommend lc=300 → worst −3.28%/leg).

## Recommendation
VIABLE new STANDALONE additive engine for ETH. Recommend **W=4 (4h), 4-leg BE-cascade, loss_cut=300bp**,
g = operator's call (1.0 literal reversal-only +2281 PF2.73, or 0.5 tighter +2369 PF2.84). New-engine
adverse-protection mandate SATISFIED (this doc = the backtested loss-protection verdict). Ship as
live-shadow on josgp1 (independent book, touches nothing else). Extend to other coins per the
never-half-the-symbols standing order before/after ETH go-live.
