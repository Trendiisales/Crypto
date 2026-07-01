#!/usr/bin/env python3
# IBKRCrypto INTRADAY SHADOW LEDGER (S-2026-06-29) -- sibling of refresh_shadow.py for the
# 1h/4h trend stack. SEPARATE state dir (data/ibkrcrypto_intraday/) so the proven daily book is
# never touched. Same faithful-flip accounting; every leg tagged with its IBKR venue + cost (bps)
# so the operator can read the perp-vs-ladder comparison directly off the book.
#
# WHY this exists: intraday turns x per-turn fee kills mean-reversion + high-turn engines; only the
# trend library survives. Per-symbol/per-venue re-stress (2026-06-29) -> the cheap LADDER (BTC MBT 6bps,
# ETH MET 8bps, SOL-fut 2bps) preserves edge; the SQF PERP (BTC 22, ETH 28bps) drags most engines sub-1.
# Operator asked to run BOTH venues in shadow so the drag is measured, not assumed. SOL perp (QSOL SQF)
# is announced but NOT on the IB account yet -> SOL ladder-only for now.
#
# Companion protection is NOT in-book here: the external stall_accountant.py mirror (its own intraday
# instance, CRYPTO=this state.json) does the giveback-clip / stall / re-lock, matching the Omega design
# (real leg rides wide; companion harvests the giveback in a mirrored book). See cron.
import json, os, subprocess, csv, datetime, time
HERE=os.path.dirname(os.path.abspath(__file__)); BT=os.path.join(HERE,"ibkrcrypto_bt")
_DD=os.environ.get("IBKRCRYPTO_DATADIR", os.path.join(HERE,"data","ibkrcrypto_intraday"))
os.makedirs(_DD, exist_ok=True)
STATE=os.path.join(_DD,"state.json"); LEDGER=os.path.join(_DD,"ledger.csv")
INBOUND=os.path.join(_DD,"crypto_inbound.csv")   # -> Omega ledger (scp'd to VPS, separate file)
H4=14400000; H1=3600000
def D(sym,tf): return f"{HERE}/data/{sym}USDT_{tf}.csv"
B4,E4,S4=D("BTC","4h"),D("ETH","4h"),D("SOL","4h")
B1,E1,S1=D("BTC","1h"),D("ETH","1h"),D("SOL","1h")
# (key, sym, csvf, tf_ms, cost_bps, strat, mult, dsym, dstrat)
ROSTER=[
 # --- BTC: ladder MBT(6) vs perp QTF-SQF(22), 4h ---
 ("btc_emax_mbt","BTC",B4,H4, 6,"EMAx",   0.01,"BTC MBT micro (ladder ~6bps)","Trend EMAx 4h"),
 ("btc_kelt_mbt","BTC",B4,H4, 6,"Kelt",   0.01,"BTC MBT micro (ladder ~6bps)","Trend Kelt 4h"),
 ("btc_emax_sqf","BTC",B4,H4,22,"EMAx",   0.01,"BTC QTF SQF perp (~22bps)","Trend EMAx 4h"),
 ("btc_kelt_sqf","BTC",B4,H4,22,"Kelt.vt",0.01,"BTC QTF SQF perp (~22bps)","Trend Kelt.vt 4h"),
 # --- ETH: ladder MET(8) vs perp QEF-SQF(28), 4h ---
 ("eth_emax_met","ETH",E4,H4, 8,"EMAx",   0.20,"ETH MET micro (ladder ~8bps)","Trend EMAx 4h"),
 ("eth_kelt_met","ETH",E4,H4, 8,"Kelt",   0.20,"ETH MET micro (ladder ~8bps)","Trend Kelt 4h"),
 ("eth_ichi_met","ETH",E4,H4, 8,"Ichi",   0.20,"ETH MET micro (ladder ~8bps)","Trend Ichi 4h"),
 ("eth_tsmom_met","ETH",E4,H4,8,"TSMom50",0.20,"ETH MET micro (ladder ~8bps)","Trend TSMom50 4h"),
 ("eth_macd_met","ETH",E4,H4, 8,"Macd",   0.20,"ETH MET micro (ladder ~8bps)","Trend Macd 4h"),
 ("eth_donch40_met","ETH",E4,H4,8,"Donch40",0.20,"ETH MET micro (ladder ~8bps)","Breakout Donch40 4h"),
 ("eth_emax_sqf","ETH",E4,H4,28,"EMAx",   0.20,"ETH QEF SQF perp (~28bps)","Trend EMAx 4h"),
 ("eth_kelt_sqf","ETH",E4,H4,28,"Kelt.vt",0.20,"ETH QEF SQF perp (~28bps)","Trend Kelt.vt 4h"),
 ("eth_ichi_sqf","ETH",E4,H4,28,"Ichi.vt",0.20,"ETH QEF SQF perp (~28bps)","Trend Ichi.vt 4h"),
 ("eth_tsmom_sqf","ETH",E4,H4,28,"TSMom50",0.20,"ETH QEF SQF perp (~28bps)","Trend TSMom50 4h"),
 ("eth_macd_sqf","ETH",E4,H4,28,"Macd.vt",0.20,"ETH QEF SQF perp (~28bps)","Trend Macd.vt 4h"),
 # Donch40 = ONLY non-EMAx strat that BEATS the perp wall ([[DonchianPerpBreakout]]):
 # rare-fire breakout -> low cost drag -> perp 4.18 ~ ladder 4.47 Calmar. LIVE (not dead .vt).
 ("eth_donch40_sqf","ETH",E4,H4,28,"Donch40",0.20,"ETH QEF SQF perp (~28bps)","Breakout Donch40 4h"),
 # --- SOL: ladder SOL-fut(2) only (QSOL SQF not on account yet), 4h ---
 ("sol_emax_fut","SOL",S4,H4, 2,"EMAx",   5.00,"SOL fut (ladder ~2bps; QSOL pending)","Trend EMAx 4h"),
 ("sol_kelt_fut","SOL",S4,H4, 2,"Kelt.vt",5.00,"SOL fut (ladder ~2bps; QSOL pending)","Trend Kelt.vt 4h"),
 ("sol_ichi_fut","SOL",S4,H4, 2,"Ichi",   5.00,"SOL fut (ladder ~2bps; QSOL pending)","Trend Ichi 4h"),
 ("sol_tsmom_fut","SOL",S4,H4,2,"TSMom50",5.00,"SOL fut (ladder ~2bps; QSOL pending)","Trend TSMom50 4h"),
 ("sol_macd_fut","SOL",S4,H4, 2,"Macd",   5.00,"SOL fut (ladder ~2bps; QSOL pending)","Trend Macd 4h"),
 # --- 1h EMAx survivors (only slow EMAx clears the 1h cost wall) ---
 ("eth_emax_1h","ETH",E1,H1, 8,"EMAx",   0.20,"ETH MET micro 1h (ladder ~8bps)","Trend EMAx 1h"),
 ("sol_emax_1h","SOL",S1,H1, 2,"EMAx",   5.00,"SOL fut 1h (ladder ~2bps)","Trend EMAx 1h"),
 # SOL-1h Donch40 = best-sampled new edge ([[DonchianPerpBreakout]]): ladder 4.66 / perp 4.35,
 # DD 26%, OOS 3.06 (841 bars). Ladder-only for now (SOL perp = QSOL pending on IB acct).
 ("sol_donch40_1h","SOL",S1,H1, 2,"Donch40",5.00,"SOL fut 1h (ladder ~2bps; QSOL pending)","Breakout Donch40 1h"),
]
def _unix(s):
    try: return int(datetime.datetime.strptime(s,"%Y-%m-%d %H:%M").replace(tzinfo=datetime.timezone.utc).timestamp())
    except Exception: return 0
