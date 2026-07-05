# Crypto Backtest Truth — READ BEFORE ANY UpJump/companion backtest

Sessions kept re-writing or mis-driving one-off harnesses instead of using the committed
byte-exact tool. Drift went uncaught and produced fake verdicts (a `2>/dev/null` that ate the
stderr trade-dump = fake "0 trades"; a daily-resample misread = a from-scratch Python
reimplementation = an unvalidated strawman). Days + thousands of tokens wasted. This file + the
gate below are the checks-and-balances that stop it.

## THE ONE canonical faithful tool
`src/ibkrcrypto_bt.cpp` → built to `backtest/ibkrcrypto_bt`. It is byte-identical to the live
`refresh_shadow_intraday` path. Drive it, do not reimplement it.

Correct invocation (the two traps that bit us are baked in here):
```bash
DUMP_TRADES=1 BT_TF_MS=3600000 ./ibkrcrypto_bt <SYM> data/<SYM>USDT_1h.csv --dump-trades UpJump2 2>&1
```
- `BT_TF_MS=3600000` → 1h. WITHOUT it the binary resamples 1h→DAILY **by design** (source line 46).
  Seeing "tf=86400000ms daily" is NOT a broken build — it means you forgot BT_TF_MS.
- TRADE lines emit on **STDERR**. Merge `2>&1` or you get a false zero.
- `UpJump2` = uniform +2% (the live parent). `UpJump8` = legacy per-coin 8%.

## THE GATE — run it before any verdict
```bash
bash backtest/upjump_faithful_check.sh
```
Drives the canonical binary the correct way against a fixed pre-2025 window and asserts per-coin
parent-trade counts equal `backtest/GOLDEN_UPJUMP.txt` (an immutable known-answer; appended bars
cannot shift it). GREEN = tool+data reproduce the known-answer → trust your run. RED = STOP.

## HARD RULES
1. **Never write a new UpJump/companion harness.** If you think you need one, the gate is RED or
   you mis-drove the tool — fix that, do not route around it. A fresh harness = unvalidated = banned.
2. **Never verdict on a RED gate.** No net-negative / net-positive / kill call until the gate is GREEN.
3. **Never model the BE-floor companion without its floor.** Floorless/honest-fill/MTM-flush = fake red.
4. Data feeds through `integrity_gate.py` first (the existing data gate).

## The two live "2%" levers — do not conflate
- **Companion arm = 2%** (companion config, `arm` field) — the operator's spec, WORKS, neg=0. Commits `23907d0`/`35266b6`.
- **Parent entry threshold = 2%** (`UpJump2`, src/main.cpp) — uniform entry trigger. Commit `52c0d31`.
`git log -S"0.02" -- src/main.cpp` finds ONLY the parent lever. The companion 2% is `arm=2` elsewhere.
