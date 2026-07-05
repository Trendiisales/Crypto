#!/usr/bin/env python3
"""
CryptoUpJumpCompanion BEST-PROFIT per-coin roster sweep — FAITHFUL to the live
native UpJumpCompanionEngine.observe() (chimera-direct josgp1).

Generalizes eth_upjump_clip_sweep.py to all 9 live legs
(BTC ETH SOL DOGE BNB ADA TRX NEAR AAVE). For each coin:
  * parent trades come from ibkrcrypto_bt --dump-trades UpJump8 (thr=0.08,W=24 —
    the live default; live_config has NO per-coin override, verified 2026-07-05).
  * companion overlay replicates observe() line-for-line (arm/stall/reversal/reclip).
  * gross_bp=(exit/leg_entry-1)*1e4 per clip; net_bp=gross-20 (0.20% RT Binance taker).

STANDALONE all-6 gate (companion judged on its OWN book, NEVER vs WIDE —
feedback-companion-independent-engine): net>0 AND PF>1 AND WF-H1>0 AND WF-H2>0
AND bull>0 AND bear>0. bull/bear = 200d-regime at PARENT entry.

Goal = BEST NET that still passes all-6 (robust), per coin. Prints current live
roster row for each coin alongside the sweep winner.
"""
import csv, subprocess, os, sys
from pathlib import Path

HERE = Path(__file__).resolve().parent
BT   = HERE / "ibkrcrypto_bt"
TF_S = 3600
RT_BP = 20.0
SMA_REG_BARS = 200*24
FULL_LO, FULL_HI = 1483228800000, 1799999999000

# live roster: coin -> (arm, stall, rev_gb, reclip)  [main.cpp make_companion block]
LIVE = {
    "BTC":  (1.0, 6, 0.30, 0.05),
    "ETH":  (2.0, 6, 0.50, 0.05),
    "SOL":  (3.0, 6, 0.50, 0.05),
    "DOGE": (3.0, 6, 0.50, 0.05),
    "BNB":  (3.0, 6, 0.50, 0.05),
    "ADA":  (2.0, 6, 0.50, 0.05),
    "TRX":  (2.0, 6, 0.0,  0.05),
    "NEAR": (3.0, 8, 0.0,  0.05),
    "AAVE": (1.0, 6, 0.0,  0.0),
}
COINS = list(LIVE.keys())

# sweep grid
ARM    = [1.0, 2.0, 3.0, 5.0, 8.0]
STALL  = [0, 3, 4, 6, 8, 10]
REVGB  = [0.0, 0.20, 0.30, 0.40, 0.50]
RECLIP = [0.0, 0.05]

def load_bars(csvf):
    ts=[]; o=[]; h=[]; l=[]; c=[]
    with open(csvf) as f:
        r=csv.reader(f); next(r)
        for row in r:
            t=int(row[0])
            if t < 1_000_000_000_000: t*=1000
            ts.append(t); o.append(float(row[1])); h.append(float(row[2]))
            l.append(float(row[3])); c.append(float(row[4]))
    N=len(ts)
    idx={t:i for i,t in enumerate(ts)}
    sma=[None]*N; run=0.0
    for i in range(N):
        run+=c[i]
        if i>=SMA_REG_BARS: run-=c[i-SMA_REG_BARS]
        if i>=SMA_REG_BARS-1: sma[i]=run/SMA_REG_BARS
    return ts,o,h,l,c,N,idx,sma

def load_trades(coin, csvf):
    env=dict(os.environ, DUMP_TRADES="1", COSTBPS="18", BT_TF_MS="3600000")
    p=subprocess.run([str(BT),coin,str(csvf),"--dump-trades","UpJump8"],
                     capture_output=True,text=True,env=env)
    tr=[]
    for ln in p.stderr.splitlines():
        if ln.startswith("TRADE"):
            _,ets,xts,epx,dirc,size=ln.split()
            tr.append((int(ets),int(xts),float(epx)))
    return tr

def clips_for_trade(bars, ets, xts, arm, stall_bars, rev_gb, reclip):
    ts,o,h,l,c,N,idx,sma = bars
    ei=idx.get(ets); xi=idx.get(xts)
    if ei is None or xi is None or xi<=ei: return []
    entry_px=o[ei]
    recs=[]
    open_=False; clipped=False; prior_peak=0.0; mfe=0.0; ext_bar=0; open_bar=0
    leg_entry=[entry_px]
    def do_close(exit_px, reason):
        gross_bp=(exit_px/leg_entry[0]-1.0)*1e4
        recs.append((reason, (gross_bp-RT_BP)/100.0, gross_bp/100.0))
    for i in range(ei, xi):
        bar=ts[i]//(TF_S*1000); cur=c[i]
        fav=(cur-entry_px)/entry_px*100.0
        if clipped:
            if reclip>0.0 and prior_peak>0.0 and fav>prior_peak*(1.0+reclip):
                clipped=False; leg_entry[0]=cur
            else:
                continue
        if not open_:
            open_=True; open_bar=bar; mfe=fav; ext_bar=bar
        if fav>mfe+1e-9:
            mfe=fav; ext_bar=bar
        stall=int(bar-ext_bar); armed=mfe>=arm
        if armed and stall_bars>0 and stall>=stall_bars:
            prior_peak=mfe; do_close(cur,"STALL"); clipped=True; open_=False; continue
        if armed and rev_gb>0.0 and fav<=mfe*(1.0-rev_gb):
            prior_peak=mfe; do_close(cur,"REVERSAL"); clipped=True; open_=False; continue
    if open_ and not clipped:
        last=c[xi-1] if xi-1>=ei else o[ei]
        do_close(last,"ENGINE_EXIT")
    return recs

