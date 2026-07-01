#!/usr/bin/env python3
"""
WHOLE-MONTH (June 2026) book reconstruction at the $10k pool — REAL engine replay, no
hypothetical lifetime curve. For each leg, replay the SAME engine/gate the live shadow uses
(ibkrcrypto_bt --postrace: per-bar pos + size-scaled MTM cumret, regime-gated like the live
--signal path) over [June 1, now]. Then size exactly like refresh_shadow.py: a single $POOL
split equally across the legs OPEN on each bar -> book daily $ = POOL * mean(open-leg daily ret).
Writes a `month` block into the book's state.json so the GUI can show the full-June figure.

Daily book. (Intraday handled by month_book_june_intraday.py with the same method.)
"""
import os, subprocess, json, datetime
HERE=os.path.dirname(os.path.abspath(__file__))
BT=os.path.join(HERE,"ibkrcrypto_bt")
STATE_DIR=os.path.join(HERE,"data","ibkrcrypto")
TICK="/Users/jo/Tick"
BTCF=f"{HERE}/data/BTCUSDT_1d.csv"; ETHF=f"{HERE}/data/ETHUSDT_1d.csv"; SOLF=f"{HERE}/data/SOLUSDT_1d.csv"; NDXF=f"{TICK}/NDX_daily_2016_2026.csv"
# mirror refresh_shadow.py ROSTER (key,sym,csv,COST_BPS,strat). COST is the round-trip
# bps the live shadow passes as COSTBPS (ETH 28, BTC 14, SOL 11, NDX 4) -- NOT the contract
# multiplier (0.20/0.01/...). Earlier this column was wrongly filled with the mult -> the
# daily June book ran near-zero-cost. Fixed S-2026-06-30 to match refresh_shadow.py exactly.
ROSTER=[
 ("eth_emax","ETH",ETHF,28,"EMAx"),("eth_kelt","ETH",ETHF,28,"Kelt"),
 ("btc_emax","BTC",BTCF,14,"EMAx"),("btc_kelt","BTC",BTCF,14,"Kelt"),
 ("sol_emax","SOL",SOLF,11,"EMAx"),("sol_kelt","SOL",SOLF,11,"Kelt"),
 ("ndx_tsmom","NDX",NDXF,4,"TSMom50"),
 ("btc_reg","BTC",BTCF,14,"Regime"),("eth_reg","ETH",ETHF,28,"Regime"),("sol_reg","SOL",SOLF,11,"Regime"),
 ("btc_ibs","BTC",BTCF,14,"IBS"),("sol_ibs","SOL",SOLF,11,"IBS"),
 ("ndx_rsir","NDX",NDXF,4,"RSIrev"),
 ("btc_roc","BTC",BTCF,14,"Roc"),("sol_roc","SOL",SOLF,11,"Roc"),
]
GATED_STRATS={"EMAx","TSMom50","Roc"}; REGIME_GATE_MA=200
POOL=float(os.environ.get("POOL_USD","10000"))
JUNE_START_MS=1780272000000   # 2026-06-01 00:00 UTC
NOW_MS=int(datetime.datetime.now(datetime.timezone.utc).timestamp()*1000)

# live-fidelity cost overlays (model what a real IBKR switch would pay):
#  SLIP_BPS  = adverse-fill slippage beyond half-spread, per round trip (SOL thinner -> wider)
#  FUND_DIR  = Binance USDT-perp funding history, a REAL-financing PROXY for the CME SQF daily
#              TFA basis (not entitled to real SQF basis yet -- see fetch_funding.py). NDX is
#              equity SQF (different carry) -> keep the flat ANNUAL_CARRY placeholder, no proxy.
SLIP_BPS={"BTC":3,"ETH":3,"SOL":5,"NDX":1}
FUND_DIR=os.path.join(HERE,"data","funding")
def postrace(sym,csv,cost,strat,slip_on=True,fund_on=True):
    rma=REGIME_GATE_MA if strat in GATED_STRATS else 0
    env=dict(os.environ,COSTBPS=str(cost),REGIME_MA=str(rma),
             SLIPBPS=str(SLIP_BPS.get(sym,3) if slip_on else 0))
    fc=os.path.join(FUND_DIR,f"{sym}.csv")
    if fund_on:
        if os.path.exists(fc): env["FUNDCSV"]=fc; env["USE_REAL_FUND"]="1"   # crypto: real perp-funding proxy
    else:
        env["USE_REAL_FUND"]="0"; env["ANNUAL_CARRY"]="0"                    # strip ALL financing (real proxy + flat carry)
    out=subprocess.run([BT,sym,csv,"--postrace",strat,str(JUNE_START_MS),str(NOW_MS)],
        capture_output=True,text=True,env=env,timeout=30).stdout
    series=[]
    for ln in out.splitlines():
        p=ln.split(",")
        if len(p)==3 and p[0].lstrip("-").isdigit():
            series.append((int(p[0]),int(p[1]),float(p[2])))
    return series

