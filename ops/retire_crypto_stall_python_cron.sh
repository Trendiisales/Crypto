#!/usr/bin/env bash
# retire_crypto_stall_python_cron.sh — idempotent swap of the last 3 crypto-intraday
# stall_accountant.py cron lines (SKIP_OMEGA=1) for the native C++ stall_companion exe.
#
# Removes the ACTIVE (non-comment) cron lines matching  stall_accountant.py && SKIP_OMEGA=1
# (the 3 books: intraday_sol_emax_4h / eth_donch_4h / sol_emax_1h) and ADDS one line running
# /Users/jo/Crypto/build/stall_companion (all 3 books in one invocation, wave_companion idiom).
# KEEPS every comment + everything unrelated.
#
# SAFETY (feedback-crontab-edit-via-script — a prior inline sed/heredoc WIPED the crontab):
#   * backs the live crontab up to /tmp/ct.bak.<epoch> BEFORE any change
#   * builds the new crontab with awk into a temp file
#   * installs via `crontab <file>` (atomic); idempotent (2nd run = no-op)
#   * restore with:  crontab /tmp/ct.bak.<epoch>
set -euo pipefail

BIN=/Users/jo/Crypto/build/stall_companion
CRON_LINE='* * * * * CRYPTO_STATE=/Users/jo/Crypto/backtest/data/ibkrcrypto_intraday/state.json STALL_ACCT_DIR=/Users/jo/stall-accountant COMPANION_STATE_INTRADAY=/Users/jo/Crypto/companion_intraday/companion_state.json /Users/jo/Crypto/build/stall_companion >> /tmp/crypto_stall_companion.log 2>&1  # 3 intraday stall books (native C++ port of stall_accountant.py SKIP_OMEGA)'

[ -x "$BIN" ] || { echo "[retire] FATAL: $BIN not built/executable — build it first"; exit 1; }

TS=$(date +%s); BAK="/tmp/ct.bak.${TS}"; NEW="/tmp/ct.new.${TS}"
crontab -l > "$BAK" 2>/dev/null || { echo "no crontab to edit"; exit 0; }
echo "[retire] backed up live crontab -> $BAK ($(wc -l <"$BAK" | tr -d ' ') lines)"

# 1) drop the 3 active python stall lines
awk '
  /^[[:space:]]*#/ { print; next }                                   # keep comments verbatim
  /stall_accountant\.py/ && /SKIP_OMEGA=1/ { next }                  # drop active crypto stall books
  { print }
' "$BAK" > "$NEW"

REMOVED=$(( $(wc -l <"$BAK") - $(wc -l <"$NEW") ))
echo "[retire] python stall lines removed: $REMOVED (expect 3, or 0 if already retired)"

# 2) ensure the C++ line is present exactly once
if grep -Fq "/Users/jo/Crypto/build/stall_companion" "$NEW"; then
  echo "[retire] C++ stall_companion cron line already present — not re-adding"
else
  printf '%s\n' "$CRON_LINE" >> "$NEW"
  echo "[retire] added C++ stall_companion cron line"
fi

# sanity: no active python stall line survives; exactly one C++ line
PY_LEFT=$(grep -c 'stall_accountant\.py' "$NEW" 2>/dev/null | tr -d ' ' || true); PY_LEFT=${PY_LEFT:-0}
CPP_N=$(grep -Fc '/Users/jo/Crypto/build/stall_companion' "$NEW" 2>/dev/null | tr -d ' ' || true); CPP_N=${CPP_N:-0}
echo "[retire] stall_accountant.py lines remaining (incl comments): $PY_LEFT ; C++ lines: $CPP_N"

if [ "$REMOVED" -eq 0 ] && [ "$CPP_N" -ge 1 ] && diff -q "$BAK" "$NEW" >/dev/null 2>&1; then
  echo "[retire] no change (idempotent no-op)."; rm -f "$NEW"; exit 0
fi

crontab "$NEW"
echo "[retire] installed. Restore if needed:  crontab $BAK"
rm -f "$NEW"
