#!/usr/bin/env python3
# IBKRCrypto Crypto-book GUI server -- SEPARATE from the Omega GUI.
#   Omega book  -> Omega's own GUI (OmegaApiServer / telemetry, existing ports)
#   Crypto book -> THIS server on :8090, reading the IbkrCryptoEngine's state.json
# Same IBKR gateway feeds both (one login, clientId 88 for crypto). Split = two
# dashboards, two ports, one gateway. Scaffold; production = embed this in the C++
# IbkrCryptoEngine the way Chimera's QuadEngine owns :8080.
import http.server, socketserver, json, os, time, datetime, threading, subprocess, base64
PORT=int(os.environ.get("PORT",8090))
HERE=os.path.dirname(os.path.abspath(__file__))
# State file: env override > local copy beside this server (VPS: cron scps it here) >
# the backtest output (Mac dev). First existing wins.
def _find_state():
    e=os.environ.get("IBKRCRYPTO_STATE")
    if e: return e
    for p in (os.path.join(HERE,"state.json"),
              os.path.join(HERE,"..","backtest","data","ibkrcrypto","state.json")):
        if os.path.exists(p): return p
    return os.path.join(HERE,"state.json")
STATE=_find_state()
# Companion (stall-clip) overlay — the Mac stall-accountant mirrors every crypto leg and books an early
# clip while the real trade rides wide. Served so the book can nest a companion row under each engine.
COMP=os.environ.get("COMPANION_STATE", os.path.expanduser("~/stall-accountant/companion_state.json"))
# INTRADAY (1h/4h) book — separate state + companion, same gateway. Served on the SAME page as a
# second section. Defaults to the local intraday producer output / its mirror companion dir.
STATE_INTRA=os.environ.get("IBKRCRYPTO_STATE_INTRADAY",
    os.path.join(HERE,"..","backtest","data","ibkrcrypto_intraday","state.json"))
COMP_INTRA=os.environ.get("COMPANION_STATE_INTRADAY",
    os.path.expanduser("~/Crypto/companion_intraday/companion_state.json"))
# LUKE (daily multi-name momentum) book — separate producer (refresh_luke_shadow.py), own state.
# VPS: cron scps it beside this server; Mac dev: backtest output. First existing wins.
STATE_LUKE=os.environ.get("IBKRCRYPTO_STATE_LUKE",
    next((p for p in (os.path.join(HERE,"state_luke.json"),
        os.path.join(HERE,"..","backtest","data","ibkrcrypto_luke","state.json"))
        if os.path.exists(p)), os.path.join(HERE,"state_luke.json")))

# S-2026-06-26 REAL-TIME NDX from OUR IBKR feed -- the crypto legs tick live via a client-side Coinbase WS;
# NDX must match. The Omega gateway (4001) streams every NQ TRADE into C:\Omega\logs\ibkr_l2\ibkr_trades_NQ_*.csv
# (sub-second). A background thread tails the last trade price every ~2s over ssh and scales NQ-future ->
# ^NDX cash by the carry basis (0.991). /api/ndx serves the cached value instantly, so the GUI's pullNdx
# (every 2s) shows NDX ticking in real time. NO Yahoo, NO 60s aurora/CSV path. Stale iff the tail goes >15s old.
NDX_MIN,NDX_MAX=5000.0,100000.0
NDX_NQ_BASIS=0.991
_nq={"px":0.0,"raw":0.0,"ts":0.0}
_PS_NQ=('$f=Get-ChildItem C:\\Omega\\logs\\ibkr_l2\\ibkr_trades_NQ_*.csv|'
        'Sort-Object LastWriteTime|Select-Object -Last 1; Get-Content $f.FullName -Tail 1')
