#!/bin/bash
cd /Users/jo/Crypto/backtest
COINS="AAVE ADA APT ATOM AVAX BCH BNB BTC COMP CRV DOGE DOT ETC ETH FIL GRT ICP INJ LDO LINK LTC MANA NEAR OP RUNE SAND SOL SUI SUSHI THETA TIA TRX UNI VET XRP"
for c in $COINS; do for thr in 0.005 0.010 0.015 0.020; do
  echo "$c $thr"
done; done | xargs -P 8 -L1 bash -c 'UM_COIN=$0 UM_THR=$1 ./cascade_increment_bt > cascade_inc_2026-07-20/${0}_thr${1}.txt 2>&1'
echo DONE
