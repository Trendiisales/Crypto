#!/usr/bin/env python3
"""
NDX companion-clip BT (S-2026-07-01). Settles audit item #3: is the NDX leg's
clip a viable STANDALONE additive book, on the operator's framing?

Operator framing (resolved 2026-07-01): a companion clip is a SEPARATE contract.
Its alternative is "don't take it", NOT "ride wide". So the question is NOT
"clip vs WIDE on one unit" (a dominance test — the error class this audit hunts);
it is: as its own book, is the clip net-positive after costs, both WF halves,
both regimes? If yes it is additive positive-expectancy P&L regardless of WIDE.

FAITHFUL signal: the live NDX leg is TSMom50 (Crypto/include/IbkrCryptoStrat.hpp:66-68,
confirmed via dragleg_study.py -> ./ibkrcrypto_bt): target = sign(close[i] - close[i-50]),
long AND short, lookback 50. Trade = a run of constant sign; flip ends it (=WIDE/ride-to-flip).
Clip = earliest enabled early-exit inside the run. Same clip_exit/metrics/WF/regime machinery
as crypto_companion_lever_sweep.py (operator-accepted as faithful) so it is apples-to-apples.

Data: /Users/jo/Tick/NDX_daily_2016_2026.csv  (ts_sec,o,h,l,c ; ~2637 daily bars 2016-2026).
Cost: 2bp round-trip (index CFD; matches index_turtle_d1_audit / engine "2bp RT").
Entry/exit at bar close (mirrors the crypto harness convention exactly).
"""
import csv
from pathlib import Path

NDX_CSV = Path.home() / "Tick" / "NDX_daily_2016_2026.csv"
LB = 50                 # TSMom lookback (faithful)
SMA_REG = 200           # regime split
ATR_N = 14
COST = 2e-4             # 2bp round-trip


def load():
    o=[];h=[];l=[];c=[];ts=[]
    with open(NDX_CSV) as f:
        for row in csv.reader(f):
            if not row or not (row[0][0]=='-' or row[0][0].isdigit()): continue
            ts.append(int(row[0])); o.append(float(row[1])); h.append(float(row[2]))
            l.append(float(row[3])); c.append(float(row[4]))
    return ts,o,h,l,c


def atr_series(h,l,c,n=ATR_N):
    out=[None]*len(c); tr=[0.0]*len(c)
    for i in range(len(c)):
        tr[i]= h[i]-l[i] if i==0 else max(h[i]-l[i],abs(h[i]-c[i-1]),abs(l[i]-c[i-1]))
    s=0.0
    for i in range(len(c)):
        if i<n: s+=tr[i]
        elif i==n: out[i]=s/n
        elif i>n: out[i]=(out[i-1]*(n-1)+tr[i])/n
    return out


def sma(c,n):
    out=[None]*len(c); s=0.0
    for i,x in enumerate(c):
        s+=x
        if i>=n: s-=c[i-n]
        if i>=n-1: out[i]=s/n
    return out


def tsmom_target(c,i):
    if i<LB: return 0
    d = c[i]-c[i-LB]
    return 1 if d>0 else (-1 if d<0 else 0)


def signals():
    ts,o,h,l,c = load(); n=len(c)
    tgt=[tsmom_target(c,i) for i in range(n)]
    s200=sma(c,SMA_REG); atr=atr_series(h,l,c)
    trades=[]; i=0
    while i<n:
        if tgt[i]==0: i+=1; continue
        s=tgt[i]; ent=i; j=i+1
        while j<n and tgt[j]==s: j+=1
        flip=min(j,n-1)
        bull=(s200[ent] is not None) and (c[ent]>s200[ent])
        atr_pct=(atr[ent]/c[ent]) if (atr[ent] and c[ent]>0) else None
        trades.append((ent,flip,s,bull,atr_pct))
        i=j
    return ts,c,trades


def clip_exit(c, ent, flip, s, cfg, atr_pct):
    entry=c[ent]; peak=0.0; since=0
    rev_atr_pts=(cfg["rev_atr_k"]*atr_pct) if (cfg.get("rev_atr_k") and atr_pct) else None
    for k in range(ent+1, flip+1):
        fav=s*(c[k]/entry-1.0)
        if fav>peak: peak=fav; since=0
        else: since+=1
        if peak < cfg["gate"]: continue
        if cfg.get("stall") and since>=cfg["stall"]: return k
        if cfg.get("rev_frac") and fav<=peak*(1.0-cfg["rev_frac"]): return k
        if rev_atr_pts is not None and fav<=peak-rev_atr_pts: return k
    return None


