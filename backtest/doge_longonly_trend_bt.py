#!/usr/bin/env python3
# DOGE long-only trend sleeve — faithful BT (spot Paxos: long-only, no shorts).
#
# Context: DOGE is spot-only on IBKR/Paxos (long-only), no standalone validated
# edge in the Chimera vault. This tests whether a plain long-only trend sleeve
# (Donchian breakout / EMA cross) with the mandatory BTC>200DMA regime gate is
# net-positive after real spot cost, IS and OOS. Per [[BearSpotNoEdge]] the
# gate is essential; per [[UpMoveTrailLossMitigation]] a trend sleeve rides wide
# (trailing/structural exit only, no per-trade BE/stop that amputates fat tails).
#
# Data verified clean 2026-07-02: integrity_gate flagged 4 ">50% jumps" but all
# are real DOGE meme moves (2021-01-28 +392% WSB squeeze, Elon pumps) with
# internally-consistent OHLC — NOT x1000 corruption. Threshold mis-calibrated
# for meme volatility.
import csv, datetime

def load(path):
    rows=[]
    for r in csv.reader(open(path)):
        if not r or not r[0].isdigit(): continue
        rows.append((int(r[0]),float(r[1]),float(r[2]),float(r[3]),float(r[4])))
    return rows

DOGE=load('data/DOGEUSDT_1d.csv')
BTC =load('data/BTCUSDT_1d.csv')

# BTC 200DMA gate, keyed by UTC date -> True if BTC close > its 200DMA that day.
btc_close={datetime.datetime.utcfromtimestamp(ts/1000).date():c for ts,o,h,l,c in BTC}
btc_dates=sorted(btc_close)
gate={}
cvals=[btc_close[d] for d in btc_dates]
for i,d in enumerate(btc_dates):
    if i<200: gate[d]=False; continue
    ma=sum(cvals[i-200:i])/200.0
    gate[d]=cvals[i]>ma

def sma(xs,n,i):  # sma of xs[i-n..i-1]
    if i<n: return None
    return sum(xs[i-n:i])/n
def ema_series(xs,n):
    k=2/(n+1); out=[xs[0]]
    for x in xs[1:]: out.append(out[-1]+k*(x-out[-1]))
    return out

def run(sig, cost_bps_side, gated=True, split=None):
    ds=[datetime.datetime.utcfromtimestamp(ts/1000).date() for ts,*_ in DOGE]
    closes=[c for *_,c in DOGE]; highs=[h for _,_,h,_,_ in DOGE]; lows=[l for _,_,_,l,_ in DOGE]
    idx0,idx1 = split if split else (0,len(DOGE))
    pos=False; entry=0.0; eq=1.0; peak=1.0; maxdd=0.0
    wins=0; losses=0; gross_win=0.0; gross_loss=0.0; trades=0
    ema_f=ema_series(closes,20); ema_s=ema_series(closes,50)
    c=cost_bps_side/1e4
    for i in range(max(60,idx0),idx1):
        d=ds[i]; px=closes[i]
        g = gate.get(d,False) if gated else True
        # signal (long-only)
        if sig=='donch20':
            enter = i>=20 and closes[i]>max(highs[i-20:i])
            exit_ = i>=10 and closes[i]<min(lows[i-10:i])
        elif sig=='donch55':
            enter = i>=55 and closes[i]>max(highs[i-55:i])
            exit_ = i>=20 and closes[i]<min(lows[i-20:i])
        elif sig=='emax':
            enter = ema_f[i]>ema_s[i]
            exit_ = ema_f[i]<ema_s[i]
        if not pos:
            if enter and g:
                pos=True; entry=px*(1+c); trades+=1
        else:
            if exit_ or (gated and not g):   # structural exit OR regime flips off
                ret=(px*(1-c))/entry-1
                eq*=(1+ret)
                if ret>0: wins+=1; gross_win+=ret
                else: losses+=1; gross_loss+=-ret
                pos=False
        if pos:
            m=(px*(1-c))/entry-1; cur=eq*(1+m)
        else: cur=eq
        peak=max(peak,cur); maxdd=max(maxdd,(peak-cur)/peak)
    if pos:  # mark-to-close final open trade
        ret=(closes[idx1-1]*(1-c))/entry-1; eq*=(1+ret)
        if ret>0: wins+=1; gross_win+=ret
        else: losses+=1; gross_loss+=-ret
    pf = gross_win/gross_loss if gross_loss>0 else float('inf')
    wr = wins/(wins+losses) if (wins+losses)>0 else 0
    return dict(net=eq-1, pf=pf, dd=maxdd, trades=trades, wr=wr)

n=len(DOGE); split_is=(0,int(n*0.7)); split_oos=(int(n*0.7),n)
print(f"DOGE long-only trend BT  ({n} bars, {datetime.datetime.utcfromtimestamp(DOGE[0][0]/1000).date()} .. {datetime.datetime.utcfromtimestamp(DOGE[-1][0]/1000).date()})")
print("cost = per-side bps (Paxos spot commission+slip). Long-only, BTC>200DMA gate.\n")
for sig in ['donch20','donch55','emax']:
    print(f"== {sig} ==")
    for cost in [20,35,50]:
        full=run(sig,cost,gated=True)
        ung =run(sig,cost,gated=False)
        isr =run(sig,cost,gated=True,split=split_is)
        oos =run(sig,cost,gated=True,split=split_oos)
        print(f"  cost{cost:>2}bps  GATED full: net{full['net']*100:+7.0f}% PF{full['pf']:4.2f} DD{full['dd']*100:3.0f}% n{full['trades']:>3} WR{full['wr']*100:2.0f}  | IS PF{isr['pf']:4.2f} net{isr['net']*100:+6.0f}%  OOS PF{oos['pf']:4.2f} net{oos['net']*100:+6.0f}%  | ungated PF{ung['pf']:4.2f}")
    print()
