#!/usr/bin/env bash
# run_ab_gate_shadow.sh — SHADOW A/B for the per-symbol 200DMA regime gate (S-2026-07-10).
#
# WHAT: runs the SAME build/shadow_refresh_intraday binary as the live intraday book, but with
#   AB_GATE_TREND=200 -> the ETH/SOL TREND legs (EMAx/TSMom50/Ichi/Kelt/Macd/Donch, NOT BTC,
#   NOT *_upjump companions) run per-symbol close>SMA200 + vol-target OFF. Writes to a SEPARATE
#   datadir so the LIVE ledger + stall_companion + GUI are NEVER touched. NO scp to the VPS.
#
# WHY: ablation (backtest/ibkrcrypto_bt --protect-sweep, gate OFF vs REGIME_MA=200) showed the
#   gate BEATS vol-target on high-vol trend legs (net-preserving DD cut) and is the only tool that
#   avoids bear-regime longs entirely. This forward-records the gated book alongside the live
#   ungated book so the operator can compare realized P&L before any live flip.
#   See wiki/entities/CryptoRegimeGateSelective.md (Memory-Chimera).
#
# COMPARE: live (ungated+vt)  data/ibkrcrypto_intraday/state.json
#          A/B  (gated, vt-off) data/ibkrcrypto_ab_gate/state.json   <- this book
set -euo pipefail
REPO="$(cd "$(dirname "${BASH_SOURCE[0]}")/.." && pwd)"
cd "$REPO/backtest"

# Piggyback on the live fetch (cron :00/:03 already refreshes the CSVs). Do NOT re-fetch here —
# same csv_dir(), so the A/B reads exactly the same bars the live book did this hour.
AB_DIR="$REPO/backtest/data/ibkrcrypto_ab_gate"
mkdir -p "$AB_DIR"

AB_GATE_TREND=200 \
IBKRCRYPTO_DATADIR="$AB_DIR" \
ENGINE_TAG="IBKRCrypto-Intraday-GATE200" \
"$REPO/build/shadow_refresh_intraday"
