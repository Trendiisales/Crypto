#!/usr/bin/env python3
"""Export the LOCKED ladder roster + parent trades + reference clip stream so the
native C++ UpJumpLadderCompanion can be validated byte-exact against the python
crypto_upjump_tiered_ladder_sweep.py Leg/run_trade path (deploy rule #4).

Writes (in this dir):
  roster_cfg.csv    coin,W,thr,cost_gate_bp,t_arm,t_stall,t_gb,w_arm,w_stall,w_gb,reclip,cap
  parent_trades.csv coin,ei,xi,epx
  ref_clips.csv     coin,trade_idx,clip_seq,net_bp        (python reference, 4dp)
"""
import csv
import crypto_upjump_tiered_ladder_sweep as M

ROSTER = ["BTC","ETH","SOL","DOGE","BNB","ADA","TRX","NEAR"]   # AAVE DROPPED (task1: PF1.04, H1~0 noise)

def select_best(coin, bars, trades, cg):
    """Reproduce the sweep's best-2-tier / best-1-tier selection for this coin."""
    best2=None; best1=None
    for ta in M.TIGHT_ARM:
      for (tst,tg) in M.TIGHT_EXIT:
        tight=(ta,tst,tg,M.RECLIP)
        m1=M.metrics(M.book(bars,trades,tight,tight,cg))
        if m1["passed"] and (best1 is None or m1["net"]>best1[1]["net"]): best1=(tight,m1)
        for wa in M.WIDE_ARM:
          for (wst,wg) in M.WIDE_EXIT:
            wide=(wa,wst,wg,M.RECLIP)
            m=M.metrics(M.book(bars,trades,tight,wide,cg))
            if m["passed"] and (best2 is None or m["net"]>best2[2]["net"]): best2=(tight,wide,m)
    for wa in M.WIDE_ARM:
      for (wst,wg) in M.WIDE_EXIT:
        wide=(wa,wst,wg,M.RECLIP)
        m1=M.metrics(M.book(bars,trades,wide,wide,cg))
        if m1["passed"] and (best1 is None or m1["net"]>best1[1]["net"]): best1=(wide,m1)
    if best2: return best2[0], best2[1]                 # (tight, wide)
    if best1: return best1[0], best1[0]                 # 1-tier: both tiers identical
    return None, None

def main():
    rc=open("roster_cfg.csv","w",newline=""); rcw=csv.writer(rc)
    rcw.writerow(["coin","W","thr","cost_gate_bp","t_arm","t_stall","t_gb","w_arm","w_stall","w_gb","reclip","cap"])
    pt=open("parent_trades.csv","w",newline=""); ptw=csv.writer(pt)
    ptw.writerow(["coin","ei","xi","epx"])
    cl=open("ref_clips.csv","w",newline=""); clw=csv.writer(cl)
    clw.writerow(["coin","trade_idx","clip_seq","net_bp"])

    for coin in ROSTER:
        bars=M.load(coin); W,thr=M.PARENT[coin]; trades=M.parent(bars,W,thr)
        cg=M.RT_BP if M.COST_COVER.get(coin) else 0.0
        tight,wide=select_best(coin,bars,trades,cg)
        assert tight is not None, f"{coin} has no passing config"
        ta,tst,tg,_=tight; wa,wst,wg,_=wide
        rcw.writerow([coin,W,thr,f"{cg:g}",f"{ta:g}",tst,f"{tg:g}",f"{wa:g}",wst,f"{wg:g}",f"{M.RECLIP:g}",M.CAP])
        for ti,(ei,xi,epx) in enumerate(trades):
            ptw.writerow([coin,ei,xi,f"{bars[1][ei]:.10f}"])   # bars[1]=o ; epx=o[ei]
            for seq,net in enumerate(M.run_trade(bars,ei,xi,tight,wide,cg)):
                clw.writerow([coin,ti,seq,f"{net:.4f}"])
        print(f"{coin:5s} W={W}h thr={thr:+.2f} cg={cg:g}  T(a{ta:g}/s{tst}/g{tg:g}) W(a{wa:g}/s{wst}/g{wg:g})  trades={len(trades)}")
    rc.close(); pt.close(); cl.close()
    print("wrote roster_cfg.csv / parent_trades.csv / ref_clips.csv")

if __name__=="__main__":
    main()
