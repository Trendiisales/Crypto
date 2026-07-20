#!/bin/bash
# S-2026-07-20 honest entry-basis re-cert runner — full S-20j matrix, honest basis
cd /Users/jo/Crypto/backtest
COINS="AAVE ADA APT ATOM AVAX BCH BNB BTC COMP CRV DOGE DOT ETC ETH FIL GRT ICP INJ LDO LINK LTC MANA NEAR OP RUNE SAND SOL SUI SUSHI THETA TIA TRX UNI VET XRP"
for c in $COINS; do for thr in 0.005 0.010; do for e in 0 1; do
  echo "$c $thr $e"
done; done; done | xargs -P 8 -L1 bash -c 'UM_COIN=$0 UM_THR=$1 UM_EARLY=$2 ./honest_entry_basis_bt > honest_basis_2026-07-20/${0}_thr${1}_e${2}.txt 2>&1'
echo DONE
