#!/usr/bin/env bash
# ============================================================================
# install_binary_freshness_cron.sh  (S-2026-07-12)  — IDEMPOTENT
# Adds the binary-freshness guard to crontab (the missing BINARY staleness
# check; all prior checks were DATA-only, which is how a Jul-5 stale binary
# ran the live crypto book for weeks). Safe: backs up crontab, adds the line
# only if absent, never edits inline. Re-runnable.
# ============================================================================
set -euo pipefail
MARK="binary_freshness_guard"   # unique tag to detect prior install
WRAP="/Users/jo/Omega/tools/monitor_crashsafe_wrap.sh"
GUARD="/Users/jo/Crypto/ops/binary_freshness_selftest.py"
LINE="19,49 * * * * cd /Users/jo/Crypto && ${WRAP} -n binary_freshness -l /tmp/binary_freshness.log -k 2 -r 2 -q \"^RESULT:\" -t \"🔧 STALE CRYPTO BINARY\" -m \"BINARY-FRESHNESS RED — a live crypto binary is stale/shadowed; rebuild (cmake --build build) before trusting the book; see /tmp/binary_freshness.log\" -- /usr/bin/python3 ${GUARD} # ${MARK} S-2026-07-12"

[ -x "$WRAP" ]  || { echo "FATAL: wrapper missing: $WRAP"; exit 1; }
[ -f "$GUARD" ] || { echo "FATAL: guard missing: $GUARD"; exit 1; }

BAK="/tmp/crontab.bak.$(date +%s)"
crontab -l > "$BAK" 2>/dev/null || true
echo "crontab backed up -> $BAK"

if crontab -l 2>/dev/null | grep -q "$MARK"; then
  echo "already installed ($MARK present) — no change"
  exit 0
fi
{ crontab -l 2>/dev/null || true; echo "$LINE"; } | crontab -
echo "installed binary-freshness cron (19,49 * * * *):"
crontab -l 2>/dev/null | grep "$MARK"
