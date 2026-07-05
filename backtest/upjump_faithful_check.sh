#!/usr/bin/env bash
# CANONICAL faithful-harness GATE for the crypto UpJump parent + companion stack.
#
# WHY THIS EXISTS: sessions kept re-writing / mis-driving one-off harnesses instead of
# using the committed byte-exact tool, and drift went UNCAUGHT (a 2>/dev/null that ate the
# stderr trade-dump manufactured a fake "0 trades"; a daily-resample misread produced a
# from-scratch Python reimplementation = an unvalidated strawman). Operator: "where are the
# checks and balances". THIS is the check.
#
# WHAT IT DOES: drives the ONE canonical binary the ONE correct way against a FIXED historical
# window and asserts per-coin parent-trade counts equal backtest/GOLDEN_UPJUMP.txt. Any mismatch
# (wrong binary build, wrong invocation, eaten stderr, corrupted data) FAILS LOUD, exit 1.
#
# RULE: before ANY UpJump/companion backtest verdict, run this. GREEN = the tool+data reproduce
# the known-answer, trust it. RED = STOP, do not verdict, do NOT write a new harness -- fix the
# tool/invocation until this passes. Writing a fresh harness to route around a RED is BANNED.
set -euo pipefail
cd "$(dirname "$0")"
BIN=./ibkrcrypto_bt
GOLDEN=GOLDEN_UPJUMP.txt
CUT=1735689600000   # 2025-01-01, the fixed golden-window cutoff
[ -x "$BIN" ] || { echo "FAIL: $BIN missing/not built. Build src/ibkrcrypto_bt.cpp first."; exit 1; }
[ -f "$GOLDEN" ] || { echo "FAIL: $GOLDEN missing."; exit 1; }

fail=0
while read -r coin want; do
  [[ "$coin" =~ ^#|^$ ]] && continue
  f="data/${coin}USDT_1h.csv"
  if [ ! -f "$f" ]; then echo "FAIL $coin: $f missing"; fail=1; continue; fi
  got=$(DUMP_TRADES=1 BT_TF_MS=3600000 "$BIN" "$coin" "$f" --dump-trades UpJump2 2>&1 \
        | awk -v cut="$CUT" '/^TRADE/ && $2 < cut {n++} END{print n+0}')
  if [ "$got" = "$want" ]; then
    printf "OK   %-5s %s\n" "$coin" "$got"
  else
    printf "FAIL %-5s got=%s want=%s  <- DRIFT: tool/invocation/data changed\n" "$coin" "$got" "$want"
    fail=1
  fi
done < "$GOLDEN"

if [ "$fail" -ne 0 ]; then
  echo "=== HARNESS DRIFT DETECTED. STOP. Do not issue any backtest verdict. Do NOT write a new harness. ==="
  exit 1
fi
echo "=== FAITHFUL HARNESS GREEN -- known-answer reproduced. Safe to backtest UpJump/companion. ==="
