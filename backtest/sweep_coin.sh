#!/usr/bin/env bash
# Faithful mimic-floor up-jump gate sweep for ONE coin (VALIDATED method, ONDO parity).
# Usage: sweep_coin.sh <COIN>   e.g. sweep_coin.sh APT
# Fetches 1h data if missing, runs the full grid, prints ONLY PASS rows (config-tagged)
# + a summary. Full per-run log saved to /tmp/sweep_<COIN>.log.
set -uo pipefail
cd "$(dirname "$0")" || exit 1
COIN="$1"
CSV="data/${COIN}USDT_1h.csv"
LOG="/tmp/sweep_${COIN}.log"
: > "$LOG"

if [ ! -s "$CSV" ]; then
  echo "[fetch] $COIN ..." >&2
  python3 fetch_coin_1h.py "$COIN" >&2 || { echo "FETCH-FAIL $COIN"; exit 2; }
fi
# integrity: need >=1500 bars for both-WF to mean anything
NBARS=$(($(wc -l < "$CSV") - 1))
echo "$COIN bars=$NBARS csv=$CSV" >&2

# Fixed grid (handoff-specified). Reclip 0.05 REQUIRED (else strawman). RT=20 cut=50 confirm=20.
PASS_ROWS=""
NPASS=0
for W in 1 2 4 8 24; do
  for THR in 0.035 0.04 0.05 0.055 0.07; do
    for TG in 0 24 48; do
      OUT=$(CC_COIN="$COIN" CC_W="$W" CC_THR="$THR" CC_RT=20 CC_CUT=50 CC_CONFIRM=20 \
            CC_RECLIP=0.05 CC_TRENDGATE="$TG" \
            ./upjump_earlyarm_bt mimicg 2>/dev/null | grep -v '\[CLIP\]')
      echo "=== W=$W THR=$THR TG=$TG ===" >> "$LOG"
      echo "$OUT" >> "$LOG"
      # collect any PASS g-rows, tag with config
      while IFS= read -r line; do
        case "$line" in
          *PASS*)
            NPASS=$((NPASS+1))
            PASS_ROWS+="W=$W thr=$THR tg=$TG | $line"$'\n'
            ;;
        esac
      done <<< "$OUT"
    done
  done
done

echo "########## $COIN VERDICT ##########"
if [ "$NPASS" -eq 0 ]; then
  echo "$COIN: NO-GO — 0 PASS across full grid (bars=$NBARS). Full log: $LOG"
else
  echo "$COIN: $NPASS PASS cell(s) (bars=$NBARS). Re-check at MEASURED cost before wiring."
  printf '%s' "$PASS_ROWS"
fi
