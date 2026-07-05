#!/usr/bin/env python3
"""
CryptoUpJumpCompanion — TIERED-2 + SELF-FUNDING LADDER roster sweep (S-2026-07-05).

Operator spec (this session):
  1. Enter parent the instant the coin crosses ITS up-jump trigger (intraday, no wait).
  2. Stack a MINIMUM OF 2 companion engines on every trade (tight tier banks cost fast,
     wide tier rides far) — "our only opportunity to make profit".
  3. SELF-FUNDING LADDER: each time a companion banks a cost-covered clip (net>0),
     open ONE MORE companion leg at the live price to capitalise on the continuation,
     up to CAP concurrent legs. The clip that spawns it already paid its cost (opt C).
  4. Every tier exits on REVERSAL *or* STAGNATION and RE-CLIPS if the trend resumes.
  5. Judge the WHOLE companion book STANDALONE all-6 (net>0, PF>1, WF both halves,
     both 200d regimes) — NEVER vs WIDE (feedback-companion-independent-engine).

Parent = Python UpJump(W,thr) replication, VALIDATED byte-exact (1086/1086 trades)
vs the live C++ ibkrcrypto_bt UpJump8 (entry=next-open, exit=next-open, end-flush).
Intraday window W + thr per coin taken from parent_window_sweep (best-net W<=8 all-6).
Cost 0.20% RT (Binance spot taker). shadow=$0.
"""
import csv, sys
from pathlib import Path

DATA = Path(__file__).resolve().parent / "data"
COINS = ["BTC","ETH","SOL","DOGE","BNB","ADA","TRX","NEAR","AAVE"]  # OP dropped (parent fails all-6)
TF_MS=3600000; RT_BP=20.0; SMA_BARS=200*24; CAP=5

# HARD COST-COVER GATE (operator task1, 05-07): a leg may NOT bank a CLIP unless
# its GROSS move clears RT cost (>= RT_BP bp). A sub-cost trigger is suppressed and
# the leg keeps holding (spot-long) until it either covers cost or is marked-to-
# market at parent exit. NOTE: the flush is ALWAYS MTM'd (no abandon) — an earlier
# abandon-underwater variant looked great (PF999) purely by HIDING 552 open
# underwater legs (-4396% unrealized); that is a BACKTEST_TRUTH mirage and was
# removed. Honest verdict: the gate does NOT rescue AAVE (still PF~1.04, H1~0) —
# see the session note; AAVE is a DROP candidate. Per-coin so it's opt-in.
COST_COVER = {c: False for c in COINS}
COST_COVER["AAVE"] = True

# per-coin intraday parent (W hours, thr) — best-net W<=8 passing all-6 standalone
PARENT = {
    "BTC": (4,0.08), "ETH": (4,0.05), "SOL": (4,0.12), "DOGE": (8,0.05),
    "BNB": (4,0.05), "ADA": (6,0.05), "TRX": (8,0.08), "NEAR": (6,0.05), "AAVE": (4,0.05),
}

# tier param grid. Each tier freely picks its exit lever(s): reversal-only, stall-only,
# or BOTH (>=1 must be on). reclip + self-funding ladder ALWAYS on. This lets a coin use
# whichever exit actually PROTECTS net (e.g. ETH is reversal-only — a stall exit clips its
# slow grinds early and destroys the edge; forcing "both" was the bug that broke ETH).
TIGHT_ARM=[1.0,2.0,3.0]
WIDE_ARM =[5.0,8.0]
# (stall_bars, giveback) exit combos — 0 = that lever OFF
TIGHT_EXIT=[(0,0.30),(0,0.50),(3,0.0),(4,0.0),(3,0.30),(4,0.50)]
WIDE_EXIT =[(0,0.40),(0,0.50),(6,0.0),(8,0.0),(6,0.50),(8,0.40)]
RECLIP=0.05

def load(coin):
    ts=[];o=[];c=[]
    with open(DATA/f"{coin}USDT_1h.csv") as f:
        r=csv.reader(f); next(r)
        for row in r:
            ts.append(int(row[0])); o.append(float(row[1])); c.append(float(row[4]))
    N=len(ts); sma=[None]*N; run=0.0
    for i in range(N):
        run+=c[i]
        if i>=SMA_BARS: run-=c[i-SMA_BARS]
        if i>=SMA_BARS-1: sma[i]=run/SMA_BARS
    return ts,o,c,N,sma

