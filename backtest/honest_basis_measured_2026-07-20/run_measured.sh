#!/bin/bash
# S-2026-07-20 re-cert at MEASURED per-coin cost (operator order: honest ledger + correct cost).
# Basis: TRANSFORM (entry at le*(1+confirm) — certified live-fill model, validated byte-exact
# vs the S-20 RT=30 run after the engine's fill_px change via ClipRecord.anchor_le).
# Cost: DepthLiquidationModel safe_cost_bps at $1k (>= pilot $100, conservative):
#   RUNE 29.3bp; all other fleet coins 28.0bp (measured deep-coin class / fee-decomposition
#   default for unmeasured alts — same figure the live boot now computes). Stress = 2x measured.
cd /Users/jo/Crypto/backtest
COINS="AAVE ADA APT ATOM AVAX BCH BNB BTC COMP CRV DOGE DOT ETC ETH FIL GRT ICP INJ LDO LINK LTC MANA NEAR OP RUNE SAND SOL SUI SUSHI THETA TIA TRX UNI VET XRP"
for c in $COINS; do for thr in 0.005 0.010 0.015 0.020; do
  rt=28.0; [ "$c" = "RUNE" ] && rt=29.3
  echo "$c $thr $rt"
done; done | xargs -P 8 -L1 bash -c 'UM_COIN=$0 UM_THR=$1 UM_RT=$2 UM_EARLY=1 UM_NATIVE=0 UM_W=1,2,4,8,12 ./honest_entry_basis_bt > honest_basis_measured_2026-07-20/${0}_thr${1}_e1.txt 2>&1'
echo DONE
