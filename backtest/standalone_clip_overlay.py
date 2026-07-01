#!/usr/bin/env python3
"""
STANDALONE-CLIP OVERLAY — engine-agnostic companion viability harness (S-2026-07-01).

The companion clip is a SEPARATE INDEPENDENT engine (never modifies its parent; the
parent rides WIDE regardless). So this harness judges a clip book ON ITS OWN MERIT —
NEVER against WIDE. See Memory-Omega/wiki/entities/CompanionDominanceError.md and
auto-memory feedback-companion-independent-engine.

INPUT: a per-trade path CSV emitted by ANY engine's FAITHFUL backtest. One row per
(trade, bar-from-entry):
    trade_id,seq,exit_ms,dir,entry_px,px,atr_pct,bull,cost_rt
  - trade_id : integer, groups rows of one trade
  - seq      : 0..K bar index within the trade (0 = entry bar)
  - exit_ms  : ms timestamp of THIS bar (used to order the standalone book by clip time)
  - dir      : +1 long / -1 short
  - entry_px : entry price (constant per trade)
  - px       : close at this bar
  - atr_pct  : ATR%/price at entry (constant per trade; blank if unknown -> ATR clips skipped)
  - bull     : 1 if regime bull at entry else 0
  - cost_rt  : round-trip cost as a fraction (constant per trade)

The harness scans each trade's path, applies each clip config, and books the clipped
P&L as its OWN trade at the clip time. Reports standalone metrics + STANDALONE verdict.

VERDICT (standalone, NOT vs-WIDE): a clip config is a viable additive book if
  net>0 AND PF>1 AND both WF halves net>0 AND both regimes net>=0.
"""
import csv, sys
from collections import defaultdict

def load_paths(csvf):
    trades=defaultdict(list)
    with open(csvf) as f:
        r=csv.DictReader(f)
        for row in r:
            trades[int(row["trade_id"])].append(row)
    out=[]
    for tid,rows in trades.items():
        rows.sort(key=lambda x:int(x["seq"]))
        d=int(rows[0]["dir"]); ent=float(rows[0]["entry_px"])
        ap=rows[0].get("atr_pct","")
        atr_pct=float(ap) if ap not in ("","None",None) else None
        bull=int(rows[0]["bull"]); cost=float(rows[0]["cost_rt"])
        path=[(int(x["seq"]),int(x["exit_ms"]),float(x["px"])) for x in rows]
        out.append(dict(tid=tid,dir=d,ent=ent,atr=atr_pct,bull=bull,cost=cost,path=path))
    return out

def clip_pnl(t,cfg):
    """returns (exit_ms, pnl) for the clip book on this trade. If never clips -> exits
    at the last bar (companion holds to the same flip the parent would)."""
    d=t["dir"]; ent=t["ent"]; cost=t["cost"]; peak=0.0; since=0
    rev_atr=(cfg["rev_atr_k"]*t["atr"]) if (cfg.get("rev_atr_k") and t["atr"]) else None
    last=t["path"][-1]
    for seq,ms,px in t["path"]:
        if seq==0: continue
        fav=d*(px/ent-1.0)
        if fav>peak: peak=fav; since=0
        else: since+=1
        if peak<cfg["gate"]: continue
        hit = ((cfg.get("stall") and since>=cfg["stall"]) or
               (cfg.get("rev_frac") and fav<=peak*(1.0-cfg["rev_frac"])) or
               (rev_atr is not None and fav<=peak-rev_atr))
        if hit:
            return ms, d*(px/ent-1.0)-cost
    return last[1], d*(last[2]/ent-1.0)-cost

def metrics(rows):
    if not rows: return dict(n=0,pf=0,net=0,dd=0,mar=0)
    rows=sorted(rows,key=lambda x:x[0]); pnl=[r[1] for r in rows]
    w=sum(p for p in pnl if p>0); l=sum(-p for p in pnl if p<0)
    pf=w/l if l>0 else float('inf')
    eq=pk=dd=0.0
    for p in pnl:
        eq+=p; pk=max(pk,eq); dd=min(dd,eq-pk)
    net=100*sum(pnl); ddp=100*dd
    return dict(n=len(pnl),pf=pf,net=net,dd=ddp,mar=(net/abs(ddp) if ddp<0 else float('inf')))

def halves(rows):
    rows=sorted(rows,key=lambda x:x[0]); m=len(rows)//2
    return rows[:m],rows[m:]

CFGS=[
    ("default5%",   dict(gate=0.015,stall=None,rev_frac=0.05,rev_atr_k=None)),
    ("G5|revFrac30",dict(gate=0.05, stall=None,rev_frac=0.30,rev_atr_k=None)),
    ("G5|revATR2",  dict(gate=0.05, stall=None,rev_frac=None,rev_atr_k=2)),
    ("G10|revATR2", dict(gate=0.10, stall=None,rev_frac=None,rev_atr_k=2)),
    ("G10|revFrac50",dict(gate=0.10,stall=None,rev_frac=0.50,rev_atr_k=None)),
    ("G5|stall5",   dict(gate=0.05, stall=5,   rev_frac=None,rev_atr_k=None)),
]

def run(csvf, label):
    trades=load_paths(csvf)
    print(f"\n=== {label}  ({len(trades)} trades) ===")
    print(f"  {'config':14s} {'net%':>8s} {'PF':>5s} {'maxDD%':>8s} {'MAR':>6s} {'WF-H1':>7s} {'WF-H2':>7s} {'bull':>7s} {'bear':>7s}  verdict")
    any_pass=False
    for name,cfg in CFGS:
        book=[clip_pnl(t,cfg) for t in trades]
        # attach regime for split
        reg=[(clip_pnl(t,cfg)[0], clip_pnl(t,cfg)[1], t["bull"]) for t in trades]
        m=metrics([(x[0],x[1]) for x in reg])
        bull=metrics([(x[0],x[1]) for x in reg if x[2]])
        bear=metrics([(x[0],x[1]) for x in reg if not x[2]])
        h1,h2=halves([(x[0],x[1]) for x in reg]); m1=metrics(h1); m2=metrics(h2)
        ok = m["net"]>0 and m["pf"]>1 and m1["net"]>0 and m2["net"]>0 and bull["net"]>=0 and bear["net"]>=0
        any_pass=any_pass or ok
        print(f"  {name:14s} {m['net']:8.0f} {m['pf']:5.2f} {m['dd']:8.0f} {m['mar']:6.2f} "
              f"{m1['net']:7.0f} {m2['net']:7.0f} {bull['net']:7.0f} {bear['net']:7.0f}  {'PASS' if ok else '.'}")
    print(f"  -> STANDALONE VIABLE: {'YES' if any_pass else 'NO'} (>=1 config net+/PF>1/WF-both+/both-regimes+)")
    return any_pass

if __name__=="__main__":
    if len(sys.argv)<3:
        print("usage: standalone_clip_overlay.py <path_csv> <engine_label>"); sys.exit(1)
    run(sys.argv[1], sys.argv[2])
