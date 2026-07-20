#!/bin/bash
# S-2026-07-20q wide-g cascade legs 2-3 (handoff 20p order item 2b)
# Escalating-confirm cascade (OHLC own-fill drive), wide-g only, shallow legs.
cd /Users/jo/Crypto/backtest
COINS="AAVE ADA APT ATOM AVAX BCH BNB BTC COMP CRV DOGE DOT ETC ETH FIL GRT ICP INJ LDO LINK LTC MANA NEAR OP RUNE SAND SOL SUI SUSHI THETA TIA TRX UNI VET XRP"
for c in $COINS; do for thr in 0.005 0.010 0.015 0.020; do for legs in 2 3; do
  echo "$c $thr $legs"
done; done; done | xargs -P 8 -L1 bash -c 'UM_G=0.75,0.9 UM_LEGS=$2 UM_COIN=$0 UM_THR=$1 ./cascade_increment_bt > wideg_cascade_2026-07-20/${0}_thr${1}_L${2}.txt 2>&1'
echo DONE
