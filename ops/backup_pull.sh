#!/bin/bash
# Mac daily: pull VPS staged L2 archives -> Google Drive 'L2 backup' -> purge VPS originals.
GD="/Users/jo/Library/CloudStorage/GoogleDrive-kiwi18@gmail.com/My Drive/L2 backup"
VPS=omega-vps
LOG=/Users/jo/Crypto/ops/backup_pull.log
ts(){ date -u '+%F %T'; }
mkdir -p "$GD" 2>/dev/null
files=$(ssh -o ConnectTimeout=15 "$VPS" 'powershell -NoProfile -Command "Get-ChildItem C:\Omega\to_gdrive\*.tgz -ErrorAction SilentlyContinue | ForEach-Object { $_.Name }"' 2>/dev/null | tr -d '\r')
if [ -z "$files" ]; then echo "$(ts) no archives staged" >> "$LOG"; exit 0; fi
if scp -o ConnectTimeout=15 "$VPS":'C:/Omega/to_gdrive/*.tgz' "$GD/" 2>>"$LOG"; then
  # verify something landed, then purge VPS originals + staged tgz
  if ls "$GD"/*.tgz >/dev/null 2>&1; then
    ssh "$VPS" 'powershell -NoProfile -ExecutionPolicy Bypass -File C:\Omega\purge_archived.ps1' 2>>"$LOG"
    echo "$(ts) pulled+purged: $files" >> "$LOG"
  fi
else echo "$(ts) scp FAILED -- VPS originals kept" >> "$LOG"; fi
