#!/usr/bin/env python3
"""
Crypto INTRADAY companion validation BT (S-2026-06-30).

Q (operator): the intraday companion (1h/4h, 2% ABSOLUTE giveback) banks +$87 live
(n=18, exploratory). Does the reversal-clip actually EARN on the intraday timeframe,
or is it daily's "clip loses to WIDE" verdict in disguise?

FAITHFUL: same signal port as crypto_companion_trigger_bt.py (KELT N=20 m=2.0; EMAX
20/50). Run on 1h AND 4h bars (the intraday book's TFs). Companion rule = the LIVE
intraday cron: GATE 1.0%, REVERSAL = 2% ABSOLUTE giveback from MFE peak (REV_GB_PTS),
stall = 2 bars. WIDE = ride to flip. Regime = own 200-bar SMA at entry. Cost swept
(ladder vs perp) because intraday flips often and cost dominates.

Policies: WIDE | REV_ONLY (2% abs) | STALL_ONLY | CLIP_BOTH.
PASS gate to wire = REV_ONLY net-positive AND both WF-halves positive AND not wrecked
in bear regime, at the realistic (ladder) cost.
"""
import csv, sys
from pathlib import Path

DATA = Path.home() / "Crypto" / "backtest" / "data"

GATE_PCT  = 0.010
N_STALL   = 2
REV_GB_PTS= 0.020   # 2 percentage-points absolute giveback from peak (intraday rule)
SMA_REG   = 200

KELT_N, KELT_M = 20, 2.0
EMA_F, EMA_S = 20, 50
ALLOW_SHORT = True
POLICIES = ("WIDE", "REV_ONLY", "STALL_ONLY", "CLIP_BOTH")

# cost scenarios (RT fraction). ladder = what the cheap micro/ladder path pays; perp = SQF perp.
COST = {
    "ladder": {"BTC": (6+3)*1e-4,  "ETH": (8+3)*1e-4,  "SOL": (2+5)*1e-4},
    "perp":   {"BTC": (22+3)*1e-4, "ETH": (28+3)*1e-4, "SOL": (22+5)*1e-4},
}


def load(sym, tf):
    h=[];l=[];c=[];ms=[]
    with open(DATA / f"{sym}USDT_{tf}.csv") as f:
        r = csv.reader(f); next(r)
        for row in r:
            ms.append(int(row[0])); h.append(float(row[2]))
            l.append(float(row[3])); c.append(float(row[4]))
    return ms,h,l,c


def sma(c, n):
    out=[None]*len(c); s=0.0
    for i,x in enumerate(c):
        s+=x
        if i>=n: s-=c[i-n]
        if i>=n-1: out[i]=s/n
    return out


def kelt_target(h,l,c,i):
    N=KELT_N
    if i < N: return 0
    a=2.0/(N+1); e=c[i-N]
    for j in range(i-N+1, i+1): e=a*c[j]+(1-a)*e
    atr=0.0
    for j in range(i-N+1, i+1):
        tr=max(h[j]-l[j], abs(h[j]-c[j-1]), abs(l[j]-c[j-1])); atr+=tr
    atr/=N
    if atr<=0: return 0
    cc=c[i]
    if cc>e+KELT_M*atr: return 1
    if cc<e-KELT_M*atr: return -1 if ALLOW_SHORT else 0
    return 0


def emax_target(c,i):
    if i < 4*EMA_S: return 0
    def ema(p):
        st=max(0,i-4*p); a=2.0/(p+1); e=c[st]
        for j in range(st+1, i+1): e=a*c[j]+(1-a)*e
        return e
    ef=ema(EMA_F); es=ema(EMA_S)
    if ef>es: return 1
    if ef<es: return -1 if ALLOW_SHORT else 0
    return 0


def signals(sym, mode, tf, cost):
    ms,h,l,c = load(sym, tf)
    n=len(c); tgt=[0]*n
    for i in range(n):
        tgt[i]= kelt_target(h,l,c,i) if mode=="KELT" else emax_target(c,i)
    s200=sma(c,SMA_REG)
    trades=[]; i=0
    while i<n:
        if tgt[i]==0: i+=1; continue
        s=tgt[i]; ent=i; j=i+1
        while j<n and tgt[j]==s: j+=1
        flip=min(j, n-1)
        bull=(s200[ent] is not None) and (c[ent] > s200[ent])
        trades.append((ent, flip, s, bull)); i=j
    return ms,c,trades,cost


