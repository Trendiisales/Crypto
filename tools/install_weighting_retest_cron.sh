#!/bin/bash
# ─────────────────────────────────────────────────────────────────────────────
# install_weighting_retest_cron.sh — idempotent cron installer for the monthly
# UpJump weighting/extension re-test (S-2026-07-08, task 3).
#
# Pattern: Omega tools/install_*_cron.sh + feedback-crontab-edit-via-script —
# NEVER edit the crontab inline/by hand: this script backs up the current
# crontab, strips any previous copy of its own entry (idempotent), appends the
# new one, and installs. Restore path: crontab /tmp/ct.bak.<ts>
#
# THE OPERATOR SESSION RUNS THIS — it is committed, not executed, by AI sessions.
#
# Cadence: monthly, 1st of month 02:17 local (after the hourly data fetch has
# long since refreshed the 1h csvs; odd minute avoids the :00 herd).
# ─────────────────────────────────────────────────────────────────────────────
set -euo pipefail

REPO="$(cd "$(dirname "${BASH_SOURCE[0]}")/.." && pwd)"
TAG="# CRYPTO-WEIGHTING-RETEST (installed by tools/install_weighting_retest_cron.sh)"
CMD="17 2 1 * * cd $REPO && bash tools/run_weighting_retest.sh >> $REPO/outputs/weighting_retest_cron.log 2>&1 $TAG"

TS="$(date +%s)"
BAK="/tmp/ct.bak.$TS"
crontab -l > "$BAK" 2>/dev/null || : > "$BAK"
echo "[INSTALL] crontab backed up to $BAK"

NEW="$(mktemp)"
grep -vF "CRYPTO-WEIGHTING-RETEST" "$BAK" > "$NEW" || true
echo "$CMD" >> "$NEW"
crontab "$NEW"
rm -f "$NEW"

echo "[INSTALL] installed monthly re-test cron:"
crontab -l | grep -F "CRYPTO-WEIGHTING-RETEST"
echo "[INSTALL] restore previous crontab with: crontab $BAK"
