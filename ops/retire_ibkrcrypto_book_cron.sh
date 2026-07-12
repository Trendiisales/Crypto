#!/usr/bin/env bash
# ============================================================================
# retire_ibkrcrypto_book_cron.sh  (S-2026-07-12)  — IDEMPOTENT
#
# Crypto CONSOLIDATION onto ONE system (josgp1/Chimera). The Mac ibkrcrypto
# shadow book (daily + intraday) is a SECOND disparate trading book; its viable
# engines were folded onto Chimera (ADA-KELT-D1/ADA-REGIME-D1/AAVE-KELT-D1,
# commit 26afc35); its majors/upjump/NDX legs are redundant with Chimera's
# EDGE-SLOTS + UPJUMP-GRID. This retires the Mac book's crons so only the ONE
# Chimera system trades. KEEPS: refresh_crypto_companion.sh (the chimera->desk
# relay) and the companions — out of scope here.
#
# Comments (prefixes) the 4 active ibkrcrypto-book cron lines, never deletes;
# backs up crontab first; re-runnable (skips already-retired lines). Never
# inline-sed the crontab (that wiped it once — feedback-crontab-edit-via-script).
# ============================================================================
set -euo pipefail
TAG="RETIRED-MAC-FOLD S-2026-07-12"
BAK="/tmp/crontab.bak.$(date +%s)"
crontab -l > "$BAK" 2>/dev/null || true
echo "crontab backed up -> $BAK"

python3 - "$TAG" <<'PY'
import subprocess, sys
tag = sys.argv[1]
cur = subprocess.run(["crontab","-l"], capture_output=True, text=True).stdout.splitlines()
# match ACTIVE (non-comment) lines that RUN the Mac ibkrcrypto book binaries
needles = ("/Users/jo/Crypto/build/shadow_refresh", "/Users/jo/Crypto/build/live_mark")
out, n = [], 0
for ln in cur:
    s = ln.lstrip()
    if s.startswith("#"):
        out.append(ln); continue
    if any(nd in ln for nd in needles):
        out.append(f"# [{tag}] {ln}"); n += 1
    else:
        out.append(ln)
if n == 0:
    print("no active ibkrcrypto-book crons found (already retired?) — no change")
else:
    subprocess.run(["crontab","-"], input="\n".join(out)+"\n", text=True, check=True)
    print(f"retired {n} ibkrcrypto-book cron line(s)")
PY
echo "=== remaining ACTIVE crypto crons (chimera relay + companions stay) ==="
crontab -l 2>/dev/null | grep -vE '^\s*#' | grep -iE 'crypto|companion|wave|chimera' | sed 's/ >>.*//' | head
