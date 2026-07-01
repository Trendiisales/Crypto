#!/usr/bin/env python3
# IBKRCrypto SHADOW LEDGER (Omega-style): paper-as-real executor.
# A "trade" = a position FLIP (engine target changes). On flip: close old leg (bank
# realized P&L at full cost) + open new (record entry px/time). Open legs show live
# unrealized P&L (mark-to-market at latest px). Closed trades kept with realized P&L.
import json, os, subprocess, csv, datetime, time, urllib.request
HERE=os.path.dirname(os.path.abspath(__file__)); BT=os.path.join(HERE,"ibkrcrypto_bt")
_DD=os.environ.get("IBKRCRYPTO_DATADIR", os.path.join(HERE,"data","ibkrcrypto"))  # override for safe testing
STATE=os.path.join(_DD,"state.json")
LEDGER=os.path.join(_DD,"ledger.csv")
INBOUND=os.path.join(_DD,"crypto_inbound.csv")  # -> Omega ledger (scp'd to VPS)
def _unix(s):
    try: return int(datetime.datetime.strptime(s,"%Y-%m-%d %H:%M").replace(tzinfo=datetime.timezone.utc).timestamp())
    except Exception: return 0
TICK="/Users/jo/Tick"
BTCF=f"{HERE}/data/BTCUSDT_1d.csv"; ETHF=f"{HERE}/data/ETHUSDT_1d.csv"; SOLF=f"{HERE}/data/SOLUSDT_1d.csv"; NDXF=f"{TICK}/NDX_daily_2016_2026.csv"
ROSTER=[
 ("eth_emax","ETH",ETHF,28,"EMAx",0.20,"ETH Spot-Quoted (QEF)","Trend (EMAx)"),
 ("eth_kelt","ETH",ETHF,28,"Kelt",0.20,"ETH Spot-Quoted (QEF)","Trend (Keltner)"),
 ("btc_emax","BTC",BTCF,14,"EMAx",0.01,"BTC Spot-Quoted (QTF)","Trend (EMAx)"),
 ("btc_kelt","BTC",BTCF,14,"Kelt",0.01,"BTC Spot-Quoted (QTF)","Trend (Keltner)"),
 ("sol_emax","SOL",SOLF,11,"EMAx",5.00,"SOL (fut, SQF pending)","Trend (EMAx)"),
 ("sol_kelt","SOL",SOLF,11,"Kelt",5.00,"SOL (fut, SQF pending)","Trend (Keltner)"),
 ("ndx_tsmom","NDX",NDXF,4,"TSMom50",0.10,"Nasdaq-100 SQF (QNDX)","Trend (TSMom50)"),
 ("btc_reg","BTC",BTCF,14,"Regime",0.01,"BTC Spot-Quoted (QTF)","Regime-switch"),
 ("eth_reg","ETH",ETHF,28,"Regime",0.20,"ETH Spot-Quoted (QEF)","Regime-switch"),
 ("sol_reg","SOL",SOLF,11,"Regime",5.00,"SOL (fut, SQF pending)","Regime-switch"),
 ("btc_ibs","BTC",BTCF,14,"IBS",0.01,"BTC Spot-Quoted (QTF)","Mean-Rev (IBS)"),
 ("sol_ibs","SOL",SOLF,11,"IBS",5.00,"SOL (fut, SQF pending)","Mean-Rev (IBS)"),
 ("ndx_rsir","NDX",NDXF,4,"RSIrev",0.10,"Nasdaq-100 SQF (QNDX)","MeanRev (RSIrev)"),
 # Roc20 momentum (S-2026-06-30 daily sweep): fat-tail trend rider, OOS-robust on BTC/SOL
 # (BTC OOS PF1.67 +166%, SOL OOS 1.92 +219%); ETH OOS marginal (1.00) -> NOT wired. Bear-weak
 # (BTC 2022 0.84) -> GATED on own SMA200 like the other trend legs.
 ("btc_roc","BTC",BTCF,14,"Roc",0.01,"BTC Spot-Quoted (QTF)","Momentum (Roc20)"),
 ("sol_roc","SOL",SOLF,11,"Roc",5.00,"SOL (fut, SQF pending)","Momentum (Roc20)"),
]
# REGIME GATE (S-2026-06-30, [[PerpTrendRegimeGate]]): per-symbol close>SMA(REGIME_GATE_MA)
# gate on the DAILY TREND legs only. Faithful BT: ~halves FULL/bear DD and flips the 2022
# sustained bear flat/loss->positive on EMAx+TSMom50, all of BTC/ETH/SOL (monotone MA 0/100/200
# = robust shoulder, matches the 200DMA convention). Each name gates on its OWN close vs its OWN
# SMA200 (NOT BTC-gated -> [[MacroGateAltDecoupling]]). Kelt/IBS/Regime/RSIrev are NOT gated
# (Kelt already robust; daily MR is a separate live question). REGIME_GATE_MA=0 disables.
REGIME_GATE_MA = int(os.environ.get("REGIME_GATE_MA","200"))
GATED_STRATS   = {"EMAx","TSMom50","Roc"}
# live-fidelity cost overlays (so the forward ledger pays what a real IBKR switch would):
#  SLIP_BPS = adverse-fill slippage beyond half-spread per round trip (SOL thinner). NDX liquid.
#  FUND_DIR = Binance USDT-perp funding = REAL-financing PROXY for the CME SQF daily TFA basis
#             (not entitled to real SQF basis yet). NDX equity SQF -> flat ANNUAL_CARRY, no proxy.
SLIP_BPS={"BTC":3,"ETH":3,"SOL":5,"NDX":1}
FUND_DIR=os.path.join(HERE,"data","funding")
def _cost_env(sym,base):
    base["SLIPBPS"]=str(SLIP_BPS.get(sym,3))
    fc=os.path.join(FUND_DIR,f"{sym}.csv")
    if os.path.exists(fc): base["FUNDCSV"]=fc; base["USE_REAL_FUND"]="1"
    return base