# REGIME GATE (S-2026-06-30, [[PerpTrendRegimeGate]]): TRUE 200-DAY gate, tf-scaled to bars
# (4h->1200, 1h->4800). INTRADAY is mixed -> ETH-ONLY: the 4h gate is a clean win on ETH EMAx+TSMom50
# (every window better, bear -17%/+44%->+58%/+58%, OOS/FULL DD ~halved) but on BTC/SOL it helps the
# bear yet WORSENS FULL DD (4h whipsaws the MA boundary) -> BTC/SOL intraday left UNGATED per
# BACKTEST_TRUTH (don't ship a worse-DD result). Daily book gates all symbols (refresh_shadow.py).
# Kelt.vt already robust -> never gated. REGIME_GATE_MA_DAYS=0 disables.
REGIME_GATE_MA_DAYS = int(os.environ.get("REGIME_GATE_MA_DAYS","200"))
# ETH Ichi ADDED S-2026-06-30: faithful full-history harness -> gate net 1.841->1.764 (noise),
# maxDD -21% (1.268->1.001), ret/DD 1.45->1.76, June better -> clean DD win, no edge damage.
# (SOL Ichi NOT gated: gate costs 22% of its +5.83 full-history net -> damages the edge.)
GATED_INTRADAY      = {("ETH","EMAx"),("ETH","TSMom50"),("ETH","Ichi")}
# live-fidelity cost overlays (see refresh_shadow.py): per-symbol slippage + real perp-funding
# proxy for the SQF daily TFA basis (daily-bucketed funding maps onto each leg's midnight bar).
SLIP_BPS={"BTC":3,"ETH":3,"SOL":5}
FUND_DIR=os.path.join(HERE,"data","funding")
def _cost_env(sym,base):
    base["SLIPBPS"]=str(SLIP_BPS.get(sym,3))
    fc=os.path.join(FUND_DIR,f"{sym}.csv")
    if os.path.exists(fc): base["FUNDCSV"]=fc; base["USE_REAL_FUND"]="1"
    return base
