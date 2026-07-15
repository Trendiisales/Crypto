#!/usr/bin/env python3
# Generic Binance PUBLIC-REST 1h fetcher (data-only, NOT exec gateway).
# Pages FORWARD from a fixed early startTime (fixes the start=0 "latest-1000" bug).
# Usage: fetch_coin_1h.py <SYM>   e.g. fetch_coin_1h.py APT  -> data/APTUSDT_1h.csv
import urllib.request, json, time, sys, os

SYM = sys.argv[1].upper()
INTERVAL = "1h"; LIMIT = 1000
OUT = os.path.join(os.path.dirname(os.path.abspath(__file__)), "data", f"{SYM}USDT_1h.csv")
BASE = "https://api.binance.com/api/v3/klines"
start = 1672531200000  # 2023-01-01 (harness omits <2023 anyway; later-listed coins return from listing)

rows = []; last_open = None; empties = 0
while True:
    url = f"{BASE}?symbol={SYM}USDT&interval={INTERVAL}&limit={LIMIT}&startTime={start}"
    try:
        with urllib.request.urlopen(url, timeout=30) as r:
            data = json.load(r)
    except Exception as e:
        print(f"ERR {e}", file=sys.stderr); time.sleep(2); empties += 1
        if empties > 5: print(f"ABORT {SYM}: too many errors", file=sys.stderr); sys.exit(2)
        continue
    if not data: break
    for k in data:
        ot = int(k[0])
        if last_open is not None and ot <= last_open: continue
        rows.append((ot, k[1], k[2], k[3], k[4])); last_open = ot
    if len(data) < LIMIT: break
    start = last_open + 1
    time.sleep(0.25)

if not rows:
    print(f"NO DATA for {SYM}USDT", file=sys.stderr); sys.exit(3)
os.makedirs(os.path.dirname(OUT), exist_ok=True)
with open(OUT, "w") as f:
    f.write("open_time_ms,open,high,low,close\n")
    for ot, o, h, l, c in rows:
        f.write(f"{ot},{o},{h},{l},{c}\n")
print(f"WROTE {len(rows)} rows -> {OUT}  range "
      f"{time.strftime('%Y-%m-%d', time.gmtime(rows[0][0]/1000))}..{time.strftime('%Y-%m-%d', time.gmtime(rows[-1][0]/1000))}")
