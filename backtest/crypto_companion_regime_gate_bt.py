#!/usr/bin/env python3
"""
Crypto companion REGIME-GATE BT (S-2026-07-01).

Answers the open question [[RegimeSwitchTransfer]]: the global lever sweep
(crypto_companion_lever_sweep.py) found NO single clip config beats riding WIDE,
because the clip helps in favourable regime but wrecks the bear half. A global
on/off can't express that. This BT tests the obvious next policy:

  REGIME-GATED book = clip ONLY on bull-regime trades (200d SMA at entry),
  ride WIDE on bear-regime trades.

If the clip's value is genuinely regime-conditional, gating recovers the bull-side
banked-profit / lower-DD without paying the bear-side churn that parked it.

FAITHFUL: identical signal/cost/WF/regime machinery as crypto_companion_lever_sweep.py
(KELT N=20 m=2.0; EMAX 20/50 w/ 4*slow seed; ride-to-flip = WIDE; live RT bps; WF
halves split by exit time; regime = own 200d SMA at entry). ONLY the exit policy
gains a regime switch.

PASS (a gate is worth shipping) = gated book beats WIDE on total-return OR MAR,
AND both WF halves positive, AND the bear half is NOT worse than WIDE's bear half
(the gate must not damage bear -- it should leave bear = WIDE by construction).
"""
import csv
from pathlib import Path

DATA = Path.home() / "Crypto" / "backtest" / "data"

SMA_REG = 200
KELT_N, KELT_M = 20, 2.0
EMA_F, EMA_S = 20, 50
ALLOW_SHORT = True
ATR_N = 14

COSTBPS = {"BTC": 14, "ETH": 28, "SOL": 11}
SLIP    = {"BTC": 3,  "ETH": 3,  "SOL": 5}
def rt_cost(sym): return (COSTBPS[sym] + 2*1.0 + SLIP[sym]) * 1e-4

LEGS = [("BTC","KELT"),("ETH","KELT"),("SOL","KELT"),
        ("BTC","EMAX"),("ETH","EMAX"),("SOL","EMAX")]


def load(sym):
    o=[];h=[];l=[];c=[];ms=[]
    with open(DATA / f"{sym}USDT_1d.csv") as f:
        r = csv.reader(f); next(r)
        for row in r:
            ms.append(int(row[0])); o.append(float(row[1])); h.append(float(row[2]))
            l.append(float(row[3])); c.append(float(row[4]))
    return ms,o,h,l,c


def atr_series(h,l,c,n=ATR_N):
    out=[None]*len(c); tr=[0.0]*len(c)
    for i in range(len(c)):
        if i==0: tr[i]=h[i]-l[i]
        else: tr[i]=max(h[i]-l[i], abs(h[i]-c[i-1]), abs(l[i]-c[i-1]))
    s=0.0
    for i in range(len(c)):
        if i< n: s+=tr[i]
        elif i==n: out[i]=s/n
        elif i> n: out[i]=(out[i-1]*(n-1)+tr[i])/n
    return out


def sma(c,n):
    out=[None]*len(c); s=0.0
    for i,x in enumerate(c):
        s+=x
        if i>=n: s-=c[i-n]
        if i>=n-1: out[i]=s/n
    return out


def kelt_target(h,l,c,i):
    N=KELT_N
    if i<N: return 0
    a=2.0/(N+1); e=c[i-N]
    for j in range(i-N+1,i+1): e=a*c[j]+(1-a)*e
    atr=0.0
    for j in range(i-N+1,i+1):
        tr=max(h[j]-l[j], abs(h[j]-c[j-1]), abs(l[j]-c[j-1])); atr+=tr
    atr/=N
    if atr<=0: return 0
    cc=c[i]
    if cc>e+KELT_M*atr: return 1
    if cc<e-KELT_M*atr: return -1 if ALLOW_SHORT else 0
    return 0


def emax_target(c,i):
    if i<4*EMA_S: return 0
    def ema(p):
        st=max(0,i-4*p); a=2.0/(p+1); e=c[st]
        for j in range(st+1,i+1): e=a*c[j]+(1-a)*e
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
    s200=sma(c,SMA_REG); atr=atr_series(h,l,c)
    cost=rt_cost(sym)
    trades=[]; i=0
    while i<n:
        if tgt[i]==0: i+=1; continue
        s=tgt[i]; ent=i; j=i+1
        while j<n and tgt[j]==s: j+=1
        flip=min(j,n-1)
        bull=(s200[ent] is not None) and (c[ent]>s200[ent])
        atr_pct = (atr[ent]/c[ent]) if (atr[ent] and c[ent]>0) else None
        trades.append((ent,flip,s,bull,atr_pct))
        i=j
    return ms,c,trades,cost


