#!/usr/bin/env python3
# staleness_alarm.py — SURFACE crypto staleness loudly (backstop to the in-GUI guard).
# History: detection (refresh_shadow) froze the daily book silently for 2.6d because NOTHING
# watched the flag. This closes the loop with desktop notification + a RED marker file the GUI
# reads (data/<book>/ALARM_STALE.txt) + a log line. Idempotent: clears the marker when fresh.
#
# S-2026-06-30 FIXES (the alarm itself was half-blind):
#   1) `updated` parse expected ISO "…T…Z" but the field is "YYYY-MM-DD HH:MM UTC" (space) ->
#      strptime threw, the except swallowed it, so the producer-dead check NEVER fired. Fixed.
#   2) added live_mark_ts heartbeat check (live_mark cron every 5min) — the REAL liveness signal;
#      frozen marks => live P&L is a lie even if `updated` looks recent.
#   3) extended to BOTH books (daily + intraday) — intraday was unwatched entirely.
# Run every ~30min via cron/launchd.
import json, os, subprocess, time

HERE = os.path.dirname(os.path.abspath(__file__))
LOG  = "/tmp/crypto_staleness_alarm.log"
now_iso = time.strftime("%Y-%m-%dT%H:%M:%SZ", time.gmtime())

# cadence-aware caps (seconds). live_mark every 5min; daily signal eval ~daily; intraday hourly.
MARK_MAX = 900            # 15min -> marks frozen
BOOKS = {
    "daily":    {"dir": os.path.join(HERE, "data", "ibkrcrypto"),          "sig_max": 30*3600},
    "intraday": {"dir": os.path.join(HERE, "data", "ibkrcrypto_intraday"), "sig_max": 100*60},
}

def log(msg):
    try:
        with open(LOG, "a") as f: f.write(f"{now_iso} {msg}\n")
    except Exception: pass

def parse_utc(s):
    if not s: return None
    s = str(s).replace(" UTC", "").replace("Z", "").replace("T", " ").strip()
    for fmt in ("%Y-%m-%d %H:%M:%S", "%Y-%m-%d %H:%M"):
        try: return time.mktime(time.strptime(s, fmt)) - time.timezone
        except Exception: pass
    return None

def check(name, cfg):
    ddir, sig_max = cfg["dir"], cfg["sig_max"]
    state  = os.path.join(ddir, "state.json")
    flag   = os.path.join(ddir, "stale_data.flag")
    marker = os.path.join(ddir, "ALARM_STALE.txt")
    reasons, now = [], time.time()
    # Producer intentionally OFF (operator reset): a frozen book is EXPECTED, not stale.
    # Marker file presence => skip the staleness check + clear any prior alarm marker.
    # Re-arm by deleting PRODUCER_DISABLED when the producer is turned back on.
    if os.path.exists(os.path.join(ddir, "PRODUCER_DISABLED")):
        if os.path.exists(marker):
            try: os.remove(marker)
            except Exception: pass
        log(f"SKIP [{name}] producer disabled (intentional)"); print(f"SKIP [{name}] producer disabled")
        return False
    if os.path.exists(flag):
        try: reasons.append("flag:" + open(flag).read().strip())
        except Exception: reasons.append("flag present")
    if not os.path.exists(state):
        reasons.append("state.json MISSING")
    else:
        try:
            d = json.load(open(state))
            dh = d.get("data_health", {})
            if dh.get("all_fresh") is False:
                reasons.append("all_fresh=false stale=" + ",".join(dh.get("stale_sources", [])))
            mt = os.path.getmtime(state)
            if now - mt > 1200:
                reasons.append(f"producer DEAD: file untouched {int((now-mt)/60)}min")
            su = parse_utc(d.get("updated"))
            mk = parse_utc(d.get("live_mark_ts"))
            # heartbeat = freshest of (updated, live_mark_ts); EITHER fresh => producer alive
            hb = [x for x in (mk, su) if x is not None]
            if not hb:
                reasons.append("no timestamps in state")
            elif now - max(hb) > MARK_MAX:
                reasons.append(f"MARKS frozen {int((now-max(hb))/60)}min (live P&L not real)")
            # signals cadence (refresh_shadow dead): updated older than its cadence cap
            if su is not None and now - su > sig_max:
                reasons.append(f"SIGNALS stale {int((now-su)/3600)}h (>{int(sig_max/3600)}h)")
        except Exception as e:
            reasons.append(f"state.json unreadable: {e}")
    if reasons:
        summary = f"[{name}] " + " | ".join(reasons)
        try:
            with open(marker, "w") as f: f.write(f"STALE-DATA ALARM {now_iso}\n{summary}\n")
        except Exception: pass
        try:
            subprocess.run(["osascript", "-e",
                f'display notification "{summary[:200]}" with title "\U0001F534 CRYPTO (Binance) STALE ({name})" sound name "Basso"'],
                timeout=10)
        except Exception: pass
        log("ALARM: " + summary); print("ALARM: " + summary)
        return True
    else:
        if os.path.exists(marker):
            try: os.remove(marker)
            except Exception: pass
        log(f"OK [{name}] fresh"); print(f"OK [{name}] fresh")
        return False

any_stale = False
for name, cfg in BOOKS.items():
    any_stale = check(name, cfg) or any_stale
raise SystemExit(1 if any_stale else 0)
