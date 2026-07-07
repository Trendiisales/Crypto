# ADA late-arm giveback fix — candidate sweep VERDICT (S-2026-07-07, operator item 3)

**Harness:** `latearm_bt.cpp` drives the UNMODIFIED live header
(`UpJumpLadderCompanionLive.hpp`, scp'd from chimera-direct @98c92d6) + an X-variant
with the 3 candidate gates (default-off, fidelity-checked byte-exact vs live, all 10
coins). Exact live spec: be_floor, be_bp=20, T1 trail 20bp / T2 trail 150bp, cap 2,
self-detector 2h/+1%, H1-close driven, RT 20bp. Binance 1h 2021-01..2026-07 (OP from
2022-06), 10 coins, integrity-scanned (DOGE 70% bar = real Jan-2021 WSB pump).
Judged on the HONEST REAL column (worse-of H1-close fill − 20bp RT); model column
(fill-at-floor, cost-free) reported alongside. Full table: `latearm_report.txt`.

## Incident replication (mandatory before any verdict)

Live ADA loss (07-06): T1+T2 ENGINE_EXIT real −122.12bp ×2 = **−244.2bp**, entry
0.186050, mfe 0.00, 7 bars.

- Sim reproduces the failure class on the same event: detector entered 15:00, legs
  opened 16:00 close 0.18550 (the local top, +198bp above ref), next close tripped
  the floored trail → real **−90.08 ×2** (model 0.00).
- The live version was WORSE for an extra reason: leg `entry_ts=...600077` is
  tick-time, not bar-time → the legs were **`seed_open`-rehydrated OPEN at 0.186050
  mid-event by a restart**, bypassing the +20bp open gate entirely, then flushed at
  event end. The "late arm" was partly a **rehydrate artifact**, not the open gate.

## Candidate verdicts — ALL FAIL (REAL column, roster Σ, 2021-26)

| variant | n | net REAL bp | PF | verdict |
|---|---|---|---|---|
| BASELINE (live) | 55,922 | **−1,132,507** | 0.71 | reproduces the S-2026-07-07f audit −1.13Mbp exactly |
| (a) open cutoff 2..12 bars | 25k..47k | −549k .. −993k | 0.70-0.72 | best (N=2) halves bleed, still deep red |
| (b) expand-K 1..6 | ~55k | −1.10M .. −1.12M | 0.71 | no effect (opens are mostly "fresh-high" anyway) |
| (c) max peak-giveback 20-50% | ~56k | −1.13M | 0.71 | no effect |
| structural: drop T1 / trail 300-500 / be100-200 | 10k..52k | −205k .. −1.02M | 0.73-0.79 | direction right, never positive |

No variant makes ANY coin all-6 positive on real fills. 44k of 56k baseline clips are
real-negative. **Late-arm timing is NOT the bleed mechanism** — the bleed is the BE
floor itself: `stop = max(le, hwm·(1−trail))` floors every leg at entry, so every
ordinary pullback through entry exits at a below-entry H1 close (slip + cost),
exactly the floor-churn that killed BeFloorMimicExit and the Omega gold/XAG/oil/FX/
stock BE-floor companions (registry §5).

## Per-tick stop evaluation — TESTED, ALSO DEAD (the floor is the disease)

First instinct said the −1.13M real vs +3.6M model gap was H1-close fill slippage
(stop trips mid-hour, engine only sees the close ~65bp lower), so "stop-fill + cost
= model − n·20bp = +2.48M" looked positive. **That bound was INVALID** — it assumed
the clip count doesn't change. An intrabar-stop sim (BeSim in the harness: stop from
prior-bar hwm vs bar low, fill = min(open, stop), gap-through honest; close-mode
self-check reproduces the live baseline byte-exact) shows per-tick evaluation makes
it WORSE:

| mode | n | net REAL bp | PF |
|---|---|---|---|
| live (H1-close stops) | 55,922 | −1,132,507 | 0.71 |
| intrabar stops (live tiers) | 84,688 | **−2,572,638** | 0.04 |
| intrabar be100/t300 | 45,576 | −1,333,157 | 0.05 |
| intrabar be200/t500 | 25,662 | −746,575 | 0.06 |

Mechanism: the floor pins stop ≈ entry, so ANY intra-hour dip below entry exits at
breakeven-fill − 20bp RT = a guaranteed −20bp per open, and per-tick tripping
explodes churn (84k/84.7k clips negative). Close-mode's slip-bleed was actually
MITIGATED by an accidental filter (only dips persisting to the close trip it).
The BE floor loses BOTH ways: close-driven = slip bleed, tick-driven = churn bleed.
Same disease as every other BE-floor tombstone (BeFloorMimicExit, gold/XAG/oil/FX/
stock/index companions, registry §5). **No exit-timing or open-timing variant of a
floored trail is real-fill viable. The floor itself must go.**

## Recommendations (operator decision, no live config touched)

1. Candidates (a)/(b)/(c): **DEAD — do not wire.** Per-tick stops: **DEAD.**
2. Fix the rehydrate artifact regardless of everything else: `seed_open` re-opens
   legs at event price mid-event with no +be_bp discipline (ADA's extra −32bp/leg vs
   organic). Mechanism-true fix: rehydrate legs FLAT with ref = event entry (they
   re-open only on a fresh +be_bp move).
3. The only trail family that passed all-6 on REAL fills remains the NO-FLOOR
   giveback-from-peak tiered ladder (upjump_tiered_ladder_sweep 2026-07-05,
   roster_cfg.csv) — the spec the floor replaced. Reverting to it (or accepting the
   floored book runs model-column-only as a display fiction with real column folding
   the truth) is the operator's call.