_NQ_B64=base64.b64encode(_PS_NQ.encode('utf-16-le')).decode()   # EncodedCommand: no quotes -> survives cmd.exe
def _nq_poll_loop():
    # ControlMaster multiplexing: the 2s poll reuses ONE persistent ssh connection (no per-poll TCP/auth
    # handshake storm). ControlPersist keeps the master alive between polls.
    cmd=["ssh","-o","ConnectTimeout=5","-o","BatchMode=yes","-o","ServerAliveInterval=15",
         "-o","ControlMaster=auto","-o","ControlPath=/tmp/ssh-omega-nq-%r@%h:%p","-o","ControlPersist=120",
         "omega-new","powershell -NoProfile -EncodedCommand "+_NQ_B64]
    while True:
        try:
            out=subprocess.run(cmd,capture_output=True,text=True,timeout=10)
            parts=out.stdout.strip().split(",")
            if len(parts)>=2:
                raw=float(parts[1]); px=raw*NDX_NQ_BASIS
                if NDX_MIN<=px<=NDX_MAX: _nq.update(px=round(px,2),raw=raw,ts=time.time())
        except Exception: pass
        time.sleep(2)
threading.Thread(target=_nq_poll_loop,daemon=True).start()
def ndx_quote():
    now=time.time(); age=round(now-_nq["ts"],1) if _nq["ts"] else None
    stale = (age is None) or (age>15)
    return {"px":_nq["px"],"src":"IBKR-NQ-rt","state":"","stale":stale,"age_s":age}

# ---------------------------------------------------------------------------
# FRESHNESS GUARD (S-2026-06-30) — the book rows mark live off the BROWSER's
# Coinbase WS, so if the producer dies the P&L keeps ticking and LOOKS alive
# while signals/marks are frozen. That silent-staleness is the no-go. This
# computes freshness SERVER-SIDE from the state file itself (so the GUI is
# guarded even if the staleness_alarm cron is dead) and the GUI badges it red.
#   - live_mark_ts  : the real liveness heartbeat (live_mark cron every 5min).
#                     >15min stale => marks frozen => live P&L is a LIE.
#   - updated       : last signal eval. Cadence-aware: daily ~24h is normal,
#                     intraday must re-eval hourly.
#   - file mtime    : producer hasn't written at all => dead.
#   - data_health   : upstream bar feeds stale (refresh_shadow's own check).
#   - ALARM marker  : staleness_alarm.py's RED file, if present (extra reason).
MARK_MAX_S   = 900          # live_mark every 5min, tolerate 3 misses
MTIME_MAX_S  = 1200         # file untouched 20min => producer dead
SIG_MAX_S    = {"daily": 30*3600, "intraday": 100*60}
def _parse_utc(s):
    if not s: return None
    s=str(s).replace(" UTC","").replace("Z","").replace("T"," ").strip()
    for fmt in ("%Y-%m-%d %H:%M:%S","%Y-%m-%d %H:%M"):
        try: return time.mktime(time.strptime(s,fmt))-time.timezone
        except Exception: pass
    return None
def _freshness(d, path, kind):
    now=time.time(); reasons=[]; ages={}
    # 1) producer alive? (file mtime)
    try:
        mt=os.path.getmtime(path); ages["file_age_s"]=int(now-mt)
        if now-mt > MTIME_MAX_S: reasons.append(f"PRODUCER DEAD: file untouched {int((now-mt)/60)}min")
    except Exception: reasons.append("PRODUCER DEAD: state file missing")
    # 2) marks fresh? heartbeat = freshest of (live_mark_ts, updated). live_mark sets the
    #    former every 5min; refresh_shadow sets the latter and momentarily drops live_mark_ts,
    #    so EITHER being fresh => producer alive. Both stale => live P&L is a LIE.
    mt2=_parse_utc(d.get("live_mark_ts")); su=_parse_utc(d.get("updated"))
    if mt2 is not None: ages["mark_age_s"]=int(now-mt2)
    if su  is not None: ages["sig_age_s"]=int(now-su)
    hb=[x for x in (mt2,su) if x is not None]
    if not hb: reasons.append("MARKS: no timestamps in state")
    else:
        h=max(hb); ages["heartbeat_age_s"]=int(now-h)
        if now-h > MARK_MAX_S: reasons.append(f"MARKS FROZEN {int((now-h)/60)}min (live P&L not real)")
    # 3) signals fresh? (cadence-aware: daily ~24h normal, intraday hourly)
    cap=SIG_MAX_S.get(kind,30*3600)
    if su is not None and now-su > cap:
        reasons.append(f"SIGNALS STALE {int((now-su)/3600)}h (>{int(cap/3600)}h cap)")
    # 4) upstream bar feeds
    dh=d.get("data_health") or {}
    if dh.get("all_fresh") is False:
        reasons.append("DATA STALE: "+",".join(dh.get("stale_sources") or []) or "data_health=false")
    # 5) staleness_alarm.py RED marker (extra reason if cron caught it)
    try:
        mk=os.path.join(os.path.dirname(path),"ALARM_STALE.txt")
        if os.path.exists(mk): reasons.append("ALARM: "+open(mk).read().strip().split(chr(10))[0])
    except Exception: pass
    return {"stale": bool(reasons), "reasons": reasons, "ages": ages, "checked_s": int(now)}
