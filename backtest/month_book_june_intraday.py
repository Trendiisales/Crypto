#!/usr/bin/env python3
"""
WHOLE-MONTH (June 2026) INTRADAY book reconstruction at the $10k pool — REAL engine replay.
Same method as month_book_june.py but for the mixed 4h/1h intraday roster: each leg replayed
via ibkrcrypto_bt --postrace at its own BT_TF_MS, ETH-only regime gate (tf-scaled 200d), then
sized like refresh_shadow_intraday.py (single $POOL split across legs OPEN at each instant).
Because legs run on different timeframes, n_open is evaluated by FORWARD-FILLING each leg's
position to the realizing bar's timestamp (a 4h leg stays open across the 1h bars it spans).
.vt legs signal 0 in the binary (parked) -> contribute nothing, matching the live book.
"""
import os, subprocess, json, datetime, bisect
HERE=os.path.dirname(os.path.abspath(__file__))
BT=os.path.join(HERE,"ibkrcrypto_bt"); STATE_DIR=os.path.join(HERE,"data","ibkrcrypto_intraday")
H4=14400000; H1=3600000
def D(sym,tf): return f"{HERE}/data/{sym}USDT_{tf}.csv"
B4,E4,S4=D("BTC","4h"),D("ETH","4h"),D("SOL","4h"); B1,E1,S1=D("BTC","1h"),D("ETH","1h"),D("SOL","1h")
# (key, sym, csv, tf_ms, cost, strat) — mirror refresh_shadow_intraday ROSTER
ROSTER=[
 ("btc_emax_mbt","BTC",B4,H4,6,"EMAx"),("btc_kelt_mbt","BTC",B4,H4,6,"Kelt"),
 ("btc_emax_sqf","BTC",B4,H4,22,"EMAx"),("btc_kelt_sqf","BTC",B4,H4,22,"Kelt.vt"),
 ("eth_emax_met","ETH",E4,H4,8,"EMAx"),("eth_kelt_met","ETH",E4,H4,8,"Kelt"),
 ("eth_ichi_met","ETH",E4,H4,8,"Ichi"),("eth_tsmom_met","ETH",E4,H4,8,"TSMom50"),
 ("eth_macd_met","ETH",E4,H4,8,"Macd"),("eth_donch40_met","ETH",E4,H4,8,"Donch40"),
 ("eth_emax_sqf","ETH",E4,H4,28,"EMAx"),("eth_kelt_sqf","ETH",E4,H4,28,"Kelt.vt"),
 ("eth_ichi_sqf","ETH",E4,H4,28,"Ichi.vt"),("eth_tsmom_sqf","ETH",E4,H4,28,"TSMom50"),
 ("eth_macd_sqf","ETH",E4,H4,28,"Macd.vt"),("eth_donch40_sqf","ETH",E4,H4,28,"Donch40"),
 ("sol_emax_fut","SOL",S4,H4,2,"EMAx"),("sol_kelt_fut","SOL",S4,H4,2,"Kelt.vt"),
 ("sol_ichi_fut","SOL",S4,H4,2,"Ichi"),("sol_tsmom_fut","SOL",S4,H4,2,"TSMom50"),
 ("sol_macd_fut","SOL",S4,H4,2,"Macd"),
 ("eth_emax_1h","ETH",E1,H1,8,"EMAx"),("sol_emax_1h","SOL",S1,H1,2,"EMAx"),
 ("sol_donch40_1h","SOL",S1,H1,2,"Donch40"),
]
GATED_INTRADAY={("ETH","EMAx"),("ETH","TSMom50"),("ETH","Ichi")}; REGIME_GATE_MA_DAYS=200  # ETH Ichi gated S-2026-06-30 (DD -21%, edge intact); SOL Ichi left ungated (gate costs 22% net)
POOL=float(os.environ.get("POOL_USD","10000"))
JUNE_START_MS=1780272000000; NOW_MS=int(datetime.datetime.now(datetime.timezone.utc).timestamp()*1000)

# live-fidelity overlays (see month_book_june.py): per-symbol slippage + real perp-funding proxy
# for the SQF daily TFA basis. funding csv is daily-bucketed; the binary maps it onto the leg's
# own BT_TF_MS bar timestamps (intraday legs accrue the day's funding once it crosses a day edge).
SLIP_BPS={"BTC":3,"ETH":3,"SOL":5}
FUND_DIR=os.path.join(HERE,"data","funding")
def pos_at(L,t):  # forward-filled position at instant t (most-recent bar <= t)
    i=bisect.bisect_right(L["ts"],t)-1
    return L["pos"][i] if i>=0 else 0
