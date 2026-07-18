# BTC LOW-THRESHOLD mimic — lower entry point + additional 4x cells — S-2026-07-18ab

## Ask (operator, 2026-07-18)
"Find a lower entry point — we simply have to trade more on BTC, it is the best coin and we
have only long/spot to trade this. Min cost is 30bp — factor that in for the start of the
mimic. Then look at how we add additional 4x mimic."

Context: BTC was the ONLY coin still detecting at 2.0% (whole fleet runs 1.5%). Its 4 live
cells (UJ2-BECASC W4/g0.5, UJ20-BECASC-F W2/g0.75, UJ2W8-MIM W8, UJ20-BECASC-S W12/g0.20)
all fire at det=+2.00%.

## Why this is NOT the tombstoned low-thr study
`UPJUMP_LOWTHR_STOPRESCUE_FINDINGS.md` (ALL-FAIL at thr≤2.5%) killed **IMMEDIATE entry +
pre-BE stop** — that mechanism bleeds pre-BE on noise triggers. This study is the **BE-ENTRY
confirm60 anchored mimic** (BeFloorOnOpenFoundation): the leg books NOTHING until fav≥60bp,
then opens floored at BE. Lowering detection adds triggers WITHOUT reopening the pre-BE
window — different mechanism, so the old kill does not apply
(feedback-verify-kill-replicates-mechanism, in reverse).

