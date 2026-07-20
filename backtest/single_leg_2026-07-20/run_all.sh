#!/bin/bash
# S-2026-07-20q single-leg mimic cert (handoff 20p order item 1)
cd /Users/jo/Crypto/backtest
COINS="AAVE ADA APT ATOM AVAX BCH BNB BTC COMP CRV DOGE DOT ETC ETH FIL GRT ICP INJ LDO LINK LTC MANA NEAR OP RUNE SAND SOL SUI SUSHI THETA TIA TRX UNI VET XRP"
for c in $COINS; do for thr in 0.005 0.010 0.015 0.020; do
  echo "$c $thr"
done; done | xargs -P 8 -L1 bash -c 'UM_SINGLE=1 UM_G=0.2,0.5,0.75,0.9 UM_COIN=$0 UM_THR=$1 ./cascade_increment_bt > single_leg_2026-07-20/${0}_thr${1}.txt 2>&1'
echo DONE
