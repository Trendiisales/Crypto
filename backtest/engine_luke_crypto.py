#!/usr/bin/env python3
"""
LukeCryptoEngine — faithful crypto momentum engine, validated 2026-06-25.
Recovered from Trash + given a --live emitter for the IBKRCrypto shadow book (2026-06-30).

Recipe (Omega/backtest/luke_system matrix, 20 alts, Binance daily 2019-2026;
see Memory-Chimera/wiki/entities/LukeCryptoMomentum.md):
  ENTRY : setup C (inside-day / micro-VCP breakout) PRIMARY + setup B (anchored-VWAP
          cluster) SECONDARY. Setup A (pullback-to-EMA) EXCLUDED (dead on crypto, CY2022 0.05).
  SELECT: high-ADR (>=3%) + tight structural stop-width <= 6%.
  EXIT  : RIDE WIDE — exit only on first DAILY CLOSE below 9EMA. No trimming (cuts the fat tail).
  REGIME: BTC>200MA market gate MANDATORY (ungated dies 2022 -65% bear, -$112k; gated sidesteps).
  SIZING: shares = risk_frac*equity / (entry - stop).

Modes:
  python3 engine_luke_crypto.py <csv_dir>          -> faithful BT summary (validation)
  python3 engine_luke_crypto.py <csv_dir> --live   -> JSON of currently-open positions +
                                                      trades closed on the last bar (shadow feed)
CSV schema: date,open,high,low,close,volume
"""
import sys, glob, os, json
import numpy as np, pandas as pd

CFG = dict(
    entry_C=True, entry_B=True, entry_A=False,
    adr_min=3.0, adr_win=20,
    touch_buf=0.015, base_buf=0.06, stop_buf=0.003,
    avwap_band=0.02, cluster_band=0.02, cluster_min=2, avwap_lookback=40,
    min_stopw=0.005, max_stopw=0.06,
    risk_pct=0.01, max_pos_pct=0.35, max_concurrent=5,
    cost_bps=6.0, stop_slip_bps=10.0, annual_carry=0.10,  # long SQF pays daily TFA financing; flat proxy (alt universe not in BTC/ETH/SOL funding csv)
    regime_gate=True, regime_sma=200,
    equity0=10000.0, engine_tag="LukeCrypto",
    live_start="2026-06-01",  # forward shadow book opens here (this month = all of June); NO earlier trades counted
)

def ema(s, n): return s.ewm(span=n, adjust=False).mean()

def load(path):
    t = os.path.basename(path)[:-4]
    d = pd.read_csv(path); d['date'] = pd.to_datetime(d['date'])
    d = d.sort_values('date').reset_index(drop=True)
    d = d[(d[['open','high','low','close']] > 0).all(axis=1)].reset_index(drop=True)
    if len(d) < 260: return None
    c, h, l, v = d['close'], d['high'], d['low'], d['volume']
    d['ema9'], d['ema21'], d['ema50'] = ema(c,9), ema(c,21), ema(c,50)
    d['adr'] = ((h-l)/c*100).rolling(CFG['adr_win']).mean()
    d['ema21_slope'] = d['ema21'] - d['ema21'].shift(5)
    tp = (h+l+c)/3.0; n=len(d); av=np.full(n,np.nan); lb=CFG['avwap_lookback']
    cpv=cv=0.0
    for i in range(n):
        swing = 2<=i<n-2 and l[i]==min(l[max(0,i-lb):i+1]) and l[i]<l[i-1] and l[i]<l[i+1]
        if swing: cpv=tp[i]*v[i]; cv=v[i]
        else: cpv+=tp[i]*v[i]; cv+=v[i]
        av[i]=cpv/cv if cv>0 else np.nan
    d['avwap']=av; d['ticker']=t
    return d

def btc_regime(univ):
    key = next((k for k in univ if k.upper().startswith('BTC')), None)
    if key:
        s = univ[key].set_index('date')['close']
    else:
        acc={}
        for d in univ.values():
            n=d.set_index('date')['close']; n=n/n.iloc[0]
            for dt,val in n.items(): acc.setdefault(dt,[]).append(val)
        s = pd.Series({dt:np.mean(v) for dt,v in acc.items()}).sort_index()
    reg = np.where(s > s.rolling(CFG['regime_sma']).mean(), 'bull', 'bear')
    return dict(zip(s.index, reg))

