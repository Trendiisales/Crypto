#!/usr/bin/env python3
"""
Crypto companion TRIGGER-ISOLATION backtest (S-2026-06-30, follow-up).

Q (operator): on the crypto trend companion, KEEP the reversal-clip and CUT the
stall-clip? Isolate each trigger.

FAITHFUL: extends /tmp/crypto_regime_switch_bt.py (signal ported bar-for-bar from
IbkrCryptoStrat.hpp: KELT N=20 m=2.0; EMAX 20/50 w/ 4*slow EMA seeding). WIDE =
ride to the signal flip (live book's "no profit-stop, exit-on-turn"). Companion
policies differ ONLY in the early-exit trigger set; entries identical.

Policies:
  WIDE       ride to flip (no companion clip) -- the benchmark.
  CLIP_BOTH  arm @GATE then exit on (2-bar STALL) OR (REV_GB giveback of peak).
  REV_ONLY   arm @GATE then exit on REV_GB giveback only (stall removed).
  STALL_ONLY arm @GATE then exit on 2-bar stall only (reversal removed).

Regime = each coin's own close vs own 200d SMA at ENTRY. Cost = live-fidelity RT bps.
WF halves split by exit time so we test both-halves + both-regimes (BACKTEST_TRUTH).
"""
import csv
from pathlib import Path

DATA = Path.home() / "Crypto" / "backtest" / "data"

GATE_PCT = 0.015
N_STALL  = 2
REV_GB   = 0.05
SMA_REG  = 200

COSTBPS = {"BTC": 14, "ETH": 28, "SOL": 11}
SLIP    = {"BTC": 3,  "ETH": 3,  "SOL": 5}
def rt_cost(sym): return (COSTBPS[sym] + 2*1.0 + SLIP[sym]) * 1e-4

KELT_N, KELT_M = 20, 2.0
EMA_F, EMA_S = 20, 50
ALLOW_SHORT = True

POLICIES = ("WIDE", "CLIP_BOTH", "REV_ONLY", "STALL_ONLY")


def load(sym):
    o=[];h=[];l=[];c=[];ms=[]
    with open(DATA / f"{sym}USDT_1d.csv") as f:
        r = csv.reader(f); next(r)
        for row in r:
            ms.append(int(row[0])); o.append(float(row[1])); h.append(float(row[2]))
            l.append(float(row[3])); c.append(float(row[4]))
    return ms,o,h,l,c


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
        st=i-4*p
        if st<0: st=0
        a=2.0/(p+1); e=c[st]
        for j in range(st+1, i+1): e=a*c[j]+(1-a)*e
        return e
    ef=ema(EMA_F); es=ema(EMA_S)
    if ef>es: return 1
    if ef<es: return -1 if ALLOW_SHORT else 0
    return 0


def signals(sym, mode):
    ms,o,h,l,c = load(sym)
    n=len(c); tgt=[0]*n
    for i in range(n):
        tgt[i]= kelt_target(h,l,c,i) if mode=="KELT" else emax_target(c,i)
    s200=sma(c,SMA_REG)
    cost=rt_cost(sym)
    trades=[]
    i=0
    while i<n:
        if tgt[i]==0: i+=1; continue
        s=tgt[i]; ent=i
        j=i+1
        while j<n and tgt[j]==s: j+=1
        flip=min(j, n-1)
        bull = (s200[ent] is not None) and (c[ent] > s200[ent])
        trades.append((ent, flip, s, bull))
        i=j
    return ms,c,trades,cost


def clip_exit(c, ent, flip, s, use_stall, use_rev):
    """earliest enabled-trigger exit index in (ent, flip]; None if never triggers."""
    entry=c[ent]; peak=0.0; since=0
    for k in range(ent+1, flip+1):
        fav=s*(c[k]/entry-1.0)
        if fav>peak: peak=fav; since=0
        else: since+=1
        armed = peak>=GATE_PCT
        if armed and use_stall and since>=N_STALL: return k
        if armed and use_rev and fav<=peak*(1.0-REV_GB): return k
    return None


def pnl_of(c, ent, exit_i, s, cost):
    return s*(c[exit_i]/c[ent]-1.0)-cost


def run(sym, mode):
    ms,c,trades,cost=signals(sym,mode)
    out={p:[] for p in POLICIES}
    for ent,flip,s,bull in trades:
        exits={
            "WIDE":       flip,
            "CLIP_BOTH":  clip_exit(c,ent,flip,s,True ,True ),
            "REV_ONLY":   clip_exit(c,ent,flip,s,False,True ),
            "STALL_ONLY": clip_exit(c,ent,flip,s,True ,False),
        }
        for p in POLICIES:
            ex = exits[p]
            if ex is None: ex = flip
            out[p].append((ms[ex], pnl_of(c,ent,ex,s,cost), bull))
    return out


def metrics(rows, label):
    if not rows: return f"{label:26s}   n=0"
    rows=sorted(rows, key=lambda x:x[0])
    pnl=[r[1] for r in rows]
    wins=sum(p for p in pnl if p>0); loss=sum(-p for p in pnl if p<0)
    pf= wins/loss if loss>0 else float('inf')
    wr=100*sum(1 for p in pnl if p>0)/len(pnl)
    eq=0.0; pk=0.0; dd=0.0
    for p in pnl:
        eq+=p; pk=max(pk,eq); dd=min(dd, eq-pk)
    return (f"{label:26s} n={len(pnl):4d} PF={pf:5.2f} WR={wr:4.0f}% "
            f"tot={100*sum(pnl):8.1f}% maxDD={100*dd:7.1f}%")


def halves(rows):
    rows=sorted(rows, key=lambda x:x[0])
    mid=len(rows)//2
    return rows[:mid], rows[mid:]


def main():
    legs=[("BTC","KELT"),("ETH","KELT"),("SOL","KELT"),
          ("BTC","EMAX"),("ETH","EMAX"),("SOL","EMAX")]
    print(f"params: GATE={GATE_PCT} STALL={N_STALL} REV_GB={REV_GB} SMA={SMA_REG} | "
          f"cost RT bps BTC={rt_cost('BTC')*1e4:.0f} ETH={rt_cost('ETH')*1e4:.0f} SOL={rt_cost('SOL')*1e4:.0f}")
    book={p:[] for p in POLICIES}
    for sym,mode in legs:
        r=run(sym,mode)
        for k in book: book[k]+=r[k]
        print(f"\n=== {sym} {mode} ===")
        for pol in POLICIES:
            print("  "+metrics(r[pol], pol))

    print("\n========== WHOLE CRYPTO TREND BOOK (6 legs) ==========")
    for pol in POLICIES:
        print(metrics(book[pol], pol))

    print("\n---------- WF HALVES (both must be positive) ----------")
    for pol in POLICIES:
        h1,h2=halves(book[pol])
        print(f"{pol:12s}  H1 "+metrics(h1,"").split(None,1)[1]+f"\n{'':12s}  H2 "+metrics(h2,"").split(None,1)[1])

    print("\n---------- REGIME SPLIT (bull-entry vs bear-entry) ----------")
    for pol in POLICIES:
        bull=[x for x in book[pol] if x[2]]; bear=[x for x in book[pol] if not x[2]]
        print(metrics(bull, f"{pol} BULL-entry"))
        print(metrics(bear, f"{pol} BEAR-entry"))


if __name__ == "__main__":
    main()
