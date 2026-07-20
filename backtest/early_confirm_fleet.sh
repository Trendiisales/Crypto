#!/bin/bash
# EARLY-CONFIRM fleet cert: all live-roster coins, UJ05+UJ10 lanes, EARLY 0 vs 1.
cd /Users/jo/Crypto/backtest
COINS="BTC ETH AAVE ADA APT ATOM AVAX BCH BNB COMP CRV DOGE DOT ETC FIL GRT ICP INJ LDO LINK LTC MANA NEAR OP RUNE SAND SOL SUI SUSHI THETA TIA TRX UNI VET XRP"
OUT=early_confirm_2026-07-20; mkdir -p $OUT
for C in $COINS; do
  for THR in 0.005 0.010; do
    for E in 0 1; do
      UM_COIN=$C UM_THR=$THR UM_EARLY=$E UM_W=1,2,4,12 UM_G=0.2,0.5,0.75 UM_LEGS=8 \
        ./early_confirm_bt > $OUT/${C}_thr${THR}_e${E}.txt 2>&1
    done
  done
  echo "done $C"
done
# summarize: any cell that PASSes at e0 but fails at e1 = regression
echo "=== SUMMARY: e0-PASS -> e1-fail regressions ==="
for C in $COINS; do
  for THR in 0.005 0.010; do
    paste <(grep -E "^[0-9]" $OUT/${C}_thr${THR}_e0.txt) <(grep -E "^[0-9]" $OUT/${C}_thr${THR}_e1.txt) | \
    awk -v c=$C -v t=$THR '{ if ($NF!=$(NF/2+0)) {} } /PASS.*fail/ {print c, t, $1, $2, "e0=PASS e1=FAIL"}'
  done
done
echo "=== fleet PASS counts ==="
for E in 0 1; do
  n=$(cat $OUT/*_e${E}.txt | grep -cE "PASS$"); f=$(cat $OUT/*_e${E}.txt | grep -cE "fail$")
  echo "early=$E PASS=$n fail=$f"
done
