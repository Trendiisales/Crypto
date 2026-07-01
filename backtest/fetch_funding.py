#!/usr/bin/env python3
"""
Fetch Binance USDT-perp funding-rate history -> data/funding/<SYM>.csv, used by ibkrcrypto_bt
(FUNDCSV env + USE_REAL_FUND=1) as a REAL-financing PROXY for the CME SQF daily TFA basis.

WHY a proxy: the book trades CME Spot-Quoted Futures, whose daily financing = spot<->lead-future
basis. We are NOT entitled to CME SQF basis history yet (README). Binance perp funding is the
closest free, real, same-underlying carry signal (BTC/ETH/SOL), so it replaces the flat 10%/yr
placeholder with an actual time-varying carry. Label it a PROXY in any live-cost claim.

Binance pays funding 3x/day (8h); the binary sums a day's funding = that day's rate (SQF is 1x/day).
CSV schema the overlay parser wants: symbol,funding_time_ms,funding_rate,mark
"""
import urllib.request, json, time, os, sys
HERE=os.path.dirname(os.path.abspath(__file__))
OUT=os.path.join(HERE,"data","funding"); os.makedirs(OUT,exist_ok=True)
SYMS=["BTC","ETH","SOL"]   # NDX is equity SQF -> different carry, keep flat ANNUAL_CARRY (not here)
START=1546300800000        # 2019-01-01 (perp funding history begins ~here)

def fetch(sym):
    out=[]; cur=START; now=int(time.time()*1000)
    while cur<now:
        url=(f"https://fapi.binance.com/fapi/v1/fundingRate?symbol={sym}USDT"
             f"&startTime={cur}&limit=1000")
        try: rows=json.load(urllib.request.urlopen(url,timeout=20))
        except Exception as e: print(f"{sym}: {e}",file=sys.stderr); break
        if not rows: break
        out+=rows
        last=rows[-1]["fundingTime"]
        if last<=cur or len(rows)<1000: cur=last+1
        else: cur=last+1
        if len(rows)<1000: break
        time.sleep(0.15)
    return out

def main():
    for s in SYMS:
        rows=fetch(s)
        if not rows: print(f"{s}: no funding",file=sys.stderr); continue
        path=os.path.join(OUT,f"{s}.csv")
        with open(path,"w") as fh:
            fh.write("symbol,funding_time_ms,funding_rate,mark\n")
            for r in rows:
                mark=r.get("markPrice","0") or "0"
                fh.write(f'{s},{r["fundingTime"]},{r["fundingRate"]},{mark}\n')
        print(f"{s}: {len(rows)} funding points -> {path}",flush=True)

if __name__=="__main__": main()
