# Profit-lock g-sweep — 17f out-of-class ruling REVERSED (2026-07-17)

**Operator order (2026-07-17, verbatim intent):** appalled LDO campaign gave back +5.42% peak →
+2.28%. The 17f bullet-C ruling (campaigns + PJ jump_floor = directional parents, exempt from
profit-lock) is **REVERSED**. All 6 out-of-class cells must lock profit the MIMIC way (tight
giveback + BE floor). Close the open LDO trade NOW. Structural gate so no live book can ride
profit back down again.

Per `feedback-test-operator-spec-before-verdict`: the EXACT floored spec was tested per cell
before any wiring/kill verdict. Harnesses: `upjump2pct_be_bt gsweep` (new mode — hold wired
W/thr/s, sweep g fine at 1x + 2x cost) and `upjump_earlyarm_bt campaign CP_PGB` (new lever —
mimic-way giveback on the parent; CP_PGBARM 0 = arm at fee-BE point 0.9S, 1 = arm at
cost-covered, exact jf semantics).

## Why LDO gave it back (trail anomaly resolved)

`CryptoCampaignManager` parent ladder: fee-BE arm at pmfe>=0.9S (LDO S=411 → 370bp), net-lock
at 1.8S (740bp), HWM trail at **2.0S (822bp)**. LDO peaked +542bp → only the fee-BE floor armed
(pstop = pe×(1+(20+3)/1e4) = entry×1.0023 — the observed 0.358974). The 342bp trail never armed.
PJ jump_floor cells: `jf_giveback=1.0` wired = NO trail at all — floor ratchets to BE only, rides
to window exit (why ETH-PJ7W24 rode +5%+ back to ~BE).

## Result — exact operator spec (uniform tight g 0.3–0.5)

**FAILS 5 of 7 books.** Only ETH (at wired 20bp cost) and the two UNI campaign cells survive a
uniform 0.3–0.5 giveback; AAVE/GRT/TRX flip ~zero-to-negative:

| cell | g=0.5 net | g=1.0 net | verdict at 0.3–0.5 |
|---|---|---|---|
| AAVE-PJ4W1 (20bp) | +1918 fail | +19469 PASS | dead (all s-rescues fail too) |
| ETH-PJ7W24 (28bp measured) | +5062, 2x −1316 fail | +17424 PASS | fails measured-cost gate |
| GRT-PJ5W1 (20bp) | −283 fail | +22025 PASS | dead |
| UNI-CAMP-W1 | +78% | +74% | **improves** |
| UNI-CAMP-W2 | +161% | +156% | **improves** |
| TRX-CAMP-W8 | +1% (stress neg) | +92% | dead |
| LDO-CAMP-W8 (g0.4) | +38% | +68% | halved, positive |

Mechanism (replicated, not strawman — `feedback-verify-kill-replicates-mechanism`): these books
run 15–67% pre-BE losers (preBE bleed −18k..−25k bp) paid for by rare fat-tail riders. A tight
giveback amputates exactly those riders — the same kill the 2026-07-16 ETH exit-overlay study
found for time-stops/ATR-trails. CP_PGBARM=1 (arm at cost-covered, exact mimic semantics) is
uniformly worse than arming at the fee-BE point — campaigns confirm-enter +20bp above window
epx, so an immediate trail chokes on the first pullback.

## Wired configs — tightest g per cell that KEEPS the full standalone gate

Gate: n>=30 (PJ), net>0, PF>=1.3 (PJ), both WF halves >0, 2x-cost re-sim >0 (campaigns: 30bp +
40bp stress re-sims). Conservative cost: PJ verified at BOTH 20bp wired and 28bp measured-class.

| cell | NEW g | arm | net (locked) | PF | 2x/stress | note |
|---|---|---|---|---|---|---|
| AAVE-PJ4W1 | **0.85** | floored (fav>=rt) | +11063 (28bp) | 1.48 | +10775 | g0.8 fails WF-H2 |
| ETH-PJ7W24 | **0.85** | floored | +18297 (28bp) | 1.95 | +11529 | ≈ g1.0 net, lock ~free |
| GRT-PJ5W1 | **0.70** | floored | +13193 (28bp) | 1.54 | +7817 | g0.6 fails |
| UNI-CAMP-W1 | **0.50** | fee-BE (0.9S) | +78% | 2.63 | +72/+67 | better than ride |
| UNI-CAMP-W2 | **0.50** | fee-BE | +161% | 2.70 | +152/+143 | better than ride |
| TRX-CAMP-W8 | **0.60** | fee-BE | +86% | 4.66 | +83/+79 | cliff at 0.5 (+1%) |
| LDO-CAMP-W8 | **0.40** | fee-BE | +38% | 1.22 | +30/+23 | cost of the lock: −44% net; PF<1.3 flagged — cell already borderline (z=2.02, ×0.25 size, retire −1400) |

Every live out-of-class book now carries g<1.0 — a real profit lock exists on every cell, at the
tightest setting the data certifies. LDO at +542bp peak now locks pe×(1+542×0.6/1e4) = +325bp
(vs +23bp fee-BE floor that let it slide to +228). ETH at +542bp locks +81bp + BE floor (vs BE).

## What ships with it

- `CryptoCampaignManager`: per-cell `pgiveback` (arm at fee-BE point, ratchet-only), restore-time
  re-arm (restart cannot reopen a giveback window), `flatten_before_ms` flatten path (books at
  current mark through `close_campaign_` — the LDO NOW-close, reason `CAMP_FLATTEN`).
- PJ cells: `jf_giveback` 1.0 → per-cell wired value (trail code pre-existed, was configured off).
- Boot gate `[PROFIT-LOCK-GATE]`: any live campaign cell or jump_floor cell with giveback >= 1.0
  = VIOLATION (MIMIC-FLOOR-GATE style, runs every boot).
