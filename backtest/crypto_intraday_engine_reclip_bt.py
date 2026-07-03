#!/usr/bin/env python3
"""
Intraday crypto ENGINE + COMPANION(reclip) faithful BT  (S-2026-07-02).

Purpose: pick what to trade intraday to REPLACE the stripped-out intraday legs
(dead .vt legs / cost-wall MR casualties / retired 2%-absolute companion), and
validate each survivor STANDALONE all-6, then layer the DAILY-winning companion
style (fractional arm-gate + gv + retrig/reclip) on top -- judged STANDALONE,
never vs WIDE ([[CompanionDominanceError]]).

Reuses the SYSTEM-OF-RECORD signal + cost math from
crypto_companion_intraday_bt.py (KELT N=20 m=2.0; EMAX 20/50; real IBKR venue
cost) and the CANONICAL reclip walker semantics from Omega
backtest/clip_reclip_sweep.py (trade-level MFE = retrig anchor; each re-armed
leg opens at current price and pays cost again; retrig=0 -> single clip).

Adds DONCH40 (the only non-EMAx strat that beats the perp cost wall --
[[DonchianPerpBreakout]]).

Two verdicts per (sym,tf,engine):
  BASE   = the trend book itself (ride signal->flip). all-6.
  CLIP   = companion reclip book, arm/gv/retrig. STANDALONE all-6.
all-6 PASS = net>0 in {whole,H1,H2,bull,bear} AND PF>1.
"""
import csv, sys
from pathlib import Path

DATA = Path.home() / "Crypto" / "backtest" / "data"

KELT_N, KELT_M = 20, 2.0
EMA_F, EMA_S = 20, 50
DONCH_N = 40
SMA_REG = 200
import os
# Operator mandate S-2026-07-04: crypto = Binance SPOT, LONG-ONLY, no perp / no IBKR.
# Default long-only; ALLOW_SHORT=1 only for legacy A/B research, never for the live roster.
ALLOW_SHORT = os.environ.get("ALLOW_SHORT", "0") == "1"

# RT cost fraction per venue (bps+slip)/1e4 -- same as crypto_companion_intraday_bt.py.
#   ladder/perp = the retired IBKR CME-micro / SQF-perp venues (drift, kept for A/B only).
#   spot        = Binance SPOT round-trip: taker 0.10%/side => 20bps RT + slip. THE live venue.
COST = {
    "ladder": {"BTC": (6+3)*1e-4,  "ETH": (8+3)*1e-4,  "SOL": (2+5)*1e-4},
    "perp":   {"BTC": (22+3)*1e-4, "ETH": (28+3)*1e-4, "SOL": (22+5)*1e-4},
    "spot":   {"BTC": (20+3)*1e-4, "ETH": (20+3)*1e-4, "SOL": (20+5)*1e-4},
}

# per-engine arm gate (excursion-scaled so the gate is NOT inert -- measured from
# WIDE peak-fav distribution: EMAX p50~4%, KELT p50~0.16%, DONCH breakout-sparse).
ARM = {"EMAX": 0.03, "KELT": 0.01, "DONCH": 0.01}


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


def build_targets(sym, mode, tf):
    """Return (ms,c,tgt[]) with tgt in {-1,0,1}. DONCH is a stateful channel
    breakout (hold prior side until opposite N-channel break)."""
    ms,h,l,c = load(sym, tf); n=len(c); tgt=[0]*n
    if mode == "DONCH":
        state=0
        for i in range(n):
            if i < DONCH_N: tgt[i]=0; continue
            hh=max(h[i-DONCH_N:i]); ll=min(l[i-DONCH_N:i])
            if c[i] > hh: state=1
            elif c[i] < ll: state=-1 if ALLOW_SHORT else 0
            tgt[i]=state
    else:
        f = kelt_target if mode=="KELT" else emax_target
        for i in range(n):
            tgt[i] = kelt_target(h,l,c,i) if mode=="KELT" else emax_target(c,i)
    return ms,c,tgt


def trades_of(ms,c,tgt):
    n=len(c); s200=sma(c,SMA_REG); out=[]; i=0
    while i<n:
        if tgt[i]==0: i+=1; continue
        s=tgt[i]; ent=i; j=i+1
        while j<n and tgt[j]==s: j+=1
        flip=min(j,n-1)
        bull=(s200[ent] is not None) and (c[ent] > s200[ent])
        out.append((ent,flip,s,bull)); i=j
    return out


def base_pnl(c, ent, flip, s, cost):
    return s*(c[flip]/c[ent]-1.0) - cost