## Cost ("start of the mimic")
Operator min cost 30bp RT. confirm_bp=60 = **exactly 2× the 30bp min cost** — the
BeFloorOnOpen foundation minimum (confirm ≥ 2× RT) holds with zero margin to spare at 30bp,
and the sweep base ran RT=30 (stricter than the fleet's 28bp proxy) with the harness's full
2×=60bp re-sim as the stress leg.

## Method
Harness `eth_ujmimic_15_becascade_bt` rebuilt vs HEAD engine (ChimeraCrypto a723af4, honest
S-17f fills — books the actual piercing fill). UM_COIN=BTC, confirm60/anchor1/legs8/RT=30.
Baseline reproduction FIRST: at cert params (thr2.0/rt28) the harness reproduces the vault
twin-tier numbers to the digit (W2/g0.75 n=1798 net=+2273% PF5.33 worst −781.7bp) — faithful.

Data: BTCUSDT_1h 2021-01-01→2026-07-18 (48,568 rows), integrity gate PASS.
**Data incident (fixed in-session):** `fetch_coin_1h.py BTC` REWRITES the csv from
2023-01-01 — it silently dropped 2021-22 (48,426→31,060 rows). Restored from git
(→2026-07-05) + appended the fresh tail from the stashed pull; integrity re-PASS. Do not
run fetch_coin_1h.py on a full-history csv without stashing/merging.

Grid: thr {0.5, 0.75, 1.0, 1.25, 1.5, 2.0}% × W {1,2,4,8,12,24} × g {0.2,0.5,0.75}, legs8.

## Result: ALL 108 cells PASS the full standalone gate
Gate = net>0 ∧ PF≥1.3 ∧ WF-H1>0 ∧ WF-H2>0 at base 30bp AND 2×=60bp, OMIT-2022
(long-only spot — feedback-crypto-omit-2022-longonly). Zero fails anywhere in the grid —
deep plateau, no knife-edge. Net *rises* as thr drops (more triggers, same floored
mechanics): thr=0.5% W4 lanes book ~2.9× the trades of the 2.0% baseline.

Trade-count scaling (W4 lane): 2.0% n=2274 → 1.0% n=4783 → 0.75% n=5714 → 0.5% n=6512.
Saturation check at 0.25%: n≈6914 (+6% vs 0.5%) — the 60bp confirm becomes the binding
entry filter below ~0.5%, so 0.5% is the practical floor, not a marginal scrape.

## Picks — the additional 4× BTC cells (thr=0.5%, best 2×-cost net per W lane)

| cell | W | g | n | net% | PF | worst_bp | WF H1/H2 | 2×net% | retire_bp (2× worst) |
|---|---|---|---|---|---|---|---|---|---|
| BTC-UJ05-BECASC-W1  | 1  | 0.20 | 6215 | +6963 | 16.57 | −566.9 | +4015/+2949 | +5122 | −1134 |
| BTC-UJ05-BECASC-W2  | 2  | 0.20 | 6639 | +7782 | 19.31 | −424.9 | +4567/+3215 | +5815 | −850 |
| BTC-UJ05-BECASC-W4  | 4  | 0.75 | 6512 | +8297 | 8.17  | −783.7 | +4654/+3643 | +6539 | −1567 |
| BTC-UJ05-BECASC-W12 | 12 | 0.20 | 5181 | +6164 | 23.41 | −484.7 | +3508/+2657 | +4630 | −969 |

W2/W12 g0.20 also beat their g0.75 siblings on PF and tail at near-equal net; W4 g0.75 is
the max-2×-net pick per fleet convention. ADDITIVE alongside the four 2.0% cells
(independent books, cap8 each; companion-independent-engine — judged standalone only).

Combined: BTC goes 4 → 8 cells; backtest trade count roughly triples
(existing 4 cells ≈9,255 clips vs new 4 ≈24,547 over the same 5.5y window).

## Honest tail framing (S-17f discipline)
nNeg > 0 on every cell (e.g. W4/g0.75: 2712 neg clips, sumNeg −1157%, worst −783.7bp).
The floor REDUCES the tail, it does not eliminate it: gap-through fills pierce the BE floor
and are booked at the ACTUAL fill. floorMinBp = worst single clip −425…−784bp per lane.
Never restate this as nNeg=0.

## Wiring (shipped this session)
`src/main.cpp` `_bc_btc_lowthr_cells` block (mirrors `_bc_fast_cells`/`_bc_slow_cells`,
OWN feed vector `_bc_btc_lt_feeds` with its own reserve — appending to `_bc_fs_feeds`
past its reserve would dangle every earlier `_grid_feeds` pointer). Factory
`make_becascade_cell`: confirm60 anchored, mimic_floor, stagger BE_CASCADE, reclip 0,
loss_cut 0, cap8, catchup 24h. `_grid.reserve(220)` headroom: 132→136 live companions.
Build green; 10/10 regression suites PASS.

## ADDENDUM S-2026-07-18ac — "all 3 of these": the 1.5% + 1.0% quads SHIP too
Operator confirmed all three per-thr quads from the sweep ship (0.5% went first in 18ab).
+8 cells, same cert (exact harness reruns reproduce the transcript tables), same gate —
BTC now 16 books: 2.0% quad + UJ15 (1.5%) + UJ10 (1.0%) + UJ05 (0.5%).

| cell | W | g | n | net% | PF | worst_bp | 2×net% | retire_bp |
|---|---|---|---|---|---|---|---|---|
| BTC-UJ15-BECASC-W1  | 1  | 0.75 | 1999 | +2748 | 6.50  | −783.7 | +2293 | −1567 |
| BTC-UJ15-BECASC-W2  | 2  | 0.20 | 2800 | +3650 | 17.72 | −702.4 | +2824 | −1405 |
| BTC-UJ15-BECASC-W4  | 4  | 0.20 | 3293 | +4180 | 19.67 | −482.5 | +3203 | −965 |
| BTC-UJ15-BECASC-W12 | 12 | 0.75 | 3136 | +3922 | 6.28  | −482.5 | +3065 | −965 |
| BTC-UJ10-BECASC-W1  | 1  | 0.20 | 3581 | +4632 | 19.43 | −566.9 | +3570 | −1134 |
| BTC-UJ10-BECASC-W2  | 2  | 0.20 | 4397 | +5621 | 19.35 | −483.5 | +4315 | −967 |
| BTC-UJ10-BECASC-W4  | 4  | 0.20 | 4783 | +6112 | 20.03 | −482.5 | +4692 | −965 |
| BTC-UJ10-BECASC-W12 | 12 | 0.75 | 3997 | +5129 | 6.45  | −783.7 | +4064 | −1567 |

All PASS the full standalone gate at base 30bp AND 2×=60bp, omit-2022. Quad sums:
UJ15 n=11,228 net=+14,500% (2× +11,385); UJ10 n=16,758 net=+21,494% (2× +16,641).
Note different-thr books share triggers on big moves (a +1.5% jump fires all three
families) — they are separate books by design (companion-independent, judged standalone),
so the quad sums do NOT add as portfolio-independent PnL.

Shipped: ChimeraCrypto Mac 43816e0 / box e3ed03d, reconciled merge 01a30d2 = running
build. Boot verified: [MIMIC-FLOOR-GATE] 144/144 0 VIOLATION, all 8 [CLIP-INIT] lines,
RUNTIME MODE = LIVE 41-sym pilot intact (no git reset used — feedback-josgp1-no-git-reset).
