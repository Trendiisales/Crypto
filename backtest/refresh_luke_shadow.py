#!/usr/bin/env python3
"""
LukeCrypto DAILY shadow book (S-2026-06-30) — sibling of refresh_shadow.py.
Runs the validated [[LukeCryptoMomentum]] engine (engine_luke_crypto.py) forward on a
PERSISTENT daily universe and emits a shadow ledger. Isolated dir so the proven
trend/MR daily book (refresh_shadow.py) is untouched.

WHY a separate book: Luke is a MULTI-NAME portfolio momentum engine (setup-C/B breakout,
ride-wide exit, BTC>200MA gate MANDATORY) — structurally unlike the single-symbol legs.
SHADOW/paper only (operator standing rule: measure on the live ledger before sizing).

Flow each run: (1) incrementally refresh the daily universe CSVs (Binance, persistent so a
reboot can't wipe them like /tmp did), (2) run engine --live -> open positions + trades
closed on the last bar, (3) append new closes to luke_inbound.csv (dedup), write state.json.
"""
import urllib.request, json, time, os, sys, subprocess
HERE=os.path.dirname(os.path.abspath(__file__))
UNIV=os.path.join(HERE,"data","luke_universe")
STATE_DIR=os.path.join(HERE,"data","ibkrcrypto_luke")
os.makedirs(UNIV,exist_ok=True); os.makedirs(STATE_DIR,exist_ok=True)
ENGINE=os.path.join(HERE,"engine_luke_crypto.py")
# engine needs numpy/pandas; /usr/bin/python3 (cron default) lacks them -> pin sci interpreter
PY_SCI=next((p for p in ("/opt/homebrew/bin/python3",
    "/opt/homebrew/Caskroom/miniforge/base/envs/rdagent4qlib/bin/python") if os.path.exists(p)),sys.executable)
SYMS=["BTC","ETH","SOL","BNB","XRP","ADA","AVAX","DOGE","LINK","LTC","DOT","ATOM","NEAR","APT","ARB","OP","INJ"]
START=1483228800000  # 2017-01-01

def _klines(sym,start):
    out=[]; cur=start
    while True:
        url=(f"https://api.binance.com/api/v3/klines?symbol={sym}USDT"
             f"&interval=1d&startTime={cur}&limit=1000")
        try: rows=json.load(urllib.request.urlopen(url,timeout=20))
        except Exception as e: print(f"{sym}: fetch {e}",file=sys.stderr); break
        if not rows: break
        out+=rows
        if len(rows)<1000: break
        cur=rows[-1][0]+86400000; time.sleep(0.12)
    return out

def refresh_universe():
    now=int(time.time()*1000)
    for s in SYMS:
        path=os.path.join(UNIV,f"{s}.csv")
        if not os.path.exists(path):
            rows=_klines(s,START)
            if len(rows)<260: print(f"{s}: only {len(rows)}, skip",file=sys.stderr); continue
            with open(path,"w") as fh:
                fh.write("date,open,high,low,close,volume\n")
                for k in rows: _wr(fh,k)
            print(f"{s}: seed {len(rows)}",flush=True); continue
        last=open(path).read().strip().splitlines()[-1].split(",")[0]
        last_ms=int(time.mktime(time.strptime(last,"%Y-%m-%d")))*1000
        rows=_klines(s,last_ms+86400000)
        added=[k for k in rows if k[6]<now]  # closed bars only
        if added:
            with open(path,"a") as fh:
                for k in added: _wr(fh,k)
        print(f"{s}: +{len(added)}",flush=True)

def _wr(fh,k):
    d=time.strftime("%Y-%m-%d",time.gmtime(k[0]/1000))
    fh.write(f"{d},{float(k[1]):.6f},{float(k[2]):.6f},{float(k[3]):.6f},{float(k[4]):.6f},{float(k[5]):.2f}\n")

def main():
    refresh_universe()
    out=subprocess.run([PY_SCI,ENGINE,UNIV,"--live"],capture_output=True,text=True,timeout=120)
    if out.returncode!=0:
        print("engine err:",out.stderr[-500:],file=sys.stderr); return 1
    snap=json.loads(out.stdout)
    # append new closed trades (dedup by ticker+date_in+date_out)
    ledger=os.path.join(STATE_DIR,"luke_inbound.csv")
    seen=set()
    if os.path.exists(ledger):
        for ln in open(ledger).read().splitlines()[1:]:
            p=ln.split(",");
            if len(p)>=3: seen.add((p[1],p[2],p[3]))
    newrows=[]
    for t in snap["closed_today"]:
        key=(t["ticker"],t["date_in"],t["date_out"])
        if key in seen: continue
        side="LONG"  # Luke is long-only
        newrows.append(f'{int(time.time())}_{t["ticker"]},{t["ticker"]},{t["date_in"]},{t["date_out"]},'
                       f'{side},{t["setup"]},{t["why"]},{t["pnl"]:.2f},{t["R"]:.3f}')
    if newrows:
        head=not os.path.exists(ledger)
        with open(ledger,"a") as fh:
            if head: fh.write("id,ticker,date_in,date_out,side,setup,why,pnl_usd,R\n")
            for r in newrows: fh.write(r+"\n")
    # real SHADOW forward record = closes booked since go-live (luke_inbound.csv), NOT the
    # backtest curve. equity=snap["equity"] is the in-sample hypothetical NAV (replayed from
    # ~2021 each run); shadow_realized is the only money the live-feed shadow has actually made.
    shadow_realized=0.0; shadow_n=0
    if os.path.exists(ledger):
        for ln in open(ledger).read().splitlines()[1:]:
            p=ln.split(",")
            if len(p)>=8:
                try: shadow_realized+=float(p[7]); shadow_n+=1
                except ValueError: pass
    # KILL_FLAT marker (GUI kill button): hold the book flat. Luke positions are engine-derived
    # paper; while the marker exists the GUI shows zero open (durable override of the engine
    # signal). Operator deletes STATE_DIR/KILL_FLAT to resume showing the engine's live book.
    KILLED=os.path.exists(os.path.join(STATE_DIR,"KILL_FLAT"))
    # state.json (positions + book equity)
    state=dict(book="LukeCrypto",updated=int(time.time()),last=snap["last"],
               equity=snap["equity"],equity0=10000.0,
               shadow_realized=round(shadow_realized,2),shadow_n=shadow_n,live_since="2026-06-01",
               n_open=(0 if KILLED else snap["n_open"]),
               positions=([] if KILLED else snap["open"]),killed=KILLED,
               closed_today=len(snap["closed_today"]))
    json.dump(state,open(os.path.join(STATE_DIR,"state.json"),"w"),indent=1)
    print(f"[LUKE] {snap['last']} open={snap['n_open']} closed_today={len(snap['closed_today'])} "
          f"new_ledger={len(newrows)} equity=${snap['equity']:,.0f}",flush=True)
    if snap["open"]:
        for p in snap["open"]:
            print(f"   OPEN {p['ticker']:5} {p['setup']} entry={p['entry']:.4f} stop={p['stop']:.4f} "
                  f"close={p['close']:.4f} unreal=${p['unreal']:+.0f}",flush=True)
    return 0

if __name__=="__main__":
    sys.exit(main())
