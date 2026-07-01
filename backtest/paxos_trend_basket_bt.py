#!/usr/bin/env python3
# Paxos long-only trend BASKET — faithful BT across the 14 Paxos-tradeable coins.
#
# The Paxos-tradeable spot edge hunt. [[CrossSectionalMomentum]] ranked the universe
# cross-sectionally and FAILED OOS on this universe (14-coin 2025 -28%). This tests
# the OTHER structure the vault suggested ([[DogeLongOnlyTrend]] line "natural home =
# a coin in the Paxos universe"): a per-coin long-only trend sleeve (emax 20/50 or
# Donchian), BTC>200DMA-gated, run INDEPENDENTLY on every coin, equal risk budget,
# basket = mean of the per-coin sleeve equity curves.
#
# Same faithful engine as doge_longonly_trend_bt.py (long-only, structural exit or
# regime-off, real per-side spot cost, ride-wide). IS/OOS 70/30 split. The honest
# question: does the BASKET survive OOS after cost, and how broad is the edge?
import csv, datetime

COINS=['BTC','ETH','SOL','ADA','BCH','DOGE','LINK','LTC','XRP','AVAX','UNI','NEAR','LDO','AAVE']

def load(path):
    rows=[]
    for r in csv.reader(open(path)):
        if not r or not r[0].isdigit(): continue
        rows.append((int(r[0]),float(r[1]),float(r[2]),float(r[3]),float(r[4])))
    return rows

data={c:load(f'data/{c}USDT_1d.csv') for c in COINS}
BTC=data['BTC']

# BTC 200DMA gate keyed by UTC date.
btc_close={datetime.datetime.utcfromtimestamp(ts/1000).date():c for ts,o,h,l,c in BTC}
btc_dates=sorted(btc_close); cvals=[btc_close[d] for d in btc_dates]
gate={}
for i,d in enumerate(btc_dates):
    gate[d] = (i>=200 and cvals[i]>sum(cvals[i-200:i])/200.0)

def ema_series(xs,n):
    k=2/(n+1); out=[xs[0]]
    for x in xs[1:]: out.append(out[-1]+k*(x-out[-1]))
    return out

def run_coin(rows, sig, cost_bps_side, gated=True, split=None):
    ds=[datetime.datetime.utcfromtimestamp(ts/1000).date() for ts,*_ in rows]
    closes=[c for *_,c in rows]; highs=[h for _,_,h,_,_ in rows]; lows=[l for _,_,_,l,_ in rows]
    idx0,idx1 = split if split else (0,len(rows))
    if idx1<=idx0 or idx1-max(60,idx0)<5: return None
    pos=False; entry=0.0; eq=1.0; peak=1.0; maxdd=0.0
    wins=0; losses=0; gross_win=0.0; gross_loss=0.0; trades=0
    ema_f=ema_series(closes,20); ema_s=ema_series(closes,50)
    c=cost_bps_side/1e4
    for i in range(max(60,idx0),idx1):
        d=ds[i]; px=closes[i]
        g = gate.get(d,False) if gated else True
        if sig=='donch20':
            enter = i>=20 and closes[i]>max(highs[i-20:i]); exit_ = i>=10 and closes[i]<min(lows[i-10:i])
        elif sig=='donch55':
            enter = i>=55 and closes[i]>max(highs[i-55:i]); exit_ = i>=20 and closes[i]<min(lows[i-20:i])
        else:  # emax
            enter = ema_f[i]>ema_s[i]; exit_ = ema_f[i]<ema_s[i]
        if not pos:
            if enter and g: pos=True; entry=px*(1+c); trades+=1
        else:
            if exit_ or (gated and not g):
                ret=(px*(1-c))/entry-1; eq*=(1+ret)
                if ret>0: wins+=1; gross_win+=ret
                else: losses+=1; gross_loss+=-ret
                pos=False
        cur = eq*(1+((px*(1-c))/entry-1)) if pos else eq
        peak=max(peak,cur); maxdd=max(maxdd,(peak-cur)/peak)
    if pos:
        ret=(closes[idx1-1]*(1-c))/entry-1; eq*=(1+ret)
        if ret>0: wins+=1; gross_win+=ret
        else: losses+=1; gross_loss+=-ret
    pf = gross_win/gross_loss if gross_loss>0 else (float('inf') if gross_win>0 else 0.0)
    wr = wins/(wins+losses) if (wins+losses)>0 else 0
    return dict(net=eq-1, pf=pf, dd=maxdd, trades=trades, wr=wr, gw=gross_win, gl=gross_loss)

def basket(sig, cost, split=None):
    # Each coin = independent equal-risk sleeve; basket net = mean of per-coin sleeve nets.
    # Basket PF from summed gross win/loss across sleeves (pooled). Per-coin split by
    # each coin's own bar count (aligns IS/OOS by coin history, not calendar — same as DOGE BT).
    nets=[]; gw=0.0; gl=0.0; per={}
    for c in COINS:
        rows=data[c]; n=len(rows)
        sp = None if split is None else (int(n*split[0]), int(n*split[1]))
        r=run_coin(rows,sig,cost,gated=True,split=sp)
        if r is None: continue
        nets.append(r['net']); gw+=r['gw']; gl+=r['gl']; per[c]=r
    bnet=sum(nets)/len(nets) if nets else 0.0
    bpf = gw/gl if gl>0 else float('inf')
    return bnet, bpf, per

n_split_is=(0.0,0.7); n_split_oos=(0.7,1.0)
print("Paxos long-only trend BASKET — 14 coins, per-coin equal-risk sleeve, BTC>200DMA gate")
print("basket net = mean of per-coin sleeve nets; PF = pooled gross win/loss. IS/OOS = per-coin 70/30.\n")
for sig in ['emax','donch20','donch55']:
    print(f"===== {sig} =====")
    for cost in [20,35,50]:
        fn,fp,_   = basket(sig,cost)
        isn,isp,_ = basket(sig,cost,n_split_is)
        on,op,per = basket(sig,cost,n_split_oos)
        print(f"  cost{cost:>2}bps  FULL net{fn*100:+7.0f}% PF{fp:4.2f}  |  IS net{isn*100:+6.0f}% PF{isp:4.2f}  |  OOS net{on*100:+6.0f}% PF{op:4.2f}")
    # per-coin OOS breadth at 35bps
    _,_,per = basket(sig,35,n_split_oos)
    winners=sum(1 for c in per if per[c]['net']>0)
    print(f"  OOS breadth @35bps: {winners}/{len(per)} coins net-positive")
    print("   " + "  ".join(f"{c}:{per[c]['net']*100:+.0f}%" for c in COINS if c in per))
    print()