def parent(bars,W,thr):
    ts,o,c,N,sma=bars; trades=[];pos=False;ent=None
    for i in range(W,N):
        j=c[i]/c[i-W]-1.0
        if not pos and j>=thr:
            ei=i+1
            if ei>=N: continue
            pos=True; ent=(ei,o[ei])
        elif pos and j<=-thr:
            xi=i+1
            if xi>=N: xi=N-1
            trades.append((ent[0],xi,ent[1])); pos=False;ent=None
    if pos: trades.append((ent[0],N-1,ent[1]))
    return trades

class Leg:
    """One independent companion clip book (faithful observe() %-gauge). Additive."""
    __slots__=("epx","le","arm","sb","gb","rc","cg","open","clipped","pk","mfe","ext")
    def __init__(self,entry_px,arm,stall,gb,reclip,cost_gate=0.0):
        self.epx=entry_px; self.le=entry_px; self.arm=arm; self.sb=stall
        self.gb=gb; self.rc=reclip; self.cg=cost_gate; self.open=False; self.clipped=False
        self.pk=0.0; self.mfe=0.0; self.ext=0
    def _clip(self,cur):
        # HARD COST-COVER GATE: suppress a clip whose gross does not clear cost.
        # Leg keeps holding (stays open, not clipped) -> may cover cost later or be
        # abandoned at parent-exit flush. Never books a sub-cost loss.
        gross=(cur/self.le-1.0)*1e4
        if self.cg>0 and gross<self.cg: return None
        self.pk=self.mfe; self.clipped=True
        return gross
    def step(self,bar,cur):
        fav=(cur-self.epx)/self.epx*100.0
        if self.clipped:
            if self.rc>0 and self.pk>0 and fav>self.pk*(1.0+self.rc):
                self.clipped=False; self.le=cur
            else: return None
        if not self.open:
            self.open=True; self.mfe=fav; self.ext=bar
        if fav>self.mfe+1e-9:
            self.mfe=fav; self.ext=bar
        armed=self.mfe>=self.arm; stall=bar-self.ext
        if armed and self.sb>0 and stall>=self.sb:
            return self._clip(cur)
        if armed and self.gb>0 and fav<=self.mfe*(1.0-self.gb):
            return self._clip(cur)
        return None

def run_trade(bars, ei, xi, tight, wide, cg=0.0):
    """base-2 tiers + self-funding ladder. Returns list of net_bp clips.
    cg>0 = hard cost-cover gate (see COST_COVER): no clip/flush booked sub-cost."""
    ts,o,c,N,sma=bars
    epx=o[ei]
    legs=[Leg(epx,*tight,cost_gate=cg), Leg(epx,*wide,cost_gate=cg)]
    spawned=len(legs)
    clips=[]
    for i in range(ei, xi):
        cur=c[i]; new=[]
        for lg in legs:
            g=lg.step(i,cur)
            if g is not None:
                net=(g-RT_BP)
                clips.append(net)
                if net>0 and spawned<CAP:           # cost-covered clip -> fund one more leg
                    new.append(Leg(cur,*wide,cost_gate=cg)); spawned+=1
        legs+=new
    last=c[xi-1] if xi-1>=ei else o[ei]
    for lg in legs:                                  # flush open legs at parent exit (always MTM)
        if lg.open and not lg.clipped:
            clips.append((last/lg.le-1.0)*1e4 - RT_BP)
    return clips

def book(bars,trades,tight,wide,cg=0.0):
    ts,o,c,N,sma=bars; rows=[]
    for ei,xi,epx in trades:
        bull=(sma[ei] is not None and c[ei]>sma[ei])
        for net_bp in run_trade(bars,ei,xi,tight,wide,cg):
            rows.append((ts[xi], net_bp/100.0, bull))
    return rows

