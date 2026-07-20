# HONEST LEDGER + MEASURED COST — engine fix + fleet re-cert — S-2026-07-20 (operator order)

## Order (handoff 20q, operator verbatim intent)
"Sick of dishonest ledgers/inconsistency — build the honest ledger, use the CORRECT cost
(measured, not hand-quoted 30/60), run it now."

## 1. Engine-side honest booking — SHIPPED (ChimeraCrypto `MimicLadderCompanion.hpp`)
- New per-leg **`fill_px`** — recorded at the REAL open on every path: `intrabar_confirm_opens_`,
  `step_leg_`'s open block, the reclip re-entry, jf display legs, `step_be_`. It is the exact px
  `announce_open_` hands the LiveMimicMirror.
- **`emit_clip_` books gross from `fill_px`, NEVER from the anchored `le`.** All columns
  (`gross_bp/net_bp` AND `_real`) book honest — one record, no inflated reference column.
  `ClipRecord.entry_px` = the real fill; new `ClipRecord.anchor_le` preserves the anchored le
  for audits. Log line shows `(anch=...)` delta + `fill=`.
- Design mechanics UNCHANGED (certified anchored design): confirm gate, floor/stop LEVELS from
  le, cost-gate, ladder-spawn all still anchored. Only the BOOKING is real-fill.
  Shadow ledger == mirror economics by construction, all cells, forever.
- `jf_book_` (jump-floor) and `emit_be_clip_` (be_floor) were already honest — untouched.

## 2. Measured cost — WIRED (never hand-quoted again)
- Ran `depth_cost_model_bt` over data/bookdepth_perp for **15 coins** at notionals
  $100/$1k/$5k/$25k/$100k → `ChimeraCrypto/backtest/depth_cost_all15_2026-07-20.txt`.
  At pilot size ($100–1k): **every coin 28.0–29.5bp** (fee20 + p99 slips + reserve8).
  Size is where thin coins blow out (RUNE $25k=59.4, IMX $25k=248.5, HBAR $100k=91.9).
- New **`data/depth_cost_measured.csv`** ($1k p99 entry/exit slips, ≥ pilot conservative) —
  loaded at boot in main.cpp → `g_camp_cost_ledger.set_depth_slip()` BEFORE any companion
  config reads `safe_cost_bps(sym)`. Boot prints `[COST-MEASURED]` per coin; missing file =
  LOUD warning + 28.0 fee-decomposition default (slips 0), never a silent literal.
- `make_becascade_cell` rt=28.0 PROXY literal → `g_camp_cost_ledger.safe_cost_bps(sym)`.

## 3. Fleet re-cert at honest basis + measured cost — 369/380 FAIL, 11 SURVIVORS
Harness `honest_entry_basis_bt.cpp` rebuilt against the new engine:
- **Transform basis validated byte-exact**: with `ClipRecord.anchor_le`, transform mode
  (entry at `le*(1+confirm)` — the live-fill model) reproduces the S-20 RT=30 run to the digit.
- New `UM_NATIVE=1` books engine `net_bp_real` directly. NOTE: under the OHLC e1 drive the
  intrabar confirm fill lands at the fed bar HIGH → native-BT is a pessimistic BOUND
  (DOGE thr1.5 W8 g0.5: −5073% native vs +2307% transform). Live fills land ≈ the confirm
  level on a dense tick feed → the TRANSFORM cert is the honest live model; native is the bound.
- Battery: 35 coins × thr{0.5,1.0,1.5,2.0}% × W{1,2,4,8,12} × g{0.2,0.5,0.75} × legs8, e1,
  per-coin measured RT (28.0; RUNE 29.3), stress 2× measured. Raw:
  `honest_basis_measured_2026-07-20/` + `run_measured.sh` + `join_live_cells.py`.

**Live-cell verdict (join vs main.cpp 380 BECASC cells, 0 unmapped): 369 FAIL / 11 PASS.**
Survivors (ALL wide-tail g0.75 lanes; RT=30 set of 8 + 3 recovered at measured cost):

| survivor | config | net (base/2×) | PF | H1/H2 |
|---|---|---|---|---|
| DOGE-MIM05-BECASC-W2  | thr0.5 W2 g0.75 | +8038/+4041% | 1.76 | +6164/+1873 | ← new @28bp
| DOGE-MIM05-BECASC-W4  | thr0.5 W4 g0.75 | +7495/+4356% | 1.76 | +5860/+1635 |
| DOGE-MIM05-BECASC-W12 | thr0.5 W12 g0.75 | +9840/+7335% | 2.42 | +8529/+1311 |
| DOGE-MIM10-BECASC-W2  | thr1.0 W2 g0.75 | +6665/+3745% | 1.71 | +5459/+1206 |
| DOGE-MIM10-BECASC-W4  | thr1.0 W4 g0.75 | +6993/+4588% | 1.79 | +5770/+1223 |
| DOGE-MIM10-BECASC-W12 | thr1.0 W12 g0.75 | +9039/+7014% | 2.51 | +8161/+878 |
| DOGE-MIM15-BECASC-S   | thr1.5 W8 g0.75 | +7755/+5788% | 2.22 | +6345/+1410 |
| RUNE-MIM05-BECASC-W4  | thr0.5 W4 g0.75 | +4877/+2699% | 1.86 | +3073/+1804 |
| RUNE-MIM15-BECASC-F   | thr1.5 W2 g0.75 | +3105/+1787% | 1.74 | +2063/+1042 |
| AVAX-MIM15-BECASC-F   | thr1.5 W2 g0.75 | +5497/+3157% | 1.66 | +3750/+1747 | ← new @28bp
| INJ-MIM05-BECASC-W4   | thr0.5 W4 g0.75 | +5077/+2463% | 1.74 | +2870/+2207 | ← new @28bp

Mirror stays PAUSED (`acquire_pause_substr="BECASC"`). Allowlist-vs-retire remains the
OPERATOR DECISION — candidate set is now these 11.

## Consequence of the honest ledger going live
From this deploy, every BECASC/anchored shadow clip books its true real-fill economics:
the FLOOR_TRAIL churn population that booked ≈+0 (anchored) now books ≈−(confirm−RT)−RT
per clip. Shadow banks will turn visibly negative on the failed cells — that is the honest
truth the operator ordered, not a regression.
