# EARLY-CONFIRM cert — intra-bar BE-ENTRY confirm fill — S-2026-07-20

## Ask (operator)
"ok that needs to change, it is stupid to wait an hour if the next be is met earlier fix
this now ... check it via a backtest i cannot see how this would make any change if the be
is met we should fire, if the entry reverses we are going to exit anyway"

Trigger: AVAX cells showed confirm crossed intra-bar (-90/-52bp past confirm) but the
BECASC self-detect path only evaluated confirm at the H1 close walk (process_close_ ->
observe_ladder_ -> step_leg_). det_w==0 parent-driven books already walked confirm per
tick — the hour wait was a self-detect-path inconsistency, NOT a certified design choice.

## Change (ChimeraCrypto include/core/MimicLadderCompanion.hpp)
`intrabar_confirm_opens_(cur, ts_ms, bar)` called per tick in observe() (det_w>0,
non-jump_floor branch, before the intrabar stop/floor checks): opens un-open confirmed
legs the INSTANT the mark covers confirm. Byte-matches step_leg_'s open block —
confirm_anchor_epx keeps le=epx (floored-on-open at window-entry BE preserved),
seeded_flat rule preserved, announce_open_ -> live mirror buys immediately.
Exits / floors / ratchets untouched (already tick-level). Immediate-entry legs
(confirm==0) keep close-open semantics.

## Cert method
`early_confirm_bt.cpp` (this dir) — same family as the S-18ab/ag/ai lowthr certs, drives
the REAL post-change MimicLadderCompanion. UM_EARLY=0 = byte-original close-confirm drive
(baseline). UM_EARLY=1 = live-faithful tick sequence with a PESSIMISTIC bound:
observe(bar HIGH) first (prior-bar close walk + intra-bar confirm fill AT THE HIGH — real
tick fill is at the confirm level <= high), then stop_check_only(bar LOW) (same-bar
worse-of vs the fresh leg), then observe(close). Spec: confirm60 BE-ENTRY anchored,
legs8, RT=30/60bp, omit-2022 gate (net>0, PF>=1.3, both WF halves, base AND 2x cost).

## Result — fleet-wide clean
35 coins (BTC ETH + 33 alts) x thr{0.5,1.0}% x W{1,2,4,12} x g{0.2,0.5,0.75} legs8:
- early=0: **840/840 PASS**
- early=1: **840/840 PASS**
- e0-PASS -> e1-fail regressions: **ZERO**

Character of the delta (AVAX thr0.5 W2 g0.2 example, representative):
n 11871 -> 14479 (+2608 fade entries the close-wait skipped), net +17503% -> +10585%,
PF 9.97 -> 3.03, worst -809 -> -1699bp, 2x-cost net +13953% -> +6485%. The EARLY numbers
are a LOWER BOUND: entry modeled at the bar high and same-bar exit at the low is maximum
whipsaw; real fills land at the confirm level and stop level. Under even this bound every
cell stays comfortably above the gate. Operator thesis ("floor exits a reversal anyway")
holds — the fade entries cost a bounded floor-tail, they cannot book pre-BE losses
beyond the honest gap tail (S-17f framing: REDUCED tail, not zero tail).

Raw outputs: `early_confirm_2026-07-20/<COIN>_thr<thr>_e{0,1}.txt`.

## Ship
ChimeraCrypto S-2026-07-20j via deploy_to_box.sh (hash-verified). Desk effect: a cell
whose window is open buys the moment price covers confirm — no more top-of-hour wait.