def signals(d):
    out=[]; h,l,c=d['high'].values,d['low'].values,d['close'].values
    e9,e21,e50=d['ema9'].values,d['ema21'].values,d['ema50'].values
    sl=d['ema21_slope'].values; adr=d['adr'].values; av=d['avwap'].values
    for i in range(55,len(d)-1):
        if np.isnan(e50[i]) or np.isnan(adr[i]) or adr[i]<CFG['adr_min']: continue
        up = sl[i]>0; cand=None
        if CFG['entry_C'] and up and e9[i]>e21[i] and c[i]>e21[i]:
            in1 = h[i]<h[i-1] and l[i]>l[i-1]
            in2 = in1 and h[i-1]<h[i-2] and l[i-1]>l[i-2]
            if in1 or in2: cand=('C',h[i],l[i]*(1-CFG['stop_buf']))
        if cand is None and CFG['entry_B'] and up and c[i]>e50[i] and not np.isnan(av[i]):
            if abs(l[i]-av[i])/c[i] < CFG['avwap_band'] and c[i]>av[i]:
                band=CFG['cluster_band']*c[i]
                ncl=sum(1 for x in (av[i],e21[i],e9[i],round(c[i])) if abs(x-l[i])<=band)
                if ncl>=CFG['cluster_min']: cand=('B',h[i],min(av[i],l[i])*(1-CFG['stop_buf']))
        if cand is None: continue
        setup,trig,stop=cand
        if trig<=stop: continue
        sw=(trig-stop)/trig
        if sw<CFG['min_stopw'] or sw>CFG['max_stopw']: continue
        out.append(dict(i=i,date=d['date'].iloc[i],setup=setup,trig=trig,stop=stop,adr=adr[i]))
    return out

def backtest(univ, reg, live=False):
    idx={t:{dt:k for k,dt in enumerate(d['date'])} for t,d in univ.items()}
    sigmap={}
    for t,d in univ.items():
        for s in signals(d): sigmap.setdefault(s['date'],[]).append((t,s))
    dates=sorted(set().union(*[set(d['date']) for d in univ.values()]))
    last=dates[-1]
    eq=cash=CFG['equity0']; cost=CFG['cost_bps']/1e4; slip=CFG['stop_slip_bps']/1e4
    day_carry=CFG['annual_carry']/365.0   # long SQF financing charged per day held
    pos={}; trades=[]; curve=[]
    for di,dt in enumerate(dates):
        for t in list(pos.keys()):
            k=idx[t].get(dt)
            if k is None: continue
            d=univ[t]; o,hi,lo,cl,e9=d['open'][k],d['high'][k],d['low'][k],d['close'][k],d['ema9'][k]
            p=pos[t]
            if lo<=p['stop']:
                fill=(min(o,p['stop']) if o<p['stop'] else p['stop'])*(1-slip)
                cash+=p['sh']*fill*(1-cost); p['pnl']+=p['sh']*(fill-p['entry'])
                trades.append(_fin(p,dt,'stop')); del pos[t]; continue
            if not np.isnan(e9) and cl<e9:
                fill=cl*(1-cost); cash+=p['sh']*fill*(1-cost); p['pnl']+=p['sh']*(fill-p['entry'])
                trades.append(_fin(p,dt,'close<9ema')); del pos[t]
        for t,p in pos.items():               # daily financing on each open long (notional * day rate)
            if dt in idx[t]:
                fin=p['sh']*univ[t]['close'][idx[t][dt]]*day_carry; cash-=fin; p['pnl']-=fin
        mtm=cash+sum(p['sh']*univ[t]['close'][idx[t][dt]] for t,p in pos.items() if dt in idx[t])
        eq=mtm; curve.append(eq)
        prev=sigmap.get(dates[di-1]) if di>0 else None
        can_open = (not live) or str(dt)[:10] >= CFG['live_start']
        if prev and can_open and len(pos)<CFG['max_concurrent']:
            r=reg.get(dt,'bull')
            if not (CFG['regime_gate'] and r=='bear'):
                cands=[(t,s,idx[t][dt]) for t,s in prev if t not in pos and dt in idx[t]]
                cands.sort(key=lambda x:(x[1]['trig']-x[1]['stop'])/x[1]['trig'])
                for t,s,k in cands:
                    if len(pos)>=CFG['max_concurrent']: break
                    d=univ[t]
                    if d['high'][k]<s['trig']: continue
                    fill=(d['open'][k] if d['open'][k]>s['trig'] else s['trig'])*(1+cost)
                    sw=fill-s['stop']
                    if sw<=0: continue
                    sh=int(eq*CFG['risk_pct']/sw)
                    val=sh*fill; cap=eq*CFG['max_pos_pct']
                    if val>cap: sh=int(cap/fill); val=sh*fill
                    if val>cash: sh=int(cash/fill); val=sh*fill
                    if sh<=0: continue
                    cash-=val*(1+cost)
                    pos[t]=dict(ticker=t,entry=fill,sh=sh,stop=s['stop'],setup=s['setup'],
                                reg=r,risk=eq*CFG['risk_pct'],pnl=0.0,date_in=dt)
    if live:
        # snapshot open positions at the last bar; trades closed on the last bar are today's closes
        opens=[]
        for t,p in pos.items():
            cl=float(univ[t]['close'].iloc[-1]); e9=float(univ[t]['ema9'].iloc[-1])
            opens.append(dict(ticker=t,setup=p['setup'],entry=round(p['entry'],6),stop=round(p['stop'],6),
                              shares=p['sh'],close=round(cl,6),ema9=round(e9,6),reg=p['reg'],
                              date_in=str(p['date_in'])[:10],unreal=round(p['sh']*(cl-p['entry']),2)))
        closed_today=[t for t in trades if str(t['date_out'])[:10]==str(last)[:10]]
        return dict(equity=round(eq,2),equity0=CFG['equity0'],last=str(last)[:10],n_open=len(opens),
                    open=opens,closed_today=closed_today)
    for t,p in list(pos.items()):
        p['pnl']+=p['sh']*(univ[t]['close'].iloc[-1]-p['entry']); trades.append(_fin(p,last,'eob'))
    return trades, curve