# Rebuild the WHOLE book under a cost config (slip/funding on/off) so slip & funding $ are
# attributed by differencing the book total: slip_cost = book(slip OFF) - book(full);
# funding_cost = book(funding OFF) - book(full). Same POOL/n_open sizing for all 3 runs.
def build_book(slip_on=True,fund_on=True):
    legs=[]
    for key,sym,csv,cost,strat in ROSTER:
        s=postrace(sym,csv,cost,strat,slip_on,fund_on)
        rets={}; pos={}; prev=0.0
        for ts,p,cum in s:
            rets[ts]=cum-prev; pos[ts]=p; prev=cum
        legs.append(dict(key=key,sym=sym,strat=strat,rets=rets,pos=pos,
                         pos_now=(s[-1][1] if s else 0)))
    all_ts=sorted(set().union(*[set(L["rets"]) for L in legs]) or set())
    book_usd=0.0; leg_usd={L["key"]:0.0 for L in legs}
    for ts in all_ts:
        openL=[L for L in legs if L["pos"].get(ts,0)!=0 and ts in L["rets"]]
        if not openL: continue
        w=POOL/len(openL)
        for L in openL:
            c=w*L["rets"][ts]; leg_usd[L["key"]]+=c; book_usd+=c
    return book_usd,leg_usd,legs,len(all_ts)

book_usd,leg_usd,legs,nbars=build_book(True,True)            # full (net of slip + real funding)
book_noslip,_,_,_=build_book(False,True)                     # slip stripped
book_nofund,_,_,_=build_book(True,False)                     # financing stripped
slip_cost=round(book_noslip-book_usd,2)                       # $ slippage drag (>=0)
fund_cost=round(book_nofund-book_usd,2)                       # $ financing drag (sign = carry direction)
gross_usd=round(book_usd+slip_cost+fund_cost,2)              # pre slip+financing (interaction tiny)

per_leg=sorted(([L["key"],round(leg_usd[L["key"]],2),L["pos_now"]] for L in legs),
               key=lambda x:x[1])
n_open_now=sum(1 for L in legs if L["pos_now"]!=0)
month=dict(window="2026-06-01..now",pool_usd=POOL,book_usd=round(book_usd,2),
           ret_pct=round(book_usd/POOL*100,2),n_open=n_open_now,
           gross_usd=gross_usd,slip_cost_usd=slip_cost,funding_cost_usd=fund_cost,
           method="real engine replay (postrace, regime-gated, POOL/n_open sizing); slip+funding attributed by book-level differencing",
           per_leg_usd={k:v for k,v,_ in per_leg})
sp=os.path.join(STATE_DIR,"state.json")
st=json.load(open(sp)) if os.path.exists(sp) else {}
st["month"]=month
json.dump(st,open(sp,"w"),indent=1)
print(f"[JUNE] Daily book ${book_usd:,.2f} ({month['ret_pct']:+.2f}% on ${POOL:,.0f}) "
      f"n_open={n_open_now} bars={nbars}")
print(f"   gross ${gross_usd:,.2f}  - slip ${slip_cost:,.2f}  - funding ${fund_cost:,.2f}  = net ${book_usd:,.2f}")
for k,v,p in per_leg: print(f"   {k:11} ${v:+8.2f}  pos_now={p:+d}")
