#!/usr/bin/env python3
"""
ETH UpJump COMPANION clip sweep — FAITHFUL to the live native
UpJumpCompanionEngine.observe() (chimera-direct josgp1, e5ad15c).

Parent trades come from ibkrcrypto_bt --dump-trades UpJump8 (the SAME binary +
code path that prints FULL n=65 net+391% -> parent fidelity gate). This overlay
replicates observe() line-for-line:
  arm at peak (mfe% >= arm_pct) -> clip on STALL (N bars no new fav high)
  OR REVERSAL (fav <= peak*(1-rev_gb)) -> RECLIP (re-arm on new fav high past
  prior_peak*(1+reclip)). gross_bp=(exit/entry-1)*1e4 from the PARENT entry each
  clip; net_bp=gross-20 (0.20% RT Binance taker). Parent flat -> ENGINE_EXIT book.
Close-based (roster convention: STALL is H1; per-tick leaves nets unchanged).

STANDALONE all-6 gate (companion judged on its OWN book, never vs WIDE):
  net>0 AND PF>1 AND WF-H1>0 AND WF-H2>0 AND bull>0 AND bear>0.
Goal: smallest rev_gb (least giveback) + shortest stall that still passes all-6.
"""
import csv, subprocess, os, statistics
from pathlib import Path

HERE = Path(__file__).resolve().parent
CSVF = HERE / "data" / "ETHUSDT_1h.csv"
BT   = HERE / "ibkrcrypto_bt"
TF_S = 3600
RT_BP = 20.0          # cfg.round_trip_bp (0.20% RT)
SMA_REG_BARS = 200*24 # 200-day regime SMA in 1h bars

# ---- load 1h bars -------------------------------------------------------------
ts=[]; o=[]; h=[]; l=[]; c=[]
with open(CSVF) as f:
    r=csv.reader(f); next(r)
    for row in r:
        t=int(row[0])
        if t < 1_000_000_000_000: t*=1000
        ts.append(t); o.append(float(row[1])); h.append(float(row[2]))
        l.append(float(row[3])); c.append(float(row[4]))
N=len(ts)
idx_of={t:i for i,t in enumerate(ts)}
# regime SMA (200d) on closes
sma=[None]*N; run=0.0
for i in range(N):
    run+=c[i]
    if i>=SMA_REG_BARS: run-=c[i-SMA_REG_BARS]
    if i>=SMA_REG_BARS-1: sma[i]=run/SMA_REG_BARS

# ---- parent trades from the faithful binary ----------------------------------
def load_trades():
    env=dict(os.environ, DUMP_TRADES="1", COSTBPS="18", BT_TF_MS="3600000")
    p=subprocess.run([str(BT),"ETH",str(CSVF),"--dump-trades","UpJump8"],
                     capture_output=True,text=True,env=env)
    tr=[]
    for ln in p.stderr.splitlines():
        if ln.startswith("TRADE"):
            _,ets,xts,epx,dirc,size=ln.split()
            tr.append((int(ets),int(xts),float(epx)))
    return tr
TRADES=load_trades()

# ---- faithful observe() overlay over ONE parent trade ------------------------
def clips_for_trade(ets, xts, arm, stall_bars, rev_gb, reclip):
    """Replicate UpJumpCompanionEngine.observe() close-by-close across the hold.
       Returns list of (exit_ts, net_pct, gross_pct) clip records for this trade."""
    ei=idx_of.get(ets); xi=idx_of.get(xts)
    if ei is None or xi is None or xi<=ei: return []
    entry_px=o[ei]                      # parent enters at open[ei]
    recs=[]
    # engine state (mirror the .hpp fields). arm/stall/reversal gates use fav from
    # PARENT entry (as observe() does); P&L books PER LEG (leg basis re-set on reclip)
    # -> "banks each leg" (vault). leg_entry starts at parent entry.
    open_=False; clipped=False; prior_peak=0.0; mfe=0.0; ext_bar=0; open_bar=0
    leg_entry=[entry_px]
    def do_close(exit_px, bar, reason):
        gross_bp=(exit_px/leg_entry[0]-1.0)*1e4
        net_bp=gross_bp-RT_BP
        recs.append((reason, net_bp/100.0, gross_bp/100.0))   # -> percent
    # walk each completed H1 bar close within the hold [ei .. xi-1]; parent flat at xi
    for i in range(ei, xi):
        bar=ts[i]//(TF_S*1000)
        cur=c[i]
        fav=(cur-entry_px)/entry_px*100.0
        if clipped:
            if reclip>0.0 and prior_peak>0.0 and fav>prior_peak*(1.0+reclip):
                clipped=False           # re-arm fresh leg; new leg basis = current px
                leg_entry[0]=cur
            else:
                continue
        if not open_:
            open_=True; open_bar=bar; mfe=fav; ext_bar=bar
        if fav>mfe+1e-9:
            mfe=fav; ext_bar=bar
        stall=int(bar-ext_bar)
        armed=mfe>=arm
        if armed and stall_bars>0 and stall>=stall_bars:
            prior_peak=mfe; do_close(cur,bar,"STALL"); clipped=True; open_=False; continue
        if armed and rev_gb>0.0 and fav<=mfe*(1.0-rev_gb):
            prior_peak=mfe; do_close(cur,bar,"REVERSAL"); clipped=True; open_=False; continue
    # parent flat -> book any still-open companion as ENGINE_EXIT at last close before flat
    if open_ and not clipped:
        last=c[xi-1] if xi-1>=ei else o[ei]
        bar=ts[xi-1]//(TF_S*1000)
        do_close(last,bar,"ENGINE_EXIT")
    return recs

