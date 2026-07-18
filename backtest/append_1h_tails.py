#!/usr/bin/env python3
# append_1h_tails.py — append fresh Binance 1h klines to backtest/data/<COIN>USDT_1h.csv.
# APPEND-ONLY tail extension (the fetch_coin_1h.py clobber trap: that script rewrites the
# whole file and destroys pre-2023 history — S-2026-07-18ab/ag both had to restore from git).
# Method = the 18ab/ag "direct-klines tail append" made repeatable for the 33-coin rollout.
# Sanity per coin: monotonic +3600000ms steps from the existing last row (gaps reported,
# not silently accepted), first appended close within +/-25% of the prior close.
import csv, json, sys, time, urllib.request

DATA = "/Users/jo/Crypto/backtest/data"
COINS = sys.argv[1:] or [
    "AAVE","ADA","APT","ATOM","AVAX","BCH","BNB","COMP","CRV","DOGE","DOT","ETC",
    "FIL","GRT","ICP","INJ","LDO","LINK","LTC","MANA","NEAR","OP","RUNE","SAND",
    "SOL","SUI","SUSHI","THETA","TIA","TRX","UNI","VET","XRP"]

def klines(sym, start_ms):
    url = ("https://api.binance.com/api/v3/klines?symbol=%sUSDT&interval=1h"
           "&startTime=%d&limit=1000" % (sym, start_ms))
    with urllib.request.urlopen(url, timeout=30) as r:
        return json.load(r)

for c in COINS:
    path = "%s/%sUSDT_1h.csv" % (DATA, c)
    try:
        with open(path) as f:
            last = f.readlines()[-1].split(",")
        last_ts, last_close = int(last[0]), float(last[4])
    except Exception as e:
        print("%-6s SKIP no file/parse (%s)" % (c, e)); continue
    added, gaps, cur = 0, 0, last_ts + 3600000
    rows = []
    while True:
        try:
            kl = klines(c, cur)
        except Exception as e:
            print("%-6s FETCH-ERR %s" % (c, e)); kl = []
        if not kl:
            break
        for k in kl:
            ot = int(k[0])
            if ot <= last_ts:
                continue
            if rows or True:
                expect = (int(rows[-1][0]) if rows else last_ts) + 3600000
                if ot != expect:
                    gaps += 1
            rows.append([str(ot), k[1], k[2], k[3], k[4]])
        cur = int(kl[-1][0]) + 3600000
        if len(kl) < 1000:
            break
        time.sleep(0.15)
    if rows:
        # drop the still-forming current hour (close not final)
        now_ms = int(time.time() * 1000)
        rows = [r for r in rows if int(r[0]) + 3600000 <= now_ms]
    if rows:
        c0 = float(rows[0][4])
        if last_close > 0 and abs(c0 / last_close - 1) > 0.25:
            print("%-6s ABORT continuity: prior close %.6g vs first appended %.6g"
                  % (c, last_close, c0)); continue
        with open(path, "a") as f:
            for r in rows:
                f.write(",".join(r) + "\n")
        added = len(rows)
    print("%-6s +%d rows%s new_last=%s" % (c, added,
          (" (%d gap-steps)" % gaps) if gaps else "", rows[-1][0] if rows else last_ts))
