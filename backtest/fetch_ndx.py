#!/usr/bin/env python3
"""fetch_ndx.py — refresh /Users/jo/Tick/NDX_daily_2016_2026.csv from yfinance ^NDX.

Merges fresh daily bars into the existing history (full history preserved), VALIDATES the
newest bar is recent BEFORE overwriting (atomic temp+rename), else keeps the old file and
exits non-zero so the caller/monitor alarms. Closes the root cause of the 2026-06-25 incident:
the NDX producer died 2026-06-15 and nothing refreshed the file or noticed it had gone stale.

Run:  python3 fetch_ndx.py            (exit 0 = wrote fresh; exit 1 = kept old, source stale/failed)
Wire: call before refresh_shadow.py, or on its own cron, paired with the data-health monitor.
"""
import sys, os, time, tempfile, datetime as dt

NDX = "/Users/jo/Tick/NDX_daily_2016_2026.csv"
SESSION_OFFSET = 48600      # 13:30 UTC — matches the existing bars' stamping (NYSE open)
MAX_AGE_DAYS = 4.0          # weekday index: Fri close -> Tue (incl. a Mon holiday)

def die(msg, code=1):
    print(f"[fetch_ndx] {msg}", flush=True)
    sys.exit(code)

try:
    import yfinance as yf
except Exception as e:
    die(f"yfinance import failed: {e}")

try:
    df = yf.download("^NDX", period="60d", interval="1d", auto_adjust=False, progress=False)
except Exception as e:
    die(f"yfinance download failed ({e}) — keeping existing file")
if df is None or df.empty:
    die("yfinance returned no rows — keeping existing file")

def col(name):
    for c in df.columns:
        cc = c[0] if isinstance(c, tuple) else c
        if cc == name:
            return df[c]
    return None
o, h, l, c = col("Open"), col("High"), col("Low"), col("Close")
if any(x is None for x in (o, h, l, c)):
    die(f"unexpected yfinance columns: {list(df.columns)}")

fresh = {}
for i, idx in enumerate(df.index):
    d = idx.date()
    ts = int(dt.datetime(d.year, d.month, d.day, tzinfo=dt.timezone.utc).timestamp()) + SESSION_OFFSET
    try:
        ov, hv, lv, cv = float(o.iloc[i]), float(h.iloc[i]), float(l.iloc[i]), float(c.iloc[i])
    except Exception:
        continue
    if cv > 0 and hv >= lv:                       # correctness: positive close, sane OHLC
        fresh[ts] = (ov, hv, lv, cv)
if not fresh:
    die("no valid fresh bars parsed — keeping existing file")

newest_ts = max(fresh)
age = (time.time() - newest_ts) / 86400.0
if age > MAX_AGE_DAYS:                             # FRESHNESS GATE: refuse to write a stale source
    nd = dt.datetime.fromtimestamp(newest_ts, dt.timezone.utc).date()
    die(f"yfinance newest ^NDX bar is {nd} ({age:.1f}d old > {MAX_AGE_DAYS}d) — source stale, NOT overwriting")

# merge fresh bars into the existing history
rows = {}
if os.path.exists(NDX):
    with open(NDX) as fh:
        for ln in fh:
            p = ln.strip().split(",")
            if len(p) >= 5 and p[0].lstrip("-").isdigit():
                rows[int(p[0])] = tuple(p[1:5])
for ts, (ov, hv, lv, cv) in fresh.items():
    rows[ts] = (f"{ov:.2f}", f"{hv:.2f}", f"{lv:.2f}", f"{cv:.2f}")

# atomic write (temp + rename) so a crash mid-write can't corrupt the live file
ordered = sorted(rows)
fd = tempfile.NamedTemporaryFile("w", delete=False, dir=os.path.dirname(NDX), newline="")
try:
    for ts in ordered:
        r = rows[ts]
        fd.write(f"{ts},{r[0]},{r[1]},{r[2]},{r[3]}\n")
    fd.close()
    os.replace(fd.name, NDX)
except Exception as e:
    try: os.unlink(fd.name)
    except Exception: pass
    die(f"write failed ({e}) — original file untouched")

nd = dt.datetime.fromtimestamp(newest_ts, dt.timezone.utc).date()
print(f"[fetch_ndx] OK — {len(ordered)} bars, newest {nd} (age {age:.1f}d), {len(fresh)} fresh merged", flush=True)
