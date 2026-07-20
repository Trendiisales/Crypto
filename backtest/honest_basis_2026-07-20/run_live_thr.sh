#!/bin/bash
# supplementary: live-spec thr 1.5/2.0% x W{1,2,4,8,12}, e1 live-faithful
cd /Users/jo/Crypto/backtest
COINS="AAVE ADA APT ATOM AVAX BCH BNB BTC COMP CRV DOGE DOT ETC ETH FIL GRT ICP INJ LDO LINK LTC MANA NEAR OP RUNE SAND SOL SUI SUSHI THETA TIA TRX UNI VET XRP"
for c in $COINS; do for thr in 0.015 0.020; do
  echo "$c $thr"
done; done | xargs -P 8 -L1 bash -c 'UM_COIN=$0 UM_THR=$1 UM_EARLY=1 UM_W=1,2,4,8,12 ./honest_entry_basis_bt > honest_basis_2026-07-20/${0}_thr${1}_e1.txt 2>&1'
echo DONE
