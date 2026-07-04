#!/usr/bin/env bash
# Idempotent installer for the CryptoWaveCompanion hourly cron (S-2026-07-05).
#
# WHY A SCRIPT (feedback-crontab-edit-via-script, HARD RULE): inline sed/heredoc crontab
# pastes wiped the whole crontab on 07-04 -> operator furious. NEVER edit the crontab by
# hand. This installer: (1) backs up the current crontab to /tmp/ct.bak.<ts>, (2) adds the
# wave_companion line ONLY if its marker tag is absent, (3) is safe to re-run any number of
# times. Restore a wipe with:  crontab /tmp/ct.bak.<ts>
#
# Scope: ENGINE-ONLY (compute state.json + ledger.csv Mac-side). NO scp push to the VPS desk
# yet -- the bank_bp/legs push side (josgp1 vs Omega) is an OPEN operator decision; add the
# scp leg here once that is settled.
set -euo pipefail

TAG="# [wave-companion S-2026-07-05]"
# engine at :07, then the effect-level self-test at :08 (writes STATUS + logs RED between sessions)
LINE='7 * * * * cd /Users/jo/Crypto/backtest && /Users/jo/Crypto/build/wave_companion >> /tmp/wave_companion.log 2>&1; /usr/bin/python3 /Users/jo/Crypto/backtest/wave_companion_selftest.py >> /tmp/wave_companion_selftest.log 2>&1  '"$TAG"

TS=$(date +%Y%m%d_%H%M%S)
BAK="/tmp/ct.bak.$TS"
# capture current crontab (empty crontab is not an error)
crontab -l > "$BAK" 2>/dev/null || true
echo "[install_wave_companion_cron] backed up crontab -> $BAK"

# Idempotent + UPDATE-safe: drop any prior tagged line, then add the current one. Re-running
# after editing $LINE refreshes the schedule in place (no duplicate, no stale line left behind).
if crontab -l 2>/dev/null | grep -qF "$TAG"; then
    if crontab -l 2>/dev/null | grep -qF "$LINE"; then
        echo "[install_wave_companion_cron] already current -> no-op"; exit 0
    fi
    echo "[install_wave_companion_cron] updating existing tagged line"
fi
{ grep -vF "$TAG" "$BAK"; echo "$LINE"; } | crontab -
echo "[install_wave_companion_cron] INSTALLED/UPDATED:"
crontab -l | grep -F "$TAG"