def _fin(p,date_out,why): return dict(ticker=p['ticker'],setup=p['setup'],reg=p['reg'],
    date_in=str(p['date_in'])[:10],date_out=str(date_out)[:10],why=why,
    pnl=round(p['pnl'],2),R=round(p['pnl']/p['risk'],3) if p['risk'] else 0)

def metrics(trades, curve, f=None):
    ts=[t for t in trades if f is None or f(t)]
    if not ts: return None
    pnl=np.array([t['pnl'] for t in ts]); w=pnl[pnl>0]; lo=pnl[pnl<0]
    eq=np.array(curve); peak=np.maximum.accumulate(eq)
    return dict(n=len(ts),pf=round(w.sum()/abs(lo.sum()),2) if lo.sum() else 9.9,
        wr=round(len(w)/len(ts)*100,1),ret=round((eq[-1]/eq[0]-1)*100,1),
        dd=round(((peak-eq)/peak).max()*100,1),net=round(pnl.sum()))

if __name__=='__main__':
    d=sys.argv[1] if len(sys.argv)>1 else '/tmp/luke_crypto'
    live='--live' in sys.argv
    univ={(os.path.basename(p)[:-4]):load(p) for p in sorted(glob.glob(f'{d}/*.csv'))}
    univ={k:v for k,v in univ.items() if v is not None}
    reg=btc_regime(univ)
    if live:
        print(json.dumps(backtest(univ,reg,live=True)))
    else:
        tr,cv=backtest(univ,reg)
        print(f"LukeCryptoEngine | {len(univ)} names | gate={CFG['regime_gate']}")
        print('  ALL  ', metrics(tr,cv))
        print('  bull ', metrics(tr,cv,lambda t:t['reg']=='bull'))
        print('  bear ', metrics(tr,cv,lambda t:t['reg']=='bear'))
        print('  CY22 ', metrics(tr,cv,lambda t:str(t['date_in'])[:4]=='2022'))