def signal(sym,csvf,tf_ms,cost,strat):
    # returns (target,size,px,exit_px). exit=0 when the binary emits none -> GUI shows "on-turn".
    rma = round(REGIME_GATE_MA_DAYS*86400000/tf_ms) if (sym,strat) in GATED_INTRADAY else 0
    try:
        out=subprocess.run([BT,sym,csvf,"--signal",strat],capture_output=True,text=True,
                           env=_cost_env(sym,dict(os.environ,COSTBPS=str(cost),BT_TF_MS=str(tf_ms),REGIME_MA=str(rma))),timeout=40).stdout
        for ln in out.splitlines():
            if "target=" in ln:
                ex=0.0
                if "exit=" in ln: ex=float(ln.split("exit=")[1].split()[0])
                return (int(ln.split("target=")[1].split()[0]),
                        float(ln.split("size=")[1].split()[0]),
                        float(ln.split("px=")[1].split()[0]), ex)
    except Exception as e: print("sig err",sym,strat,e)
    return 0,1.0,0.0,0.0
def gate(csvf):
    return subprocess.run(["python3",f"{HERE}/integrity_gate.py",csvf],capture_output=True).returncode==0
# FRESHNESS GUARD (24/7 crypto, intraday): a 4h feed must be <~14h old, a 1h feed <~5h. Stale -> FREEZE
# (hold prior pos, book NO flip off a stale bar) + flag. tf-scaled so each cadence has its own bound.
def max_age_days(tf_ms): return (tf_ms/86400000.0)*3.0 + 0.1   # 4h->0.6d(~14h); 1h->0.225d(~5.4h)
def data_age_days(csvf):
    try: fage=(time.time()-os.path.getmtime(csvf))/86400.0
    except Exception: return (float("inf"),float("inf"))
    last_ts=None
    try:
        rows=open(csvf).read().strip().splitlines()
        for ln in reversed(rows):
            c=ln.split(",")[0].strip()
            if c and (c[0].isdigit() or c[0]=="-"):
                v=float(c)
                if v>1e12: v/=1000.0
                last_ts=v; break
    except Exception: last_ts=None
    bage=(time.time()-last_ts)/86400.0 if last_ts else float("inf")
    return (bage,fage)
def fresh_ok(csvf,tf_ms):
    bage,fage=data_age_days(csvf); m=max_age_days(tf_ms)
    return (bage<=m and fage<=m), bage, fage

prior={}; closed=[]; prior_meta={}
if os.path.exists(STATE):
    try:
        d=json.load(open(STATE)); prior={s.get("key"):s for s in d.get("slots",[])}; closed=d.get("closed",[]); prior_meta=d
    except Exception: pass
