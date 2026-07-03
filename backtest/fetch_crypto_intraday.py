#!/usr/bin/env python3
# fetch_crypto_intraday.py - paginated Binance klines (1h, 4h) for the intraday stack.
# Companion to fetch_crypto.py (which only pulls 1d). Writes data/<SYM>_<tf>.csv in the
# exact format ibkrcrypto_bt.cpp expects: open_time_ms,open,high,low,close
import json, os, sys, time, urllib.request, datetime

HERE = os.path.dirname(os.path.abspath(__file__))
DATA = os.path.join(HERE, "data")
PAIRS = ["BTCUSDT", "ETHUSDT", "SOLUSDT"]
TFS   = ["1h", "4h"]
# UpJump companion parents (S-2026-07-03, slice 2): 1h ONLY (24×1h up-jump window).
# BTC/ETH/SOL 1h already pulled above -> not repeated here.
UP1H  = ["DOGEUSDT", "ADAUSDT", "TRXUSDT", "AAVEUSDT", "NEARUSDT", "BNBUSDT", "OPUSDT"]
START_MS = int(datetime.datetime(2021, 1, 1, tzinfo=datetime.timezone.utc).timestamp() * 1000)
LIMIT = 1000  # Binance max rows/call


def fetch_klines(sym, interval, start_ms):
    rows = []
    cur = start_ms
    while True:
        url = (f"https://api.binance.com/api/v3/klines?symbol={sym}"
               f"&interval={interval}&startTime={cur}&limit={LIMIT}")
        with urllib.request.urlopen(url, timeout=30) as r:
            batch = json.load(r)
        if not batch:
            break
        rows.extend(batch)
        last_open = batch[-1][0]
        if len(batch) < LIMIT:
            break
        cur = last_open + 1
        time.sleep(0.25)  # be polite to the weight limiter
    return rows


def last_open_ms(path):
    """Newest open_time_ms already in the CSV, or None. Lets cron fetch INCREMENTALLY
    (fetch from last bar fwd) instead of re-pulling 2021->now every hour."""
    try:
        rows = open(path).read().strip().splitlines()
        for ln in reversed(rows):
            c = ln.split(",")[0].strip()
            if c.isdigit():
                return int(c)
    except Exception:
        return None
    return None


def main():
    os.makedirs(DATA, exist_ok=True)
    for sym, tflist in [(s, TFS) for s in PAIRS] + [(s, ["1h"]) for s in UP1H]:
        for tf in tflist:
            path = os.path.join(DATA, f"{sym}_{tf}.csv")
            prev_last = last_open_ms(path)
            start = (prev_last + 1) if prev_last else START_MS   # incremental when CSV exists
            rows = fetch_klines(sym, tf, start)
            if not rows:
                # no new bars is normal mid-bar; only warn on a cold (missing-file) miss
                if prev_last is None:
                    print(f"{sym} {tf}: NO DATA", file=sys.stderr)
                continue
            if prev_last:                                        # APPEND new bars (header already present)
                with open(path, "a") as f:
                    for k in rows:
                        if k[0] <= prev_last:
                            continue                             # de-dupe the boundary bar
                        f.write(f"{k[0]},{k[1]},{k[2]},{k[3]},{k[4]}\n")
            else:                                                # cold pull -> fresh file with header
                with open(path, "w") as f:
                    f.write("open_time_ms,open,high,low,close\n")
                    for k in rows:
                        f.write(f"{k[0]},{k[1]},{k[2]},{k[3]},{k[4]}\n")
            last = datetime.datetime.utcfromtimestamp(rows[-1][0] / 1000)
            print(f"{sym} {tf}: +{len(rows)} bars  -> {last:%Y-%m-%d %H:%M}")


if __name__ == "__main__":
    main()