def clip_exit(c, ent, flip, s, cfg, atr_pct):
    entry=c[ent]; peak=0.0; since=0
    rev_atr_pts = (cfg["rev_atr_k"]*atr_pct) if (cfg.get("rev_atr_k") and atr_pct) else None
    for k in range(ent+1, flip+1):
        fav=s*(c[k]/entry-1.0)
        if fav>peak: peak=fav; since=0
        else: since+=1
        if peak < cfg["gate"]: continue
        if cfg.get("stall") and since>=cfg["stall"]: return k
        if cfg.get("rev_frac") and fav<=peak*(1.0-cfg["rev_frac"]): return k
        if rev_atr_pts is not None and fav<=peak-rev_atr_pts: return k
    return None


def pnl_of(c, ent, exit_i, s, cost): return s*(c[exit_i]/c[ent]-1.0)-cost


def run_book(cfg, gate_regime=False):
    """cfg=None -> pure WIDE. gate_regime=True -> clip only bull trades, ride bear WIDE."""
    rows=[]
    for sym,mode in LEGS:
        ms,c,trades,cost=signals(sym,mode)
        for ent,flip,s,bull,atr_pct in trades:
            if cfg is None:
                ex=flip
            elif gate_regime and not bull:
                ex=flip                     # bear -> ride WIDE
            else:
                ex=clip_exit(c,ent,flip,s,cfg,atr_pct)
                if ex is None: ex=flip
            rows.append((ms[ex], pnl_of(c,ent,ex,s,cost), bull))
    return rows


def metrics(rows):
    if not rows: return dict(n=0,pf=0,wr=0,tot=0,dd=0,mar=0)
    rows=sorted(rows,key=lambda x:x[0]); pnl=[r[1] for r in rows]
    wins=sum(p for p in pnl if p>0); loss=sum(-p for p in pnl if p<0)
    pf=wins/loss if loss>0 else float('inf')
    wr=100*sum(1 for p in pnl if p>0)/len(pnl)
    eq=0.0;pk=0.0;dd=0.0
    for p in pnl:
        eq+=p;pk=max(pk,eq);dd=min(dd,eq-pk)
    tot=100*sum(pnl); ddp=100*dd
    mar=tot/abs(ddp) if ddp<0 else float('inf')
    return dict(n=len(pnl),pf=pf,wr=wr,tot=tot,dd=ddp,mar=mar)


def halves(rows):
    rows=sorted(rows,key=lambda x:x[0]); m=len(rows)//2
    return rows[:m], rows[m:]


def summ(tag, rows):
    m=metrics(rows); b=metrics([x for x in rows if x[2]]); be=metrics([x for x in rows if not x[2]])
    h1,h2=halves(rows); m1=metrics(h1); m2=metrics(h2)
    print(f"{tag}")
    print(f"  ALL  n={m['n']} PF={m['pf']:.2f} tot={m['tot']:.0f}% maxDD={m['dd']:.0f}% MAR={m['mar']:.2f}")
    print(f"  BULL tot={b['tot']:.0f}%   BEAR tot={be['tot']:.0f}%   WF H1={m1['tot']:.0f}% H2={m2['tot']:.0f}%")
    return m,be,(m1['tot']>0 and m2['tot']>0)


def main():
    wide=run_book(None)
    wm,wbe,_=summ("WIDE (ride-to-flip benchmark):", wide)
    print()

    # candidate clip configs to gate: the fixed-param default + the sweep's standouts.
    cfgs=[
        ("default5%",    dict(gate=0.015,stall=None,rev_frac=0.05,rev_atr_k=None)),
        ("G5|revFrac30", dict(gate=0.05, stall=None,rev_frac=0.30,rev_atr_k=None)),
        ("G5|revATR2",   dict(gate=0.05, stall=None,rev_frac=None,rev_atr_k=2)),
        ("G10|revATR2",  dict(gate=0.10, stall=None,rev_frac=None,rev_atr_k=2)),
        ("G10|revFrac50",dict(gate=0.10, stall=None,rev_frac=0.50,rev_atr_k=None)),
    ]

    print("="*70)
    print("GLOBAL clip (every trade) vs REGIME-GATED clip (bull only, bear rides WIDE)")
    print("="*70)
    for name,cfg in cfgs:
        print(f"\n--- {name} ---")
        gm,gbe,gwf = summ("  [GLOBAL] clip all trades:", run_book(cfg, gate_regime=False))
        rm,rbe,rwf = summ("  [GATED ] clip bull only:  ", run_book(cfg, gate_regime=True))
        # PASS test for the gated policy
        beats = (rm['tot']>=wm['tot']) or (rm['mar']>=wm['mar'])
        bear_ok = rbe['tot']>=wbe['tot']-1e-9   # gate must not damage bear (should == wide bear)
        verdict = "PASS" if (beats and rwf and bear_ok) else "fail"
        why=[]
        if not beats: why.append("no beat tot/MAR")
        if not rwf:   why.append("WF half neg")
        if not bear_ok: why.append("bear damaged")
        print(f"  GATED VERDICT: {verdict}" + (f"  ({'; '.join(why)})" if why else "  (beats WIDE, WF+ both, bear intact)"))


if __name__ == "__main__":
    main()
