# Staggered floored mimic-ladder findings — 2026-07-16

Operator ask: *"once we get to BE, I want multiple mimic opening ... stagger them and open at
least 4x mimics, with all the protections in place to ensure we do not ever go negative on a
mimic."* i.e. port the just-shipped stock TURTLE A/B/C/D staggered-cell pattern onto crypto.

## Mechanism

Extended `UpJumpLadderCompanion` (ChimeraCrypto `include/core/UpJumpLadderCompanion.hpp`): a
`mimic_floor` cell may now run N legs (`mimic_stagger=true`), each with its own per-tier
`confirm` (escalating BE-entry **20/120/220/320 bp = BE/+1/+2/+3%**). Each leg opens at a
DIFFERENT price and floors at its OWN fill (`le·(1+RT)`) — so the legs are DISTINCT additive
positions, NOT the redundant same-entry N-multiply the single-leg block guarded against. All
legs eligible; per-leg `confirm` gates the opens (not `advance_stagger_`); `cap == #tiers`
(no self-funding ladder). `mimic_giveback` (g) stays uniform (the g-sweep proved tightening
HURTS these trend cells). Single-leg cells are byte-identical (the block only relaxes when
`mimic_stagger` is set).

Harness: `Crypto/backtest/upjump_earlyarm_bt.cpp` mode **`mimicstag`** drives the REAL extended
engine (parity by construction). Same detector windows as `mimicg`. Gate = long-only all-6 +
2x-cost (net>0 & PF>=1.3 & WF-H1>0 & WF-H2>0, AND 2x-cost net>0 & PF>=1.3), omit 2022, data
`*_1h` 2023-01→2026-07. Envs: CC_COIN CC_W CC_THR CC_RT(20) CC_CUT(0) CC_RECLIP(0.005 live)
CC_FROMYEAR(2023) CC_CONFIRMS("20,120,220,320") MF_G.

## Key results (live reclip=0.005, live per-coin g)

| coin | W/thr | g | max-robust legs | net% multi vs 1-leg | 2x-cost pf | floorMin |
|---|---|---|---|---|---|---|
| **DOGE** | 1/5.5% | 0.20 | **4x** | +386 vs +154 (2.5x) | 2.32 | +0.0 |
| **NEAR** | 1/4.0% | 0.75 | **4x** | +455 vs +195 (2.3x) | 1.57 | +0.0 |
| **BNB**  | 1/4.0% | 0.50 | **4x** | +137 vs +72  (1.9x) | 1.51 | +0.0 |
| **UNI**  | 1/3.5% | 1.00 | **3x** | +269 vs +208 (1.3x) | 1.48 | +0.0 |
| **TRX**  | 8/3.5% | 0.40 | **1x** (keep single) | staggering degrades net+robustness | — | +0.0 |

### Verdicts
1. **Additive, not just leverage — where the gate allows.** On DOGE/NEAR/BNB the 4x ladder is
   ~2–2.5x the single-leg net AND clears 2x-cost. UNI takes 3x. TRX gains nothing from stacking
   (2-leg net +95 < 1-leg +105; robustness drops) → stays single-leg.
2. **The floor is universal.** `floorMin = +0.0` in EVERY coin/leg-count/g run — no armed
   (floored) leg ever books negative. The never-negative guarantee is per-leg and holds
   regardless of leg count. Negatives live ONLY in the pre-arm tail (a leg that opens and
   reverses before covering cost), bounded by the window-reversal flush.
3. **Pre-arm loss_cut CANNOT rescue the tail** — cut60 turned BNB +86→−22 (nNeg 4→151); it
   churns like an entry-stop, exactly the single-leg g-sweep finding. Ships cut=0.
4. **reclip fidelity matters.** An initial run at reclip=0.05 (10x the live 0.005) was
   PESSIMISTIC — it failed BNB/DOGE that pass at the live reclip. The verdicts above use the
   live reclip=0.005 and reproduce the single-leg findings exactly (BNB 1-leg 2x-pf 1.60).

## Wired (S-2026-07-16, shadow deploy-forward)
`src/main.cpp` `_sweet_cells` gains a `stag_legs` column: DOGE/NEAR/BNB=4, UNI=3, TRX=1.
`make_mimic_floor_cell(..., stag_legs)` builds the escalating-confirm tiers when >1;
`retire_bp` scales x legs. Boot `[MIMIC-FLOOR-GATE]` still passes (mimic_floor=true).

## NOT yet done (follow-ups)
- **ETH-PJ7W24 (jump_floor)** + **LDO-CAMP-W8 (campaign)** — the operator's 2 currently-open
  trades are on DIFFERENT engine paths; the stagger extension must be ported into jump_floor
  and CryptoCampaignManager before they get the ladder.
- **5 REGIME-BEMIMIC cells** (det_w=0, D1 parent) — need a parent-replay harness (none exists);
  DOT/SUSHI/THETA also need 1h data pulled first.
