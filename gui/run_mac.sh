#!/usr/bin/env bash
# =============================================================================
# run_mac.sh — run the crypto dashboard ON THE MAC (offload from the 3GB VPS).
#
# WHY: the crypto display data is ALREADY Mac-native — refresh_shadow.py +
# live_mark.py compute state.json on the Mac, then cron scp'd it TO the VPS just
# so the VPS could display it. Backwards. Serve it where it's born: the Mac has
# the RAM + is always on. Frees a VPS python process AND lets us drop the two
# scp-to-VPS pushes from the Mac crons. VPS keeps ONLY the live trading path.
#
# Idempotent keepalive: relaunch only if :8090 isn't already served by us.
# Run by cron every 2min (see crontab). Reads local state (no VPS round-trip
# except the lightweight NQ-price ssh tail server.py already multiplexes).
# =============================================================================
set -uo pipefail

# Cron runs with a minimal PATH (/usr/bin:/bin) that OMITS /usr/sbin, where lsof
# lives. Without this, the `lsof` guard below is command-not-found in cron → the
# idempotent check always fails → a HEALTHY GUI gets pkill'd + restarted every
# 2min, creating a ~1-2s dark window that liveness_check flags as DARK COMPONENT.
# (2026-06-29 fix: this was the real "crypto-GUI flaps every 2min" root cause.)
export PATH="/usr/sbin:/sbin:/usr/bin:/bin:$PATH"

export PORT=8090
export IBKRCRYPTO_BIND=127.0.0.1   # Mac-local only (was 0.0.0.0 for public VPS)
export IBKRCRYPTO_STATE="$HOME/Crypto/backtest/data/ibkrcrypto/state.json"
export COMPANION_STATE="$HOME/stall-accountant/companion_state.json"
# intraday (1h/4h) book — second section on the same dashboard
export IBKRCRYPTO_STATE_INTRADAY="$HOME/Crypto/backtest/data/ibkrcrypto_intraday/state.json"
export COMPANION_STATE_INTRADAY="$HOME/Crypto/companion_intraday/companion_state.json"
# LUKE book: gui_server's default resolves STATE_LUKE relative to GUIDIR (~/Crypto, wrong
# tree) → would 404. Pin to the live luke producer's output (cutover S-2026-06-30).
export IBKRCRYPTO_STATE_LUKE="$HOME/Crypto/backtest/data/ibkrcrypto_luke/state.json"

HERE="$HOME/Crypto/gui"
LOG="/tmp/crypto_gui_mac.log"
PY=/usr/bin/python3
BIN=/Users/jo/Crypto/build/gui_server   # C++ gui_server (cutover S-2026-06-30; rollback: PY server.py)

PAT="Crypto/build/gui_server"   # match the C++ binary (was IBKRCrypto/gui/server.py)

# already alive? (our server.py on :8090) -> nothing to do
if pgrep -f "$PAT" >/dev/null 2>&1 && \
   lsof -nP -iTCP:8090 -sTCP:LISTEN >/dev/null 2>&1; then
  exit 0
fi

# stale/dead -> clean any listener on :8090 (incl. old 0.0.0.0 instances), relaunch detached
pkill -f "$PAT" 2>/dev/null || true
lsof -nP -tiTCP:8090 -sTCP:LISTEN 2>/dev/null | xargs -r kill 2>/dev/null || true
sleep 1
cd "$HERE"
# ROLLBACK (Python, instant): nohup "$PY" "$HERE/server.py" >>"$LOG" 2>&1 &
nohup "$BIN" >>"$LOG" 2>&1 &
echo "[$(date '+%F %T')] crypto GUI (re)started [C++ gui_server] on 127.0.0.1:8090 (state=$IBKRCRYPTO_STATE)" >>"$LOG"