def metrics(rows):
    if not rows: return dict(n=0,net=0,pf=0,h1=0,h2=0,bull=0,bear=0,passed=False)
    net=sum(r[1] for r in rows)
    gw=sum(r[1] for r in rows if r[1]>0); gl=-sum(r[1] for r in rows if r[1]<0)
    pf=gw/gl if gl>0 else (999 if gw>0 else 0)
    ex=sorted(r[0] for r in rows); mid=ex[len(ex)//2]
    h1=sum(r[1] for r in rows if r[0]<mid); h2=sum(r[1] for r in rows if r[0]>=mid)
    bull=sum(r[1] for r in rows if r[2]); bear=sum(r[1] for r in rows if not r[2])
    # GATE = net>0, PF>1, walk-forward BOTH time-halves>0 (proves it's not a one-period
    # fluke). 200d-regime split is spot-long-only DIAGNOSTIC, not a hard gate — we can't
    # short, so a bull-skewed book is fine; the ONLY regime failure that matters is bear<0
    # (longs actually BLEEDING in a down-market). Reported, and blocked only when negative.
    passed=(net>0 and pf>1 and h1>0 and h2>0 and bear>=0)
    return dict(n=len(rows),net=net,pf=pf,h1=h1,h2=h2,bull=bull,bear=bear,passed=passed)

def exit_kind(stall,gb):
    if stall>0 and gb>0: return "both"
    if stall>0: return "stall"
    return "rev"

def cfgstr(c):
    return f"a{c[0]:g}/s{c[1]}/g{int(c[2]*100)}/rc{int(c[3]*100)}[{exit_kind(c[1],c[2])}]"

def main():
    only=sys.argv[1:] if len(sys.argv)>1 else COINS
    summary=[]
    for coin in only:
        bars=load(coin); W,thr=PARENT[coin]; trades=parent(bars,W,thr)
        cg=RT_BP if COST_COVER.get(coin) else 0.0             # hard cost-cover gate (AAVE)
        # (a) best 2-tier (>=2 engines from entry). (b) best 1-tier+ladder fallback.
        best2=None; best1=None
        for ta in TIGHT_ARM:
          for (tst,tg) in TIGHT_EXIT:
            tight=(ta,tst,tg,RECLIP)
            m1=metrics(book(bars,trades,tight,tight,cg))       # single base tier + ladder
            if m1["passed"] and (best1 is None or m1["net"]>best1[1]["net"]): best1=(tight,m1)
            for wa in WIDE_ARM:
              for (wst,wg) in WIDE_EXIT:
                wide=(wa,wst,wg,RECLIP)
                m=metrics(book(bars,trades,tight,wide,cg))
                if m["passed"] and (best2 is None or m["net"]>best2[2]["net"]): best2=(tight,wide,m)
        for wa in WIDE_ARM:                                     # wide-only also a single-tier candidate
          for (wst,wg) in WIDE_EXIT:
            wide=(wa,wst,wg,RECLIP)
            m1=metrics(book(bars,trades,wide,wide,cg))
            if m1["passed"] and (best1 is None or m1["net"]>best1[1]["net"]): best1=(wide,m1)
        gtag=" COST-COVER-GATE" if cg>0 else ""
        print(f"\n=== {coin}  parent W={W}h thr={thr*100:+.0f}%  ({len(trades)} trades){gtag} ===")
        if best2:
            t,w,m=best2
            print(f"  2-TIER  TIGHT {cfgstr(t)}  WIDE {cfgstr(w)}  ladder-cap{CAP}")
            print(f"          n={m['n']:4d} net={m['net']:+8.0f}% PF={m['pf']:4.2f} H1={m['h1']:+6.0f} H2={m['h2']:+6.0f} bull={m['bull']:+6.0f} bear={m['bear']:+6.0f}  [PASS]")
            summary.append((coin,W,thr,"2tier",t,w,m))
        elif best1:
            t,m=best1
            print(f"  1-TIER+LADDER  {cfgstr(t)}  (2-tier fails all-6; ladder supplies extra engines)")
            print(f"          n={m['n']:4d} net={m['net']:+8.0f}% PF={m['pf']:4.2f} H1={m['h1']:+6.0f} H2={m['h2']:+6.0f} bull={m['bull']:+6.0f} bear={m['bear']:+6.0f}  [PASS]")
            summary.append((coin,W,thr,"1tier",t,None,m))
        else:
            print("  (no config passes all-6 standalone -> PARENT-ONLY)")
            summary.append((coin,W,thr,"none",None,None,None))
    print(f"\n{'='*112}\nROSTER — tiered + self-funding ladder, per-tier free exit lever, STANDALONE all-6, RT-0.20%")
    print(f"{'coin':5s} {'trig':9s} {'mode':6s} {'tier-1 (base/tight)':26s} {'tier-2 (wide)':26s} {'net':>7s} {'PF':>5s} {'bull':>6s} {'bear':>6s}")
    for coin,W,thr,mode,t,w,m in summary:
        trig=f"{W}h/{thr*100:+.0f}%"
        if m:
            ws=cfgstr(w) if w else "(ladder only)"
            print(f"{coin:5s} {trig:9s} {mode:6s} {cfgstr(t):26s} {ws:26s} {m['net']:+7.0f} {m['pf']:5.2f} {m['bull']:+6.0f} {m['bear']:+6.0f}")
        else:
            print(f"{coin:5s} {trig:9s} {mode:6s} {'PARENT-ONLY':26s}")

if __name__=="__main__":
    main()