def signal(sym,csvf,cost,strat):
    # returns (target, size, px, exit_px). exit_px=0 when the binary doesn't emit one
    # (pre-rebuild) or the engine has no flip level in range -> GUI shows "on-turn".
    rma = REGIME_GATE_MA if strat in GATED_STRATS else 0
    try:
        out=subprocess.run([BT,sym,csvf,"--signal",strat],capture_output=True,text=True,
                           env=_cost_env(sym,dict(os.environ,COSTBPS=str(cost),REGIME_MA=str(rma))),timeout=30).stdout
        for ln in out.splitlines():
            if "target=" in ln:
                ex=0.0
                if "exit=" in ln: ex=float(ln.split("exit=")[1].split()[0])
                return (int(ln.split("target=")[1].split()[0]),
                        float(ln.split("size=")[1].split()[0]),
                        float(ln.split("px=")[1].split()[0]), ex)
    except Exception as e: print("sig err",sym,strat,e)
    return 0,1.0,0.0,0.0
def gate(csvf,cost):
    return subprocess.run(["python3",f"{HERE}/integrity_gate.py",csvf],capture_output=True).returncode==0

# ── S-2026-06-25 STALE-DATA GUARD (operator-mandated: NEVER trade off a stale price).
# integrity_gate.py only checks CORRECTNESS (OHLC valid, monotonic, >=60 bars) -- a series
# that simply ENDS 12 days ago passes it. This adds the missing FRESHNESS check: a source is
# fresh only if BOTH its newest bar AND the file mtime are within max_age_days of now. A stale
# leg is FROZEN (prior position held, NO new signal/flip booked off stale data) + alarmed +
# flagged RED, instead of silently trading a 12-day-old NDX close. Root cause this closes:
# /Users/jo/Tick/NDX_daily_2016_2026.csv producer died 2026-06-15, nothing detected it.
MAX_AGE_DAYS = {"NDX": 4.0}   # weekday index: Fri close -> Tue (incl. a Mon holiday) ~= 4d
def max_age_for(sym): return MAX_AGE_DAYS.get(sym, 2.5)   # crypto is 24/7 -> newest bar always <=~1d

