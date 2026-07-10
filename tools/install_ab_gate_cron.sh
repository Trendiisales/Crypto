#!/usr/bin/env bash
# Idempotent installer for the 200-day regime-gate SHADOW A/B cron (S-2026-07-10).
#
# WHY A SCRIPT (feedback-crontab-edit-via-script, HARD RULE): inline sed/heredoc crontab pastes
# wiped the whole crontab on 07-04 -> operator furious. NEVER edit the crontab by hand. This
# installer: (1) backs up the current crontab to /tmp/ct.bak.<ts>, (2) drops any prior tagged
# line then adds the current one (update-safe), (3) is safe to re-run. Restore a wipe with:
#   crontab /tmp/ct.bak.<ts>
#
# WHAT RUNS: build/shadow_refresh_intraday with AB_GATE_TREND=200 + a SEPARATE datadir
# (data/ibkrcrypto_ab_gate). The ETH/SOL trend legs run per-symbol close>SMA200 (200 DAYS,
# converted to leg-tf bars) + vol-target OFF; BTC + *_upjump companions are NEVER gated. Writes
# ONLY to its own state.json/ledger.csv -> the LIVE intraday book, stall_companion + GUI are
# untouched. NO scp to the VPS (isolated paper A/B). At :06, after fetch(:00/:03)+live(:03).
#
# COMPARE after a few weeks: realized_usd in
#   live  data/ibkrcrypto_intraday/state.json  (ungated + vol-target)
#   A/B   data/ibkrcrypto_ab_gate/state.json    (200d-gated, vt-off)
# Entity: Memory-Chimera wiki/entities/CryptoRegimeGateSelective.md
set -euo pipefail

TAG="# [ab-gate-200d-shadow S-2026-07-10]"
LINE='6 * * * * /Users/jo/Crypto/tools/run_ab_gate_shadow.sh >> /tmp/ab_gate_shadow.log 2>&1  '"$TAG"

TS=$(date +%Y%m%d_%H%M%S)
BAK="/tmp/ct.bak.$TS"
crontab -l > "$BAK" 2>/dev/null || true
echo "[install_ab_gate_cron] backed up crontab -> $BAK"

if crontab -l 2>/dev/null | grep -qF "$TAG"; then
    if crontab -l 2>/dev/null | grep -qF "$LINE"; then
        echo "[install_ab_gate_cron] already current -> no-op"; exit 0
    fi
    echo "[install_ab_gate_cron] updating existing tagged line"
fi
{ grep -vF "$TAG" "$BAK"; echo "$LINE"; } | crontab -
echo "[install_ab_gate_cron] INSTALLED/UPDATED:"
crontab -l | grep -F "$TAG"