def clip_exit(c, ent, flip, s, use_stall, use_rev):
    entry=c[ent]; peak=0.0; since=0
    for k in range(ent+1, flip+1):
        fav=s*(c[k]/entry-1.0)
        if fav>peak: peak=fav; since=0
        else: since+=1
        armed = peak>=GATE_PCT
        if armed and use_stall and since>=N_STALL: return k
        if armed and use_rev and fav<=peak-REV_GB_PTS: return k
    return None


def pnl_of(c, ent, x, s, cost): return s*(c[x]/c[ent]-1.0)-cost


def run(sym, mode, tf, cost):
    ms,c,trades,cost=signals(sym,mode,tf,cost)
    out={p:[] for p in POLICIES}
    for ent,flip,s,bull in trades:
        exits={"WIDE":flip,
               "REV_ONLY":   clip_exit(c,ent,flip,s,False,True ),
               "STALL_ONLY": clip_exit(c,ent,flip,s,True ,False),
               "CLIP_BOTH":  clip_exit(c,ent,flip,s,True ,True )}
        for p in POLICIES:
            x=exits[p]; x=flip if x is None else x
            out[p].append((ms[x], pnl_of(c,ent,x,s,cost), bull))
    return out


def metrics(rows, label):
    if not rows: return f"{label:24s}   n=0"
    rows=sorted(rows, key=lambda x:x[0]); pnl=[r[1] for r in rows]
    wins=sum(p for p in pnl if p>0); loss=sum(-p for p in pnl if p<0)
    pf= wins/loss if loss>0 else float('inf')
    wr=100*sum(1 for p in pnl if p>0)/len(pnl)
    eq=0.0; pk=0.0; dd=0.0
    for p in pnl: eq+=p; pk=max(pk,eq); dd=min(dd,eq-pk)
    return (f"{label:24s} n={len(pnl):4d} PF={pf:5.2f} WR={wr:4.0f}% "
            f"tot={100*sum(pnl):8.1f}% maxDD={100*dd:7.1f}%")


def halves(rows):
    rows=sorted(rows, key=lambda x:x[0]); m=len(rows)//2
    return rows[:m], rows[m:]


def main():
    scen = sys.argv[1] if len(sys.argv)>1 else "ladder"
    costmap = COST[scen]
    legs=[(s,mode,tf) for tf in ("4h","1h") for mode in ("KELT","EMAX") for s in ("BTC","ETH","SOL")]
    print(f"=== INTRADAY companion BT | cost={scen} | GATE={GATE_PCT} REV_GB_PTS={REV_GB_PTS} STALL={N_STALL} ===")
    book={p:[] for p in POLICIES}; bytf={"4h":{p:[] for p in POLICIES},"1h":{p:[] for p in POLICIES}}
    for sym,mode,tf in legs:
        r=run(sym,mode,tf,costmap[sym])
        for k in book: book[k]+=r[k]; bytf[tf][k]+=r[k]
    for tf in ("4h","1h"):
        print(f"\n----- {tf} book (BTC/ETH/SOL x KELT/EMAX) -----")
        for pol in POLICIES: print("  "+metrics(bytf[tf][pol], pol))
    print("\n========== WHOLE INTRADAY BOOK (12 legs) ==========")
    for pol in POLICIES: print(metrics(book[pol], pol))
    print("\n---------- WF HALVES ----------")
    for pol in POLICIES:
        h1,h2=halves(book[pol])
        print(f"{pol:11s} H1 "+metrics(h1,"").split(None,1)[1])
        print(f"{'':11s} H2 "+metrics(h2,"").split(None,1)[1])
    print("\n---------- REGIME SPLIT ----------")
    for pol in POLICIES:
        bull=[x for x in book[pol] if x[2]]; bear=[x for x in book[pol] if not x[2]]
        print(metrics(bull, f"{pol} BULL")); print(metrics(bear, f"{pol} BEAR"))


if __name__ == "__main__":
    main()