def data_age_days(csvf):
    """(bar_age_days, file_age_days): age of the NEWEST bar + the file mtime, in days. inf on error/missing."""
    import time
    try: fage=(time.time()-os.path.getmtime(csvf))/86400.0
    except Exception: return (float("inf"), float("inf"))
    last_ts=None
    try:
        with open(csvf) as fh: rows=fh.read().strip().splitlines()
        for ln in reversed(rows):
            c=ln.split(",")[0].strip()
            if c and (c[0].isdigit() or c[0]=="-"):
                v=float(c)
                if v>1e12: v/=1000.0      # ms epoch (crypto open_time_ms) -> seconds
                last_ts=v; break
    except Exception: last_ts=None
    bage=(time.time()-last_ts)/86400.0 if last_ts else float("inf")
    return (bage, fage)

def fresh_ok(csvf,sym):
    bage,fage=data_age_days(csvf); m=max_age_for(sym)
    return (bage<=m and fage<=m), bage, fage

prior={}; closed=[]; prior_meta={}
if os.path.exists(STATE):
    try:
        d=json.load(open(STATE)); prior={s.get("key"):s for s in d.get("slots",[])}; closed=d.get("closed",[]); prior_meta=d
    except: pass
clipped=dict(prior_meta.get("clipped",{}))   # key -> clipped direction (+1/-1); held flat until signal flips off it
now=datetime.datetime.now(datetime.timezone.utc).strftime("%Y-%m-%d %H:%M")
newled=not os.path.exists(LEDGER); led=open(LEDGER,"a")
newinb=not os.path.exists(INBOUND); inb=open(INBOUND,"a")    # Omega-bound closed-trade feed
if newinb: inb.write("id,entry_ts,exit_ts,sym,strat,side,entry,exit,net_usd\n")
if newled: led.write("ts,key,event,pos,px,realized_pct,cum_pct\n")
# Position sizing: a single $ POOL (operator-set) split equally across the OPEN legs.
# per-leg target = POOL / n_open ; contracts = round to hit that at entry px (min 1 lot).
# $ P&L = contracts * mult * price-move. Override via env POOL_USD. n_open varies as
# signals flip, so each leg's size rebalances. (min 1 lot can push total slightly past
# the pool when a big-lot leg like NDX is open — actual deployed is reported.)
POOL=float(os.environ.get("POOL_USD","10000"))  # operator allocation $10k (2026-06-29, was $5k)
# ── COMPANION PROTECTION (S-2026-06-29): make the giveback/dead-money cut REAL in the book.
# Was shadow-only (giveback_saver/stall_accountant accounted clips the book ignored -> losers hung
# forever). These force-CLOSE a held leg. NOT a price-stop (behaviour-based: peak-giveback / stall /
# dead-money), so it keeps the trend edge while killing dead trades + locking gains. Env-tunable;
# PROTECT default ON (operator: "enable it"). BACKTEST tune via ibkrcrypto_bt before trusting params.
# DISABLED 2026-06-29 (operator design correction): the REAL crypto leg must ride wide (keep the
# edge); giveback/stall/fail clipping is the COMPANION engine's job (stall_accountant.py, its own
# mirrored book) -> real engine untouched. This in-book close is RESERVED for a CONFIRMED-REVERSAL
# safe-stop (to be built + backtested). Default OFF; flip COMPANION_PROTECT=1 only for that.
PROTECT       = os.environ.get("COMPANION_PROTECT","0") != "0"
PROT_GATE_USD = float(os.environ.get("PROT_GATE_USD","25"))     # leg must reach +$ this to ARM profit-lock
PROT_TRAIL    = float(os.environ.get("PROT_TRAIL","0.40"))      # REVERSAL: give back >= this frac of peak
PROT_STALL_SEC= float(os.environ.get("PROT_STALL_SEC","86400")) # STALL: armed leg makes no new peak this long (24h)
PROT_DEAD_SEC = float(os.environ.get("PROT_DEAD_SEC","259200")) # DEAD-MONEY: never-armed leg open this long (3d)
PROT_DEAD_USD = float(os.environ.get("PROT_DEAD_USD","-20"))    # ...and currently below this $ -> cut the bleeder
# ── BE-RATCHET on the NDX index-trend leg only (S-2026-06-30, [[NdxCompanionClip]]) ──
# NDX (TSMom50) is the ONE leg where a break-even ratchet HELPS. It is NOT the giveback
# companion -- that clip DESTROYS this trend runner (faithful ibkrcrypto_bt: 5% giveback
# +59% PF1.16 maxDD WORSE 38.9->44.7, churns 76->350 trades). The engine-native BE-ratchet
# instead cuts green->red round-trips WITHOUT clipping runners: BE_ARM=0.015/BE_FLOOR=0 ->
# +95.6% PF1.78 vs wide +87% PF1.72, and better OOS (PF2.65 DD8.6%). NDX-only, default ON.
NDX_BE       = os.environ.get("NDX_BE_RATCHET","1") != "0"
NDX_BE_ARM   = float(os.environ.get("NDX_BE_ARM","0.015"))   # arm once favorable return frac >= this
NDX_BE_FLOOR = float(os.environ.get("NDX_BE_FLOOR","0.0"))   # exit when favorable return frac <= this after armed
# CORRELATION-DOWNSIZE (S-2026-06-24, dragleg_study.py): the drag-leg failure mode is a
# leg that STOPS DIVERSIFYING -- when its rolling return correlates with the book it
# compounds the book's drawdown instead of offsetting it. Downsizing such a leg (NOT
# stopping it -- stops kill the trend edge) was the best fix: book maxDD ~halved, Sharpe
# 0.92->1.00 on the real 13-engine curves. It's a SIZING change so it fits the vol-target
# design pillar. Each refresh: leg's trailing-CORR_WIN-day corr to the book; corr>thr ->
# size * CORR_SCALE. Env-overridable; CORR_THR<=0 disables.
CORR_WIN  =int(os.environ.get("CORR_WIN","30"))
CORR_THR  =float(os.environ.get("CORR_THR","0.5"))
CORR_SCALE=float(os.environ.get("CORR_SCALE","0.5"))
def _eqret(sym,csvf,strat):                      # trailing daily returns from --equity (cum sum-ret -> delta)
    try:
        out=subprocess.run([BT,sym,csvf,"--equity",strat],capture_output=True,text=True,
                           env=_cost_env(sym,dict(os.environ)),timeout=30).stdout
        d={}
        for ln in out.splitlines():
            if ln[:1].isdigit(): ts,eq=ln.split(","); d[int(ts)//86400000]=float(eq)
        return d
    except Exception: return {}
def corr_downsize(legs):
    if CORR_THR<=0: return {L["key"]:(0.0,1.0) for L in legs}
    cur={L["key"]:_eqret(L["sym"],L["csvf"],L["strat"]) for L in legs}
    days=sorted(set().union(*[set(c) for c in cur.values()]) or {0})[-(CORR_WIN+1):]
    ret={}
    for L in legs:                                # per-leg daily return over the window (ffill cum, delta)
        ck=cur[L["key"]]; prev=None; rs=[]
        for d in days:
            v=ck.get(d, prev if prev is not None else 0.0)
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
    for L in legs:                                # LEAVE-ONE-OUT: corr of leg vs the REST of the book
        rest=[sum(ret[O["key"]][i] for O in legs if O["key"]!=L["key"]) for i in range(n)]
        c=pear(ret[L["key"]][:n], rest)
        out[L["key"]]=(c, CORR_SCALE if c>CORR_THR else 1.0)
    return out
# S-2026-06-26 LIVE INDEX MARK from OUR IBKR FEED (operator: use IBKR, NOT Yahoo). The Omega gateway
# (4001) streams live NQ into C:\Omega\logs\aurora\aurora_NQ.json (refreshes ~minutely). We read that
# live NQ FUTURE and convert it to the leg's cash (^NDX) scale via a BASIS ratio anchored when the daily
# CSV rolls (cash/NQ captured ~the same moment), then ride NQ intraday. The daily SIGNAL still runs off
# the CSV. NO Yahoo. Reject a >30min-stale NQ. Fallback: daily CSV close (stale-but-honest).
_live_cache={}
def _read_live_nq():
    c=_live_cache.get("nq")
    if c and (time.time()-c[0])<50: return c[1]
    try:
        out=subprocess.run(["ssh","-o","ConnectTimeout=6","-o","BatchMode=yes","omega-vps",
            'powershell -NoProfile -Command "$j=Get-Content C:\\Omega\\logs\\aurora\\aurora_NQ.json -Raw|ConvertFrom-Json; \\"$($j.price) $($j.stamp_ms)\\""'],
            capture_output=True,text=True,timeout=12)
        parts=out.stdout.split()
        nq=float(parts[0]); stamp=float(parts[1]) if len(parts)>1 and parts[1].replace('.','').isdigit() else 0
        if nq>0 and (stamp==0 or (time.time()-stamp/1000.0)<1800):
            _live_cache["nq"]=(time.time(),nq); return nq
        print("[live-mark] aurora NQ stale (>30min) -> daily-close fallback", flush=True)
    except Exception as e:
        print(f"[live-mark] IBKR NQ read failed ({type(e).__name__}) -> daily-close fallback", flush=True)
    return None
# daily-anchored NDX cash<-NQ basis for the live mark
_ndx_cash_close=_ndx_cash_date=None
try:
    _rr=list(csv.reader(open(NDXF))); _ndx_cash_close=float(_rr[-1][4]); _ndx_cash_date=_rr[-1][0]
except Exception: pass
_nq_live=_read_live_nq()
# NQ-front-future -> ^NDX cash basis = cost-of-carry, ~0.991 and very stable intra/day-to-day (steps only
# on the quarterly roll). A naive cash_close/nq_live anchor mis-scales because the cash close is YESTERDAY
# while NQ is live TODAY (the overnight move corrupts the ratio). Fixed carry + a sanity band on the result.
NDX_NQ_BASIS=0.991
_ndx_live_mark=None
if _nq_live and _ndx_cash_close:
    m=_nq_live*NDX_NQ_BASIS
    if _ndx_cash_close*0.96 <= m <= _ndx_cash_close*1.05: _ndx_live_mark=m   # else bad NQ read -> fallback
_ndx_basis=NDX_NQ_BASIS; _ndx_basis_date=_ndx_cash_date
# pass 1: resolve every leg's signal so we can count the open legs
legs=[]
for key,sym,csvf,cost,strat,mult,dsym,dstrat in ROSTER:
    fresh,bage,fage = fresh_ok(csvf,sym)          # STALE-DATA GUARD: hard freshness check
    integ = gate(csvf,cost)                        # integrity (correctness) check
    p0 = prior.get(key,{})
    if not fresh:
        print(f"[STALE-DATA] {key} {sym} {os.path.basename(csvf)} bar_age={bage:.1f}d file_age={fage:.1f}d "
              f"(max {max_age_for(sym)}d) -- FROZEN, no trade booked off stale data", flush=True)
        # FREEZE: hold the prior position, do NOT call signal() -> no flip, no fill at a stale price.
        t,sz,px,expx = p0.get("pos",0), 1.0, (p0.get("px") or p0.get("entry_px") or 0.0), 0.0
    elif not integ:
        t,sz,px,expx = 0,1.0,0.0,0.0               # corrupt data -> flat (existing behaviour)
    else:
        t,sz,px,expx = signal(sym,csvf,cost,strat)
    if sym=="NDX" and _ndx_live_mark and _ndx_live_mark>0: px=_ndx_live_mark  # IBKR live NQ->cash mark
    legs.append(dict(key=key,sym=sym,csvf=csvf,cost=cost,strat=strat,mult=mult,dsym=dsym,
                     dstrat=dstrat,clean=(fresh and integ),fresh=fresh,integ=integ,
                     bar_age=round(bage,1),file_age=round(fage,1),t=t,sz=sz,px=px,expx=expx))
n_open=sum(1 for L in legs if L["t"]!=0)
per_leg=POOL/max(1,n_open)
cmap=corr_downsize(legs)                          # {key:(corr, size_scale)}
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
    cum_usd=p.get("realized_usd",0.0)                       # cumulative banked $ for this leg (sized)
    corr,scale=cmap.get(key,(0.0,1.0))                      # corr-downsize: halve a leg that's gone correlated
    qty=qty_for(epx if (t!=0 and epx>0) else px, mult, scale)  # contracts for this leg's (scaled) pool share
    # re-open guard (applies to ANY protection clip -- PROTECT or BE-ratchet): hold flat until the signal flips off it
    if key in clipped:
        if t==clipped[key]: t=0
        else: clipped.pop(key,None)
    # ---- BE-RATCHET: NDX index-trend leg only (engine-native break-even ratchet, NOT the giveback companion) ----
    if NDX_BE and sym=="NDX" and strat=="TSMom50" and pos0!=0 and epx>0 and t==pos0:
        cur_ret=pos0*(px-epx)/epx                          # favorable return frac (signed by direction)
        be_pk=p.get("be_peak",cur_ret)
        if cur_ret>be_pk: be_pk=cur_ret
        p["be_peak"]=be_pk
        if be_pk>=NDX_BE_ARM and cur_ret<=NDX_BE_FLOOR:    # armed (+1.5% fav) then gave it all back to breakeven
            clipped[key]=pos0; t=0
            print(f"[BE-RATCHET] NDX force-close {'LONG' if pos0>0 else 'SHORT'} ret={cur_ret*100:.2f}% peak={be_pk*100:.2f}%",flush=True)
    # ---- COMPANION PROTECTION: force-close a HELD leg (profit-lock / dead-money cut), override ride-wide ----
    if PROTECT:
        if pos0!=0 and epx>0 and t==pos0:                  # leg held + signal not flipping this run -> check protection
            cur_usd=pos0*(px-epx)*mult*qty
            pk=p.get("peak_usd",cur_usd); pk_ts=p.get("peak_ts",_unix(now))
            if cur_usd>pk: pk=cur_usd; pk_ts=_unix(now)
            p["peak_usd"]=pk; p["peak_ts"]=pk_ts
            ent=_unix(ets) if ets else _unix(now); age=_unix(now)-ent
            armed  = pk>=PROT_GATE_USD
            revers = armed and cur_usd<=pk*(1.0-PROT_TRAIL)            # gave back TRAIL of a real peak
            stalled= armed and (_unix(now)-pk_ts)>=PROT_STALL_SEC      # armed but no new peak for STALL
            dead   = (pk<PROT_GATE_USD) and age>=PROT_DEAD_SEC and cur_usd<PROT_DEAD_USD  # never worked + bleeding
            if revers or stalled or dead:
                reason="REVERSAL" if revers else ("STALL" if stalled else "DEAD_MONEY")
                clipped[key]=pos0; t=0
                print(f"[PROTECT] {reason} force-close {key} {'LONG' if pos0>0 else 'SHORT'} unreal=${cur_usd:.2f} peak=${pk:.2f} age={age//3600}h",flush=True)
    if t!=pos0:                                            # ---- TRADE (flip) ----
        if pos0!=0 and epx>0:                              # close old leg -> realized
            q0=qty                                          # contracts the closing leg was sized at
            r=(pos0*(px-epx)/epx*sz - c*sz)*100; cum+=r; ntrades+=1
            r_usd=pos0*(px-epx)*mult*q0 - (cost+2)/10000.0*px*mult*q0; cum_usd+=r_usd  # sized $ P&L, real cost
            closed.insert(0,{"sym":dsym,"strat":dstrat,"dir":("LONG" if pos0>0 else "SHORT"),
                             "entry_ts":ets,"exit_ts":now,"entry_px":round(epx,2),"exit_px":round(px,2),
                             "contracts":q0,"realized_pct":round(r,3),"realized_usd":round(r_usd,2),"mult":mult})
            led.write(f"{now},{key},CLOSE {pos0},{t},{px:.4f},{r:.3f},{cum:.3f}\n")
            uid=f"{key}_{_unix(now)}"; side="LONG" if pos0>0 else "SHORT"   # -> Omega ledger (one row per flip-close)
            inb.write(f"{uid},{_unix(ets)},{_unix(now)},{sym},{strat},{side},{epx:.4f},{px:.4f},{r_usd:.2f}\n"); inb.flush()
        if t!=0: epx=px; ets=now; qty=qty_for(epx,mult,scale); p["peak_usd"]=0.0; p["peak_ts"]=_unix(now); p["be_peak"]=0.0; led.write(f"{now},{key},OPEN {t},{t},{px:.4f},,{cum:.3f}\n"); ntrades+=1
    unreal = (t*(px-epx)/epx*sz*100) if (t!=0 and epx>0) else 0.0
    unreal_usd = (t*(px-epx)*mult*qty) if (t!=0 and epx>0) else 0.0   # sized $ at state px (GUI re-marks live)
    if t!=0: deployed+=px*mult*qty
    tot_real+=cum; tot_unreal+=unreal; tot_real_usd+=cum_usd; tot_unreal_usd+=unreal_usd
    slots.append({"key":key,"sym":dsym,"strat":dstrat,"pos":t,"contracts":(qty if t else 0),
                  "vt":round(sz,2),"mult":mult,"pool":POOL,
                  "corr":round(corr,2),"downsized":(scale<1.0 and t!=0),
                  "notional":round(px*mult*qty,0) if t else 0,
                  "px":round(px,2),"entry_px":round(epx,2),"entry_ts":ets,"exit_px":round(expx,2),
                  "unreal_pct":round(unreal,3),"unreal_usd":round(unreal_usd,2),
                  "realized_pct":round(cum,3),"realized_usd":round(cum_usd,2),"clean":clean,
                  "peak_usd":round(p.get("peak_usd",0.0),2) if t else 0.0,"peak_ts":(p.get("peak_ts") if t else None),
                  "be_peak":round(p.get("be_peak",0.0),5) if (t and sym=="NDX") else 0.0,
                  "asset_class":("INDEX" if sym=="NDX" else "CRYPTO"),
                  "fresh":L["fresh"],"stale":(not L["fresh"]),"bar_age_d":L["bar_age"],"file_age_d":L["file_age"]})
led.close(); inb.close()
closed=closed[:25]
# ── book-level DATA HEALTH (per-source freshness) + stale flag for the GUI/monitor ──
sources={}
for L in legs:
    b=os.path.basename(L["csvf"])
    if b not in sources:
        sources[b]={"bar_age_d":L["bar_age"],"file_age_d":L["file_age"],"max_age_d":max_age_for(L["sym"]),"fresh":L["fresh"]}
stale_sources=sorted(b for b,v in sources.items() if not v["fresh"])
data_health={"all_fresh":(len(stale_sources)==0),"stale_sources":stale_sources,"sources":sources}
FLAG=os.path.join(HERE,"data","ibkrcrypto","stale_data.flag")
try:
    if stale_sources: open(FLAG,"w").write(now+" UTC STALE: "+", ".join(stale_sources)+"\n")
    elif os.path.exists(FLAG): os.remove(FLAG)
except Exception: pass
if stale_sources: print(f"[STALE-DATA] BOOK NOT ALL-FRESH -- frozen sources: {stale_sources}", flush=True)
json.dump({"engine":"IBKRCrypto","mode":"SHADOW","updated":now+" UTC","data_health":data_health,
           "open_unreal_pct":round(tot_unreal,2),"realized_pct":round(tot_real,2),
           "total_pct":round(tot_unreal+tot_real,2),
           "open_unreal_usd":round(tot_unreal_usd,2),"realized_usd":round(tot_real_usd,2),
           "total_usd":round(tot_unreal_usd+tot_real_usd,2),
           "pool_usd":POOL,"deployed_usd":round(deployed,0),
           "n_open":sum(1 for s in slots if s["pos"]),
           "ndx_basis":_ndx_basis,"ndx_basis_date":_ndx_basis_date,
           "ndx_mark_src":("IBKR-NQ" if _ndx_live_mark else "daily-close"),
           "clipped":clipped,
           "slots":slots,"closed":closed}, open(STATE,"w"), indent=1)
print(f"refresh: {ntrades} new trade-events, open unreal ${tot_unreal_usd:.2f} ({tot_unreal:.2f}%) realized ${tot_real_usd:.2f}")