def postrace(sym,csv,tf_ms,cost,strat,slip_on=True,fund_on=True):
    rma=round(REGIME_GATE_MA_DAYS*86400000/tf_ms) if (sym,strat) in GATED_INTRADAY else 0
    env=dict(os.environ,COSTBPS=str(cost),BT_TF_MS=str(tf_ms),REGIME_MA=str(rma),
             SLIPBPS=str(SLIP_BPS.get(sym,3) if slip_on else 0))
    fc=os.path.join(FUND_DIR,f"{sym}.csv")
    if fund_on:
        if os.path.exists(fc): env["FUNDCSV"]=fc; env["USE_REAL_FUND"]="1"
    else:
        env["USE_REAL_FUND"]="0"; env["ANNUAL_CARRY"]="0"
    out=subprocess.run([BT,sym,csv,"--postrace",strat,str(JUNE_START_MS),str(NOW_MS)],
        capture_output=True,text=True,env=env,timeout=40).stdout
    s=[]
    for ln in out.splitlines():
        p=ln.split(",")
        if len(p)==3 and p[0].lstrip("-").isdigit(): s.append((int(p[0]),int(p[1]),float(p[2])))
    return s

# Rebuild whole book under a cost config -> slip & funding $ attributed by book-level
# differencing (same ff POOL/n_open sizing all 3 runs). See month_book_june.py.
def build_book(slip_on=True,fund_on=True):
    legs=[]
    for key,sym,csv,tf_ms,cost,strat in ROSTER:
        s=postrace(sym,csv,tf_ms,cost,strat,slip_on,fund_on)
        ts=[r[0] for r in s]; posarr=[r[1] for r in s]
        rets={}; prev=0.0
        for t,p,cum in s: rets[t]=cum-prev; prev=cum
        legs.append(dict(key=key,ts=ts,pos=posarr,rets=rets,pos_now=(s[-1][1] if s else 0)))
    all_ts=sorted(set().union(*[set(L["rets"]) for L in legs]) or set())
    book_usd=0.0; leg_usd={L["key"]:0.0 for L in legs}
    for t in all_ts:
        nopen=sum(1 for L in legs if pos_at(L,t)!=0)
        if nopen==0: continue
        w=POOL/nopen
        for L in legs:
            if t in L["rets"] and pos_at(L,t)!=0:
                c=w*L["rets"][t]; leg_usd[L["key"]]+=c; book_usd+=c
    return book_usd,leg_usd,legs,len(all_ts)

book_usd,leg_usd,legs,nbars=build_book(True,True)
book_noslip,_,_,_=build_book(False,True)
book_nofund,_,_,_=build_book(True,False)
slip_cost=round(book_noslip-book_usd,2)
fund_cost=round(book_nofund-book_usd,2)
gross_usd=round(book_usd+slip_cost+fund_cost,2)

per_leg=sorted(([k,round(v,2),next(L["pos_now"] for L in legs if L["key"]==k)] for k,v in leg_usd.items()),
               key=lambda x:x[1])
n_open_now=sum(1 for L in legs if L["pos_now"]!=0)
month=dict(window="2026-06-01..now",pool_usd=POOL,book_usd=round(book_usd,2),
           ret_pct=round(book_usd/POOL*100,2),n_open=n_open_now,
           gross_usd=gross_usd,slip_cost_usd=slip_cost,funding_cost_usd=fund_cost,
           method="real engine replay (postrace 4h/1h, ETH-gated, POOL/n_open ff-sizing); slip+funding attributed by book-level differencing",
           per_leg_usd={k:round(v,2) for k,v in leg_usd.items()})
sp=os.path.join(STATE_DIR,"state.json")
st=json.load(open(sp)) if os.path.exists(sp) else {}
st["month"]=month; json.dump(st,open(sp,"w"),indent=1)
print(f"[JUNE] Intraday book ${book_usd:,.2f} ({month['ret_pct']:+.2f}% on ${POOL:,.0f}) "
      f"n_open={n_open_now} bars={nbars}")
print(f"   gross ${gross_usd:,.2f}  - slip ${slip_cost:,.2f}  - funding ${fund_cost:,.2f}  = net ${book_usd:,.2f}")
for k,v,p in per_leg:
    if abs(v)>=0.01: print(f"   {k:16} ${v:+8.2f}  pos_now={p:+d}")