def run_book(bars, trades, arm, stall_bars, rev_gb, reclip):
    ts,o,h,l,c,N,idx,sma = bars
    rows=[]
    for ets,xts,epx in trades:
        ei=idx.get(ets)
        bull = (ei is not None and sma[ei] is not None and c[ei]>sma[ei])
        for reason,net_pct,gross_pct in clips_for_trade(bars,ets,xts,arm,stall_bars,rev_gb,reclip):
            rows.append((xts, net_pct, gross_pct, bull))
    return rows

def metrics(rows):
    if not rows: return dict(n=0,net=0.0,pf=0.0,h1=0.0,h2=0.0,bull=0.0,bear=0.0,passed=False)
    net=sum(r[1] for r in rows)
    gw=sum(r[1] for r in rows if r[1]>0); gl=-sum(r[1] for r in rows if r[1]<0)
    pf=gw/gl if gl>0 else (999 if gw>0 else 0)
    ex=sorted(r[0] for r in rows); mid=ex[len(ex)//2]
    h1=sum(r[1] for r in rows if r[0]<mid); h2=sum(r[1] for r in rows if r[0]>=mid)
    bull=sum(r[1] for r in rows if r[3]); bear=sum(r[1] for r in rows if not r[3])
    passed=(net>0 and pf>1 and h1>0 and h2>0 and bull>0 and bear>0)
    return dict(n=len(rows),net=net,pf=pf,h1=h1,h2=h2,bull=bull,bear=bear,passed=passed)

def fmt(tag,m,flag=None):
    f = flag if flag else ("PASS" if m["passed"] else "fail")
    return (f"{tag:30s} n={m['n']:3d} net={m['net']:+8.1f}% PF={m['pf']:5.2f} "
            f"H1={m['h1']:+7.1f} H2={m['h2']:+7.1f} bull={m['bull']:+7.1f} bear={m['bear']:+7.1f}  [{f}]")

def main():
    only = sys.argv[1:] if len(sys.argv)>1 else COINS
    summary=[]
    for coin in only:
        csvf = HERE/"data"/f"{coin}USDT_1h.csv"
        if not csvf.exists():
            print(f"\n### {coin}: NO DATA {csvf}"); continue
        bars=load_bars(csvf); trades=load_trades(coin,csvf)
        print(f"\n{'='*100}\n### {coin}  (parent UpJump8 trades: {len(trades)})")
        la,ls,lr,lc = LIVE[coin]
        mlive=metrics(run_book(bars,trades,la,ls,lr,lc))
        print("  LIVE  " + fmt(f"arm{la:g} S{ls} R{int(lr*100):02d} reclip{int(lc*100):02d}", mlive))
        # sweep
        best=None; passing=[]
        for arm in ARM:
            for st in STALL:
                for rg in REVGB:
                    for rc in RECLIP:
                        if st==0 and rg==0.0: continue  # no exit lever at all
                        m=metrics(run_book(bars,trades,arm,st,rg,rc))
                        if m["passed"]:
                            passing.append((arm,st,rg,rc,m))
                            if best is None or m["net"]>best[4]["net"]:
                                best=(arm,st,rg,rc,m)
        if best:
            arm,st,rg,rc,m=best
            print("  BEST  " + fmt(f"arm{arm:g} S{st} R{int(rg*100):02d} reclip{int(rc*100):02d}", m, "BEST"))
            passing.sort(key=lambda x:-x[4]["net"])
            for arm,st,rg,rc,m in passing[1:4]:
                print("        " + fmt(f"arm{arm:g} S{st} R{int(rg*100):02d} reclip{int(rc*100):02d}", m))
            b=best
            summary.append((coin, mlive, b))
        else:
            print("  BEST  (NO config passes all-6 standalone gate)")
            summary.append((coin, mlive, None))
    # final table
    print(f"\n\n{'='*100}\nSUMMARY — best-net passing all-6 STANDALONE vs live roster (net %, all clips, RT-20bp)")
    print(f"{'coin':5s} {'LIVE cfg':22s} {'LIVE net':>10s} {'LIVE ok':>7s}   {'BEST cfg':22s} {'BEST net':>10s}   uplift")
    for coin,mlive,b in summary:
        la,ls,lr,lc=LIVE[coin]
        lcfg=f"arm{la:g}/S{ls}/R{int(lr*100)}/rc{int(lc*100)}"
        lok="PASS" if mlive["passed"] else "fail"
        if b:
            arm,st,rg,rc,m=b
            bcfg=f"arm{arm:g}/S{st}/R{int(rg*100)}/rc{int(rc*100)}"
            up=m["net"]-mlive["net"]
            print(f"{coin:5s} {lcfg:22s} {mlive['net']:+10.1f} {lok:>7s}   {bcfg:22s} {m['net']:+10.1f}   {up:+8.1f}")
        else:
            print(f"{coin:5s} {lcfg:22s} {mlive['net']:+10.1f} {lok:>7s}   {'(none pass)':22s} {'--':>10s}")

if __name__=="__main__":
    main()