# ---- run a full book (all 65 parent trades) for one config -------------------
def run_book(arm, stall_bars, rev_gb, reclip):
    rows=[]   # (exit_ts, net_pct, bull)
    for ets,xts,epx in TRADES:
        ei=idx_of.get(ets)
        bull = (ei is not None and sma[ei] is not None and c[ei]>sma[ei])
        for reason,net_pct,gross_pct in clips_for_trade(ets,xts,arm,stall_bars,rev_gb,reclip):
            rows.append((xts, net_pct, gross_pct, bull))
    return rows

def metrics(rows):
    if not rows: return dict(n=0,net=0,pf=0,h1=0,h2=0,bull=0,bear=0,passed=False)
    net=sum(r[1] for r in rows)
    gw=sum(r[1] for r in rows if r[1]>0); gl=-sum(r[1] for r in rows if r[1]<0)
    pf=gw/gl if gl>0 else (999 if gw>0 else 0)
    ex=sorted(r[0] for r in rows); mid=ex[len(ex)//2]
    h1=sum(r[1] for r in rows if r[0]<mid); h2=sum(r[1] for r in rows if r[0]>=mid)
    bull=sum(r[1] for r in rows if r[3]); bear=sum(r[1] for r in rows if not r[3])
    passed=(net>0 and pf>1 and h1>0 and h2>0 and bull>0 and bear>0)
    return dict(n=len(rows),net=net,pf=pf,h1=h1,h2=h2,bull=bull,bear=bear,passed=passed)

def show(tag,arm,stall_bars,rev_gb,reclip):
    m=metrics(run_book(arm,stall_bars,rev_gb,reclip))
    flag="PASS" if m["passed"] else "fail"
    print(f"  {tag:26s} n={m['n']:3d} net={m['net']:+7.1f}% PF={m['pf']:4.2f} "
          f"H1={m['h1']:+6.1f} H2={m['h2']:+6.1f} bull={m['bull']:+6.1f} bear={m['bear']:+6.1f}  [{flag}]")
    return m

print(f"parent trades: {len(TRADES)} (fidelity: binary FULL n=65 net+391%)")
print("\n=== FIDELITY CHECK vs vault roster (ETH arm5 rev50 reclip -> ~+211) ===")
show("arm5 S6 R50 reclip5", 5, 6, 0.50, 0.05)
show("arm5 Soff R50 reclip5", 5, 0, 0.50, 0.05)
show("arm2 S6 R50 reclip5 (LIVE)", 2, 6, 0.50, 0.05)

print("\n=== SWEEP: tighten reversal giveback toward ZERO (arm2, stall6, reclip5) ===")
best=None
for rg in [0.50,0.40,0.35,0.30,0.25,0.20,0.15,0.10,0.05]:
    m=show(f"arm2 S6 R{int(rg*100):02d} reclip5", 2, 6, rg, 0.05)
    if m["passed"] and (best is None or rg<best[0]): best=(rg,m)

print("\n=== SWEEP: shorten STALL (stagnation) toward fast, rev fixed 0.30 ===")
for sb in [6,5,4,3,2,1]:
    show(f"arm2 S{sb} R30 reclip5", 2, sb, 0.30, 0.05)

print("\n=== STALL-ONLY (pure stagnation, no giveback wait) arm2 reclip5 ===")
for sb in [6,4,3,2,1]:
    show(f"arm2 S{sb} Roff reclip5", 2, sb, 0.0, 0.05)

if best:
    print(f"\nTIGHTEST PASSING reversal giveback (arm2 S6 reclip5): R{int(best[0]*100)} "
          f"net={best[1]['net']:+.1f}% PF={best[1]['pf']:.2f}")
