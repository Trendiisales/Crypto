#!/usr/bin/env python3
# (S-2026-06-24) DRAG-LEG study for the IBKRCrypto book: one leg in deep drawdown
# while the book is up -- what's the right protection? Tests candidate solutions on the
# REAL per-engine daily equity curves (ibkrcrypto_bt --equity), the same rigour as the
# BigCapMomo give-back study. Verdict by book return / maxDD / Sharpe / Calmar.
import subprocess, math, os
BT="./ibkrcrypto_bt"; TICK="/Users/jo/Tick"
D=lambda f: f"data/{f}USDT_1d.csv"
ROSTER=[  # (label, sym, csv, strat)
 ("eth_emax","ETH",D("ETH"),"EMAx"), ("eth_kelt","ETH",D("ETH"),"Kelt"),
 ("btc_emax","BTC",D("BTC"),"EMAx"), ("btc_kelt","BTC",D("BTC"),"Kelt"),
 ("sol_emax","SOL",D("SOL"),"EMAx"), ("sol_kelt","SOL",D("SOL"),"Kelt"),
 ("ndx_tsmom","NDX",f"{TICK}/NDX_daily_2016_2026.csv","TSMom50"),
 ("btc_reg","BTC",D("BTC"),"Regime"), ("eth_reg","ETH",D("ETH"),"Regime"), ("sol_reg","SOL",D("SOL"),"Regime"),
 ("btc_ibs","BTC",D("BTC"),"IBS"), ("sol_ibs","SOL",D("SOL"),"IBS"),
 ("ndx_rsir","NDX",f"{TICK}/NDX_daily_2016_2026.csv","RSIrev"),
]
def curve(sym,csv,strat):
    out=subprocess.run([BT,sym,csv,"--equity",strat],capture_output=True,text=True).stdout
    d={}
    for ln in out.splitlines():
        if ln and ln[0].isdigit():
            ts,eq=ln.split(","); d[int(ts)//86400000]=float(eq)   # day-index -> cum equity (sum-ret, vol-targeted)
    return d
# build aligned daily-RETURN matrix (delta of cum equity)
print("generating 13 engine curves..."); curves={lbl:curve(s,c,st) for lbl,s,c,st in ROSTER}
days=sorted(set().union(*[set(c) for c in curves.values()]))
labels=[r[0] for r in ROSTER]
# per-day per-leg return (delta); forward-fill cum, delta
ret={}; last={l:0.0 for l in labels}
for l in labels:
    ck=curves[l]; prev=0.0; ret[l]=[]
    for d in days:
        cur=ck.get(d,prev); ret[l].append(cur-prev); prev=cur
T=len(days)
def book_stats(daily):  # list of book daily P&L
    eq=[]; c=0.0
    for x in daily: c+=x; eq.append(c)
    pk=-1e9; mdd=0
    for e in eq: pk=max(pk,e); mdd=max(mdd,pk-e)
    mu=sum(daily)/T; sd=(sum((x-mu)**2 for x in daily)/T)**.5
    sh=mu/sd*math.sqrt(252) if sd>0 else 0
    tot=eq[-1]
    cal=(tot/ (mdd if mdd>0 else 1))
    return dict(tot=tot*100, mdd=mdd*100, sharpe=sh, calmar=cal)
# ---- solutions ----
def sol_baseline():
    return [sum(ret[l][t] for l in labels) for t in range(T)]
def sol_perleg_stop(dd_stop):           # cut a leg once ITS drawdown > dd_stop (the 'wrong' answer)
    daily=[]; legpk={l:0.0 for l in labels}; legeq={l:0.0 for l in labels}; stopped={l:False for l in labels}
    for t in range(T):
        d=0.0
        for l in labels:
            if stopped[l]:
                # re-arm if the leg would have recovered to a new high (flat then re-enter)
                legeq[l]+=ret[l][t]
                if legeq[l]>=legpk[l]: stopped[l]=False
                continue
            d+=ret[l][t]; legeq[l]+=ret[l][t]; legpk[l]=max(legpk[l],legeq[l])
            if legpk[l]-legeq[l] > dd_stop: stopped[l]=True
        daily.append(d)
    return daily
def sol_book_ddcap(dd_cap, scale=0.5):  # halve gross when BOOK dd > dd_cap
    daily=[]; eq=0.0; pk=0.0
    for t in range(T):
        s = scale if (pk-eq)>dd_cap else 1.0
        d=s*sum(ret[l][t] for l in labels); daily.append(d); eq+=d; pk=max(pk,eq)
    return daily
def sol_corr_downsize(win=30, thr=0.5, scale=0.5):  # downsize a leg whose rolling corr to the book is high
    daily=[]
    bookret=[sum(ret[l][t] for l in labels) for t in range(T)]
    for t in range(T):
        d=0.0
        for l in labels:
            w=1.0
            if t>=win:
                a=ret[l][t-win:t]; b=bookret[t-win:t]
                ma=sum(a)/win; mb=sum(b)/win
                ca=sum((a[i]-ma)*(b[i]-mb) for i in range(win))
                va=(sum((x-ma)**2 for x in a))**.5; vb=(sum((x-mb)**2 for x in b))**.5
                if va>0 and vb>0 and ca/(va*vb)>thr: w=scale
            d+=w*ret[l][t]
        daily.append(d)
    return daily
def sol_drop_worst(win=20):             # each day exclude the leg with worst trailing-win return
    daily=[]
    for t in range(T):
        if t<win: daily.append(sum(ret[l][t] for l in labels)); continue
        trail={l:sum(ret[l][t-win:t]) for l in labels}
        worst=min(trail,key=trail.get)
        daily.append(sum(ret[l][t] for l in labels if l!=worst))
    return daily
if __name__=="__main__":
    print(f"{T} aligned days, {len(labels)} engines\n")
    rows=[("baseline (no intervention)", sol_baseline()),
          ("per-leg stop @8% (the 'wrong' one)", sol_perleg_stop(0.08)),
          ("per-leg stop @15%", sol_perleg_stop(0.15)),
          ("BOOK dd-cap @5% -> halve gross", sol_book_ddcap(0.05)),
          ("BOOK dd-cap @8% -> halve gross", sol_book_ddcap(0.08)),
          ("corr-downsize (corr>0.5 -> halve)", sol_corr_downsize()),
          ("drop-worst-leg (trailing 20d)", sol_drop_worst())]
    print(f"  {'solution':36} {'tot':>8} {'maxDD':>7} {'Sharpe':>7} {'Calmar':>7}")
    print("  "+"-"*70)
    for name,daily in rows:
        s=book_stats(daily)
        print(f"  {name:36} {s['tot']:+7.1f}% {s['mdd']:6.1f}% {s['sharpe']:6.2f} {s['calmar']:6.2f}")