def _inject_fresh(body, path, kind):
    try:
        d=json.loads(body); d["_fresh"]=_freshness(d,path,kind)
        return json.dumps(d).encode()
    except Exception:
        return body

class H(http.server.SimpleHTTPRequestHandler):
    def _json(self,body):
        if isinstance(body,(dict,list)): body=json.dumps(body).encode()
        self.send_response(200); self.send_header("Content-Type","application/json")
        self.send_header("Cache-Control","no-store"); self.end_headers(); self.wfile.write(body)
    def do_GET(self):
        if self.path.startswith("/api/state_intraday"):   # MUST precede /api/state (startswith)
            try: body=_inject_fresh(open(STATE_INTRA,"rb").read(),STATE_INTRA,"intraday")
            except Exception: body=b'{"engine":"IBKRCrypto-Intraday","mode":"SHADOW","slots":[],"closed":[],"_fresh":{"stale":true,"reasons":["state unreachable"]}}'
            return self._json(body)
        if self.path.startswith("/api/state_luke"):
            try: return self._json(open(STATE_LUKE,"rb").read())
            except Exception: return self._json({"book":"LukeCrypto","mode":"SHADOW","equity":0,"n_open":0,"positions":[],"closed_today":0,"last":"unreachable"})
        if self.path.startswith("/api/companion_intraday"):   # MUST precede /api/companion
            # RETIRED S-2026-07-02: intraday giveback companion parked on a REAL WF/regime
            # fail (WF-H2 -82%, bear -185%, 1h book -328%); its cron is disabled so the
            # bank is FROZEN, not live. Badge freshness + a hard disabled marker so the GUI
            # renders it retired instead of presenting a stale $-bank as a live book.
            try: d=json.loads(open(COMP_INTRA,"rb").read())
            except Exception: d={"open_companions":0,"open_detail":[],"realized_total":0}
            d["disabled"]=True
            d["status"]="RETIRED — intraday companion killed by backtest (WF-H2 -82%); bank frozen, not live"
            d["_fresh"]=_freshness(d,COMP_INTRA,"intraday")
            return self._json(d)
        if self.path.startswith("/api/state"):
            try: body=_inject_fresh(open(STATE,"rb").read(),STATE,"daily")
            except Exception: body=b'{"engine":"IBKRCrypto","mode":"SHADOW","slots":[],"day_pnl":0,"_fresh":{"stale":true,"reasons":["state unreachable"]}}'
            return self._json(body)
        if self.path.startswith("/api/ndx"):
            try: return self._json(ndx_quote())
            except Exception: return self._json({"px":0.0,"src":"","state":""})
        if self.path.startswith("/api/companion"):
            try: return self._json(open(COMP,"rb").read())
            except Exception: return self._json({"open_companions":0,"positions":[],"gate_pct":2.0,"stall_bars":3,"reversal_giveback":0.4,"realized_total":0})
        if self.path in("/","/index.html"):
            self.path="/index.html"
        return super().do_GET()
    def log_message(self,*a): pass
if __name__=="__main__":
    os.chdir(HERE)
    BIND=os.environ.get("IBKRCRYPTO_BIND","0.0.0.0")   # 0.0.0.0 = reachable at VPS public IP:8090
    print(f"[crypto-gui] http://{BIND}:{PORT}  (state: {STATE})")
    socketserver.TCPServer.allow_reuse_address=True   # avoid TIME_WAIT bind failure on restart
    srv=socketserver.ThreadingTCPServer((BIND,PORT),H)  # threaded: the NQ poll thread + GUI requests don't block
    srv.daemon_threads=True
    srv.serve_forever()
