#!/usr/bin/env python3
"""
Crypto companion LEVER SWEEP (S-2026-07-01, operator: "find a setting that works;
crypto swings harder -> scale the triggers; don't accept blind 'doesn't work';
show profit + risk-adjusted, not just raw return vs WIDE").

FAITHFUL: identical signal/cost/WF/regime machinery as crypto_companion_trigger_bt.py
(KELT N=20 m=2.0; EMAX 20/50 w/ 4*slow seed; ride-to-flip = WIDE; live RT bps; WF
halves split by exit time; regime = own 200d SMA at entry). ONLY the early-exit
trigger is swept.

What this answers that the fixed-param BT did NOT:
  1. Risk-adjusted, not just total return. Reports PF, maxDD, and MAR=tot/|maxDD|.
     The clip's whole point is banking profit with LESS drawdown -- judge it there.
  2. Crypto-vol-scaled triggers. The fixed 5% giveback is gold-sized noise on crypto.
     Sweep (a) wider fraction-of-peak givebacks and (b) ATR-scaled givebacks
     (clip when fav <= peak - k*ATR%_at_entry) so the trigger breathes with each
     coin's volatility -- exactly the "make provision for harder swings" ask.
  3. Wider profit GATE before arming, so a clip never fires until a real cushion exists.

PASS = a config whose total return is competitive with WIDE *or* whose MAR beats WIDE,
AND both WF halves positive AND bear-regime not wrecked. Otherwise the clip is a
worse policy than riding wide and we keep WIDE.
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
    """Wilder ATR; atr[i] usable from i>=n. Returns list (None until seeded)."""
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
    """earliest enabled-trigger exit in (ent,flip]; None if never. cfg keys:
       gate (frac), stall (bars or None), rev_frac (giveback as frac-of-peak or None),
       rev_atr_k (giveback as k*ATR% absolute or None)."""
    entry=c[ent]; peak=0.0; since=0
    rev_atr_pts = (cfg["rev_atr_k"]*atr_pct) if (cfg.get("rev_atr_k") and atr_pct) else None
    for k in range(ent+1, flip+1):
        fav=s*(c[k]/entry-1.0)
        if fav>peak: peak=fav; since=0
        else: since+=1
        if peak < cfg["gate"]: continue          # not armed
        if cfg.get("stall") and since>=cfg["stall"]: return k
        if cfg.get("rev_frac") and fav<=peak*(1.0-cfg["rev_frac"]): return k
        if rev_atr_pts is not None and fav<=peak-rev_atr_pts: return k
    return None


def pnl_of(c, ent, exit_i, s, cost): return s*(c[exit_i]/c[ent]-1.0)-cost


def run_book(cfg):
    rows=[]
    for sym,mode in LEGS:
        ms,c,trades,cost=signals(sym,mode)
        for ent,flip,s,bull,atr_pct in trades:
            if cfg is None: ex=flip
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


def main():
    wide=run_book(None); wm=metrics(wide)
    wb=metrics([x for x in wide if x[2]]); wbe=metrics([x for x in wide if not x[2]])
    print("WIDE (ride-to-flip benchmark, 6 crypto trend legs):")
    print(f"  ALL   n={wm['n']} PF={wm['pf']:.2f} tot={wm['tot']:.0f}% maxDD={wm['dd']:.0f}% MAR={wm['mar']:.2f}")
    print(f"  BULL  tot={wb['tot']:.0f}%  BEAR tot={wbe['tot']:.0f}%")
    h1,h2=halves(wide); print(f"  WF H1 tot={metrics(h1)['tot']:.0f}%  H2 tot={metrics(h2)['tot']:.0f}%")

    # ---- lever grid ----
    gates=[0.015,0.05,0.10,0.20]
    cfgs=[]
    for g in gates:
        for rf in [0.10,0.20,0.30,0.50]:                  # fraction-of-peak giveback (wider for crypto)
            cfgs.append((f"G{int(g*1000)/10}|revFrac{int(rf*100)}", dict(gate=g,stall=None,rev_frac=rf,rev_atr_k=None)))
        for kk in [1,2,3,4]:                               # ATR-scaled giveback (vol-breathing)
            cfgs.append((f"G{int(g*1000)/10}|revATR{kk}",       dict(gate=g,stall=None,rev_frac=None,rev_atr_k=kk)))
        for ns in [3,5]:                                   # stall-only (wider than gold's 2)
            cfgs.append((f"G{int(g*1000)/10}|stall{ns}",        dict(gate=g,stall=ns,rev_frac=None,rev_atr_k=None)))

    results=[]
    for name,cfg in cfgs:
        book=run_book(cfg); m=metrics(book)
        b=metrics([x for x in book if x[2]]); be=metrics([x for x in book if not x[2]])
        h1,h2=halves(book); m1=metrics(h1); m2=metrics(h2)
        wf_ok = m1['tot']>0 and m2['tot']>0
        bear_ok = be['tot'] >= wbe['tot']*0.5 if wbe['tot']>0 else be['tot']>=0
        beats_tot = m['tot'] >= wm['tot']
        beats_mar = m['mar'] >= wm['mar']
        results.append((name,m,be,wf_ok,bear_ok,beats_tot,beats_mar))

    def show(rows,title):
        print(f"\n{title}")
        print(f"  {'config':22s} {'tot%':>7s} {'maxDD%':>8s} {'MAR':>6s} {'PF':>5s} {'bearTot':>8s} {'WF':>3s} {'>wTot':>6s} {'>wMAR':>6s}")
        for name,m,be,wf,bk,bt,bm in rows:
            print(f"  {name:22s} {m['tot']:7.0f} {m['dd']:8.0f} {m['mar']:6.2f} {m['pf']:5.2f} "
                  f"{be['tot']:8.0f} {'Y' if wf else '.':>3s} {'Y' if bt else '.':>6s} {'Y' if bm else '.':>6s}")

    show(sorted(results,key=lambda r:-r[1]['mar'])[:12], "TOP 12 by MAR (risk-adjusted):")
    show(sorted(results,key=lambda r:-r[1]['tot'])[:12], "TOP 12 by total return:")

    winners=[r for r in results if (r[5] or r[6]) and r[3] and r[4]]
    print(f"\nCONFIGS that beat WIDE on tot-or-MAR AND pass WF-both-halves AND bear-ok: {len(winners)}")
    for name,m,be,wf,bk,bt,bm in winners:
        print(f"  WIN {name:22s} tot={m['tot']:.0f}% MAR={m['mar']:.2f} (wide tot={wm['tot']:.0f}% MAR={wm['mar']:.2f})")
    if not winners:
        print("  (none -- on these 6 trend legs no clip lever beats riding wide on return or MAR)")


if __name__ == "__main__":
    main()