def reclip_legs(c, ent, flip, s, arm, gv, retrig, cost):
    """Canonical reclip walker (ported from Omega clip_reclip_sweep.reclip_legs).
    Returns list of leg pnl (net of cost). retrig=0 -> single clip.
    FAITHFUL to live stall_accountant: the companion OPENS with every real trade
    (pos[key] set on first sight, before arming) -> an un-armed trade is NOT
    dropped, it rides to flip and books its full (often losing) pnl. Live also
    has a cold-loss cut that BOUNDS that loss; omitting it here is the
    conservative floor (matches the daily system-of-record which books at flip)."""
    legs=[]; mode="active"
    leg_ent=c[ent]; leg_peak=0.0; trade_peak=0.0; clip_anchor=None; leg_last=flip
    k=ent+1
    while k<=flip:
        px=c[k]; fav_trade=s*(px/c[ent]-1.0)
        if fav_trade>trade_peak: trade_peak=fav_trade
        if mode=="active":
            leg_last=k
            fav=s*(px/leg_ent-1.0)
            if fav>leg_peak: leg_peak=fav
            if leg_peak>=arm and fav<=leg_peak*(1.0-gv):
                legs.append(fav-cost); clip_anchor=trade_peak
                if retrig and retrig>0: mode="clipped"
                else: return legs
        elif mode=="clipped":
            if clip_anchor is not None and fav_trade > clip_anchor*(1.0+retrig):
                mode="active"; leg_ent=px; leg_peak=0.0; leg_last=k
        k+=1
    if mode=="active":
        # open leg at parent flip -> book full ride (armed or not; un-armed = the cold loser)
        legs.append(s*(c[leg_last]/leg_ent-1.0)-cost)
    return legs


def metrics(rows):
    """rows = list of (exit_ms, pnl, bull). Return dict of all-6 nets + PF/MAR."""
    if not rows: return None
    rows=sorted(rows, key=lambda x:x[0]); pnl=[r[1] for r in rows]
    wins=sum(p for p in pnl if p>0); loss=sum(-p for p in pnl if p<0)
    pf=wins/loss if loss>0 else float('inf')
    eq=0.0; pk=0.0; dd=0.0
    for p in pnl: eq+=p; pk=max(pk,eq); dd=min(dd,eq-pk)
    net=sum(pnl); mar=(net/abs(dd)) if dd<0 else float('inf')
    m=len(rows)//2
    h1=sum(r[1] for r in rows[:m]); h2=sum(r[1] for r in rows[m:])
    bull=sum(r[1] for r in rows if r[2]); bear=sum(r[1] for r in rows if not r[2])
    passed = (net>0 and h1>0 and h2>0 and bull>0 and bear>0 and pf>1)
    return dict(n=len(pnl), pf=pf, net=100*net, dd=100*dd, mar=mar,
                h1=100*h1, h2=100*h2, bull=100*bull, bear=100*bear, passed=passed)


def fmt(tag, m):
    if m is None: return f"{tag:28s} n=0"
    v="PASS" if m["passed"] else "----"
    pf = "inf" if m["pf"]==float('inf') else f"{m['pf']:.2f}"
    mar= "inf" if m["mar"]==float('inf') else f"{m['mar']:.2f}"
    return (f"{tag:28s} n={m['n']:4d} net={m['net']:8.1f}% PF={pf:>5s} "
            f"MAR={mar:>5s} DD={m['dd']:7.1f}% H1={m['h1']:7.1f} H2={m['h2']:7.1f} "
            f"bull={m['bull']:7.1f} bear={m['bear']:7.1f}  {v}")


def main():
    scen = sys.argv[1] if len(sys.argv)>1 else "ladder"
    costmap = COST[scen]
    # roster of engines to test per TF (1h is mostly cost-wall dead -> tested but expected weak)
    engines = ("EMAX","KELT","DONCH")
    syms = ("BTC","ETH","SOL")
    print(f"=== INTRADAY engine+reclip BT | cost={scen} | gv=0.50 | arm(EMAX={ARM['EMAX']} KELT={ARM['KELT']} DONCH={ARM['DONCH']}) ===")
    for tf in ("4h","1h"):
        print(f"\n############## {tf} ##############")
        # book aggregation per engine across syms
        for eng in engines:
            base_rows=[]; clip0_rows=[]; clip5_rows=[]
            for sym in syms:
                ms,c,tgt = build_targets(sym,eng,tf)
                trs = trades_of(ms,c,tgt); cost=costmap[sym]; arm=ARM[eng]
                for ent,flip,s,bull in trs:
                    base_rows.append((ms[flip], base_pnl(c,ent,flip,s,cost), bull))
                    for retrig,bucket in ((0.0,clip0_rows),(0.05,clip5_rows)):
                        for lp in reclip_legs(c,ent,flip,s,arm,0.50,retrig,cost):
                            bucket.append((ms[flip], lp, bull))
            print(f"--- {eng} (BTC/ETH/SOL {tf}) ---")
            print("  "+fmt("BASE trend book", metrics(base_rows)))
            print("  "+fmt("CLIP arm/gv50 retrig0", metrics(clip0_rows)))
            print("  "+fmt("CLIP arm/gv50 retrig5%", metrics(clip5_rows)))


if __name__ == "__main__":
    main()
