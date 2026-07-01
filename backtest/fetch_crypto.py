#!/usr/bin/env python3
# fetch_crypto.py -- daily-bar producer for the IBKRCrypto shadow book.
# WHY: the cron only fetched NDX (fetch_ndx.py); the crypto daily CSVs had NO
# updater, so BTCUSDT/ETHUSDT/SOLUSDT_1d.csv went stale (2.6d) and refresh_shadow's
# freshness guard FROZE the whole book. This appends the missing COMPLETED daily
# Binance klines so the book never silently freezes again.
# Appends only bars newer than the CSV's last AND already closed (close_time < now).
import urllib.request, json, time, os, sys

HERE = os.path.dirname(os.path.abspath(__file__))
DATA = os.path.join(HERE, "data")
PAIRS = [("BTCUSDT", "BTCUSDT_1d.csv"),
         ("ETHUSDT", "ETHUSDT_1d.csv"),
         ("SOLUSDT", "SOLUSDT_1d.csv")]

def fetch(sym):
    url = f"https://api.binance.com/api/v3/klines?symbol={sym}&interval=1d&limit=10"
    return json.load(urllib.request.urlopen(url, timeout=15))

def main():
    now_ms = int(time.time() * 1000)
    rc = 0
    for sym, fname in PAIRS:
        path = os.path.join(DATA, fname)
        try:
            rows = open(path).read().strip().splitlines()
            last_ot = int(rows[-1].split(",")[0])
            added = []
            for k in fetch(sym):
                ot, ct = int(k[0]), int(k[6])
                if ot > last_ot and ct < now_ms:          # newer AND completed only
                    added.append(f"{ot},{float(k[1]):.8f},{float(k[2]):.8f},{float(k[3]):.8f},{float(k[4]):.8f}")
            if added:
                with open(path, "a") as fh:
                    for ln in added:
                        fh.write(ln + "\n")
            d = lambda ms: time.strftime("%Y-%m-%d", time.gmtime(ms / 1000))
            newest = int(open(path).read().strip().splitlines()[-1].split(",")[0])
            print(f"{sym}: +{len(added)} bar(s), newest {d(newest)}", flush=True)
        except Exception as e:
            print(f"{sym}: FETCH FAILED -- {e}", file=sys.stderr, flush=True)
            rc = 1
    return rc

if __name__ == "__main__":
    sys.exit(main())