def pnl_of(c, ent, exit_i, s): return s*(c[exit_i]/c[ent]-1.0)-COST


def run_book(cfg):
    ts,c,trades=signals(); rows=[]
    for ent,flip,s,bull,atr_pct in trades:
        ex = flip if cfg is None else (clip_exit(c,ent,flip,s,cfg,atr_pct) or flip)
        rows.append((ts[ex], pnl_of(c,ent,ex,s), bull))
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
    return dict(n=len(pnl),pf=pf,wr=wr,tot=tot,dd=ddp,mar=(tot/abs(ddp) if ddp<0 else float('inf')))


def halves(rows):
    rows=sorted(rows,key=lambda x:x[0]); m=len(rows)//2
    return rows[:m], rows[m:]


def line(name,m,be,wf):
    print(f"  {name:22s} n={m['n']:4d} PF={m['pf']:5.2f} tot={m['tot']:7.0f}% "
          f"maxDD={m['dd']:7.0f}% MAR={m['mar']:6.2f} bear={be['tot']:7.0f}% WF={'BOTH+' if wf else 'no'}")


def judge(rows):
    m=metrics(rows); be=metrics([x for x in rows if not x[2]])
    h1,h2=halves(rows); wf=metrics(h1)['tot']>0 and metrics(h2)['tot']>0
    standalone = m['tot']>0 and wf and be['tot']>=0
    return m,be,wf,standalone


def main():
    print("=== NDX TSMom50 companion-clip — STANDALONE-book test (faithful TSMom50, 10yr daily) ===\n")
    wide=run_book(None); wm,wbe,wwf,_=judge(wide)
    print("WIDE (ride-to-flip benchmark):")
    line("WIDE",wm,wbe,wwf)

    gates=[0.015,0.05,0.10,0.20]
    cfgs=[]
    for g in gates:
        for rf in [0.10,0.20,0.30,0.50]:
            cfgs.append((f"G{int(g*1000)/10}|revFrac{int(rf*100)}", dict(gate=g,stall=None,rev_frac=rf,rev_atr_k=None)))
        for kk in [1,2,3,4]:
            cfgs.append((f"G{int(g*1000)/10}|revATR{kk}", dict(gate=g,stall=None,rev_frac=None,rev_atr_k=kk)))
        for ns in [3,5]:
            cfgs.append((f"G{int(g*1000)/10}|stall{ns}", dict(gate=g,stall=ns,rev_frac=None,rev_atr_k=None)))

    results=[]
    for name,cfg in cfgs:
        m,be,wf,standalone = judge(run_book(cfg))
        results.append((name,m,be,wf,standalone))

    print("\nALL clip configs (sorted by MAR):")
    for name,m,be,wf,sa in sorted(results,key=lambda r:-r[1]['mar']):
        tag=" <= STANDALONE-VIABLE" if sa else ""
        line(name,m,be,wf);
        if tag: print(f"      {tag.strip()}")

    viable=[r for r in results if r[4]]
    print(f"\nSTANDALONE-VIABLE clip configs (net+ AND WF both halves+ AND bear>=0): {len(viable)}/{len(results)}")
    for name,m,be,wf,sa in sorted(viable,key=lambda r:-r[1]['mar']):
        print(f"  VIABLE {name:22s} tot={m['tot']:.0f}% MAR={m['mar']:.2f} maxDD={m['dd']:.0f}% bear={be['tot']:.0f}%")
    if not viable:
        print("  (none — no clip lever is a net-positive standalone book on NDX TSMom50)")
    print(f"\n  [ref WIDE: tot={wm['tot']:.0f}% MAR={wm['mar']:.2f} maxDD={wm['dd']:.0f}% bear={wbe['tot']:.0f}%]")
    print("  NOTE: per resolved framing, VIABLE configs are additive books on their own merit —")
    print("        NOT to be dismissed for earning less than WIDE (that is the dominance error).")


if __name__=="__main__":
    main()