now=datetime.datetime.now(datetime.timezone.utc).strftime("%Y-%m-%d %H:%M")
newled=not os.path.exists(LEDGER); led=open(LEDGER,"a")
newinb=not os.path.exists(INBOUND); inb=open(INBOUND,"a")
if newinb: inb.write("id,entry_ts,exit_ts,sym,strat,side,entry,exit,net_usd\n")
if newled: led.write("ts,key,event,pos,px,realized_pct,cum_pct\n")
POOL=float(os.environ.get("POOL_USD","10000"))  # separate intraday allocation $10k (2026-06-30, was $5k; operator-overridable)
# CORRELATION-DOWNSIZE (mirrors daily book): halve a leg whose rolling return has gone correlated with
# the rest of the book (drag-leg fix; a sizing change, not a stop -> keeps the trend edge). CORR_THR<=0 off.
CORR_WIN  =int(os.environ.get("CORR_WIN","30"))
CORR_THR  =float(os.environ.get("CORR_THR","0.5"))
CORR_SCALE=float(os.environ.get("CORR_SCALE","0.5"))
def _eqret(sym,csvf,tf_ms,strat):
    try:
        out=subprocess.run([BT,sym,csvf,"--equity",strat],capture_output=True,text=True,
                           env=_cost_env(sym,dict(os.environ,BT_TF_MS=str(tf_ms))),timeout=40).stdout
        d={}
        for ln in out.splitlines():
            if ln[:1].isdigit(): ts,eq=ln.split(","); d[int(ts)//86400000]=float(eq)  # day-bucket corr (coarse OK)
        return d
    except Exception: return {}
def corr_downsize(legs):
    if CORR_THR<=0: return {L["key"]:(0.0,1.0) for L in legs}
    cur={L["key"]:_eqret(L["sym"],L["csvf"],L["tf_ms"],L["strat"]) for L in legs}
    days=sorted(set().union(*[set(c) for c in cur.values()]) or {0})[-(CORR_WIN+1):]
    ret={}
    for L in legs:
        ck=cur[L["key"]]; prev=None; rs=[]
        for dd in days:
            v=ck.get(dd, prev if prev is not None else 0.0)
            if prev is not None: rs.append(v-prev)
            prev=v
        ret[L["key"]]=rs
    n=min((len(r) for r in ret.values()), default=0)
    def pear(a,b):
        m=len(a)
        if m<5: return 0.0
        ma=sum(a)/m; mb=sum(b)/m
        ca=sum((a[i]-ma)*(b[i]-mb) for i in range(m))
        va=(sum((x-ma)**2 for x in a))**.5; vb=(sum((x-mb)**2 for x in b))**.5
        return ca/(va*vb) if va>0 and vb>0 else 0.0
    out={}
    for L in legs:
        rest=[sum(ret[O["key"]][i] for O in legs if O["key"]!=L["key"]) for i in range(n)]
        c=pear(ret[L["key"]][:n], rest)
        out[L["key"]]=(c, CORR_SCALE if c>CORR_THR else 1.0)
    return out
# pass 1: resolve every leg's signal so we can count open legs
legs=[]
for key,sym,csvf,tf_ms,cost,strat,mult,dsym,dstrat in ROSTER:
    fresh,bage,fage=fresh_ok(csvf,tf_ms); integ=gate(csvf); p0=prior.get(key,{})
    if not fresh:
        print(f"[STALE-DATA] {key} {sym} {os.path.basename(csvf)} bar_age={bage:.2f}d file_age={fage:.2f}d "
              f"(max {max_age_days(tf_ms):.2f}d) -- FROZEN, no trade booked off stale data", flush=True)
        t,sz,px,expx = p0.get("pos",0),1.0,(p0.get("px") or p0.get("entry_px") or 0.0),0.0
    elif not integ:
        t,sz,px,expx = 0,1.0,0.0,0.0
    else:
        t,sz,px,expx = signal(sym,csvf,tf_ms,cost,strat)
    legs.append(dict(key=key,sym=sym,csvf=csvf,tf_ms=tf_ms,cost=cost,strat=strat,mult=mult,dsym=dsym,
                     dstrat=dstrat,clean=(fresh and integ),fresh=fresh,integ=integ,
                     bar_age=round(bage,2),file_age=round(fage,2),t=t,sz=sz,px=px,expx=expx))
# KILL_FLAT marker (GUI kill button): force every leg flat. t=0 for all legs -> pass 2's
# flip-close path books each open leg out at last mark (reuses the PnL math). Durable: kept
# flat every run while the marker exists; operator deletes _DD/KILL_FLAT to resume.
KILLED=os.path.exists(os.path.join(_DD,"KILL_FLAT"))
if KILLED:
    for L in legs: L["t"]=0
n_open=sum(1 for L in legs if L["t"]!=0); per_leg=POOL/max(1,n_open)
cmap=corr_downsize(legs)
def qty_for(px_basis,mult,scale=1.0):
    pc=px_basis*mult
    return max(1,round(per_leg*scale/pc)) if pc>0 else 1
# pass 2: size + P&L
slots=[]; tot_real=0.0; tot_unreal=0.0; ntrades=0; tot_real_usd=0.0; tot_unreal_usd=0.0; deployed=0.0
for L in legs:
    key,sym,cost,strat,mult,dsym,dstrat=L["key"],L["sym"],L["cost"],L["strat"],L["mult"],L["dsym"],L["dstrat"]
    t,sz,px,expx=L["t"],L["sz"],L["px"],L["expx"]; clean=L["clean"]
    p=prior.get(key,{}); pos0=p.get("pos",0); epx=p.get("entry_px",px) or px
    ets=p.get("entry_ts"); cum=p.get("realized_pct",0.0); c=(cost+2)/10000.0
    cum_usd=p.get("realized_usd",0.0)
    corr,scale=cmap.get(key,(0.0,1.0))
    qty=qty_for(epx if (t!=0 and epx>0) else px, mult, scale)
    if t!=pos0:                                            # ---- TRADE (flip) ----
        if pos0!=0 and epx>0:                              # close old leg -> realized
            q0=qty
            r=(pos0*(px-epx)/epx*sz - c*sz)*100; cum+=r; ntrades+=1
            r_usd=pos0*(px-epx)*mult*q0 - (cost+2)/10000.0*px*mult*q0; cum_usd+=r_usd
            closed.insert(0,{"sym":dsym,"strat":dstrat,"dir":("LONG" if pos0>0 else "SHORT"),
                             "entry_ts":ets,"exit_ts":now,"entry_px":round(epx,4),"exit_px":round(px,4),
                             "contracts":q0,"realized_pct":round(r,3),"realized_usd":round(r_usd,2),"mult":mult})
            led.write(f"{now},{key},CLOSE {pos0},{t},{px:.4f},{r:.3f},{cum:.3f}\n")
            uid=f"{key}_{_unix(now)}"; side="LONG" if pos0>0 else "SHORT"
            inb.write(f"{uid},{_unix(ets)},{_unix(now)},{sym},{strat},{side},{epx:.4f},{px:.4f},{r_usd:.2f}\n"); inb.flush()
        if t!=0:
            epx=px; ets=now; qty=qty_for(epx,mult,scale)
            led.write(f"{now},{key},OPEN {t},{t},{px:.4f},,{cum:.3f}\n"); ntrades+=1
    unreal=(t*(px-epx)/epx*sz*100) if (t!=0 and epx>0) else 0.0
    unreal_usd=(t*(px-epx)*mult*qty) if (t!=0 and epx>0) else 0.0
    if t!=0: deployed+=px*mult*qty
    tot_real+=cum; tot_unreal+=unreal; tot_real_usd+=cum_usd; tot_unreal_usd+=unreal_usd
    slots.append({"key":key,"sym":dsym,"strat":dstrat,"pos":t,"contracts":(qty if t else 0),
                  "vt":round(sz,2),"mult":mult,"pool":POOL,"tf_ms":L["tf_ms"],"cost_bps":cost,
                  "corr":round(corr,2),"downsized":(scale<1.0 and t!=0),
                  "notional":round(px*mult*qty,0) if t else 0,
                  "px":round(px,4),"entry_px":round(epx,4),"entry_ts":ets,"exit_px":round(expx,4),
                  "unreal_pct":round(unreal,3),"unreal_usd":round(unreal_usd,2),
                  "realized_pct":round(cum,3),"realized_usd":round(cum_usd,2),"clean":clean,
                  "fresh":L["fresh"],"stale":(not L["fresh"]),"bar_age_d":L["bar_age"],"file_age_d":L["file_age"]})
led.close(); inb.close(); closed=closed[:25]
# book-level data health
sources={}
for L in legs:
    b=os.path.basename(L["csvf"])
    if b not in sources: sources[b]={"bar_age_d":L["bar_age"],"file_age_d":L["file_age"],"max_age_d":round(max_age_days(L["tf_ms"]),2),"fresh":L["fresh"]}
stale_sources=sorted(b for b,v in sources.items() if not v["fresh"])
FLAG=os.path.join(_DD,"stale_data.flag")
try:
    if stale_sources: open(FLAG,"w").write(now+" UTC STALE: "+", ".join(stale_sources)+"\n")
    elif os.path.exists(FLAG): os.remove(FLAG)
except Exception: pass
if stale_sources: print(f"[STALE-DATA] BOOK NOT ALL-FRESH -- frozen sources: {stale_sources}", flush=True)
json.dump({"engine":"IBKRCrypto-Intraday","mode":"SHADOW","updated":now+" UTC",
           "data_health":{"all_fresh":(len(stale_sources)==0),"stale_sources":stale_sources,"sources":sources},
           "open_unreal_pct":round(tot_unreal,2),"realized_pct":round(tot_real,2),
           "total_pct":round(tot_unreal+tot_real,2),
           "open_unreal_usd":round(tot_unreal_usd,2),"realized_usd":round(tot_real_usd,2),
           "total_usd":round(tot_unreal_usd+tot_real_usd,2),
           "pool_usd":POOL,"deployed_usd":round(deployed,0),
           "n_open":sum(1 for s in slots if s["pos"]),"killed":KILLED,
           "slots":slots,"closed":closed}, open(STATE,"w"), indent=1)
print(f"refresh-intraday: {ntrades} new trade-events, {n_open} open, unreal ${tot_unreal_usd:.2f} ({tot_unreal:.2f}%) realized ${tot_real_usd:.2f}")
