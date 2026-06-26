#!/usr/bin/env python3
"""Stall Accountant / COMPANION EXECUTOR (S-2026-06-26, operator-designed) — symbol/engine-agnostic
PAPER companion book that ACTUALLY TRADES (fires + tracks + banks + shows in GUI) across BOTH the
Omega VPS book AND the crypto book.

Dual-book design the operator asked for:
  * The REAL engine trade keeps riding WIDE (we never touch it — zero bearing on the real position).
  * Alongside it we OPEN a COMPANION mirroring the same entry/side, and BANK it on an early exit.
  * Two independent exit triggers run side by side, each TAGGED so the ledger shows which banks more:
      STALL_CLIP    — MFE makes no new favourable extreme for STALL_BARS H4 bars (the VALIDATED rule
                      on XAU_4h_DonchN20: BULL +45% vs wide +31%, CHOP +9.8% vs +7.1%).
      REVERSAL_CLIP — fast trigger: once past the gate, price gives back >= REVERSAL_GIVEBACK of its
                      MFE peak -> clip immediately (catches a major reversal BETWEEN H4 bars).
      ENGINE_EXIT   — the real engine closed the trade before either trigger -> companion matches wide.
  Profit-GATE (GATE_PCT): no trigger arms until the companion has captured >= GATE_PCT. Chop trades
  never reach it -> never clip -> ride wide. This is why we don't bleed clipping noise.

VALIDATION NOTE: STALL_CLIP@H4 is validated on XAU only. REVERSAL_CLIP and ALL crypto companions are
EXPLORATORY measurements — the realized P&L is data to compare, NOT a proven edge to size off.

This is PAPER (no broker order) but treated as REAL: tracked book + running realized P&L + cockpit panel.
Files: companion_positions.json (open) · companion_closed.csv (bank) · companion_state.json (cockpit roll-up).
"""
import json, os, subprocess, base64, time, datetime
HERE  = os.path.dirname(os.path.abspath(__file__))
POS   = os.path.join(HERE, "companion_positions.json")
CLOSED= os.path.join(HERE, "companion_closed.csv")
STATE = os.path.join(HERE, "companion_state.json")
STATE_LEGACY = os.path.join(HERE, "stall_acct_state.json")
TRK_LEGACY   = os.path.join(HERE, "stall_track.json")
CRYPTO = os.path.expanduser("~/IBKRCrypto/backtest/data/ibkrcrypto/state.json")

N_STALL  = int(os.environ.get("STALL_BARS", "3"))            # H4 bars with no new favourable extreme = stalled
GATE_PCT = float(os.environ.get("STALL_GATE_PCT", "2.0"))    # arm triggers only after capturing >= this %
TF_SEC   = int(float(os.environ.get("STALL_TF_HOURS", "4")) * 3600)
REV_GB   = float(os.environ.get("REVERSAL_GIVEBACK", "0.40"))  # clip if price gives back >= this FRACTION of MFE peak

_PS = ('try{$t=(Invoke-WebRequest -UseBasicParsing http://127.0.0.1:7779/api/telemetry -TimeoutSec 6).Content|'
       'ConvertFrom-Json; $t.live_trades|ForEach-Object{$_.engine+"|"+$_.symbol+"|"+$_.side+"|"+'
       '([math]::Round([double]$_.entry,4))+"|"+([math]::Round([double]$_.current,4))+"|"+'
       '([math]::Round([double]$_.live_pnl,2))}}catch{}')

def poll_omega():
    """Live Omega VPS book via ssh telemetry. Returns rows or None on poll failure (do NOT touch book)."""
    enc = base64.b64encode(_PS.encode("utf-16-le")).decode()
    try:
        out = subprocess.run(["ssh","-o","ConnectTimeout=6","-o","BatchMode=yes",
            "-o","ControlMaster=auto","-o","ControlPath=/tmp/ssh-omega-stall-%r@%h:%p","-o","ControlPersist=120",
            "omega-vps","powershell -NoProfile -EncodedCommand "+enc],
            capture_output=True, text=True, timeout=20).stdout
    except Exception as e:
        print("companion: omega telemetry poll failed", e); return None
    rows = []
    for ln in out.strip().splitlines():
        p = ln.split("|")
        if len(p) < 6: continue
        try: rows.append(("OMEGA", p[0], p[1], p[2], float(p[3]), float(p[4]), float(p[5])))
        except Exception: pass
    return rows

def poll_crypto():
    """Local crypto book (IBKRCrypto state.json). Mirrors every slot with an open pos."""
    rows = []
    try:
        d = json.load(open(CRYPTO))
    except Exception as e:
        print("companion: crypto book read failed", e); return rows   # [] -> just skip crypto this cycle
    for s in d.get("slots", []):
        if not s.get("pos"): continue
        try:
            entry = float(s.get("entry_px") or 0); cur = float(s.get("px") or 0)
            side  = "LONG" if s["pos"] > 0 else "SHORT"
            rows.append(("CRYPTO", s.get("strat","?"), s.get("sym","?"), side, entry, cur, float(s.get("unreal_usd") or 0)))
        except Exception: pass
    return rows

def _close(pos, key, reason, pnl, bar):
    p = pos[key]
    new = not (os.path.exists(CLOSED) and os.path.getsize(CLOSED) > 0)
    with open(CLOSED, "a") as f:
        if new: f.write("ts,book,reason,engine,symbol,side,entry,realized_pnl,mfe_peak_pct,bars_held\n")
        f.write(f"{int(time.time())},{p['book']},{reason},{p['eng']},{p['sym']},{p['side']},{p['entry']},"
                f"{round(pnl,2)},{round(p['mfe_pct'],2)},{bar-p['open_bar']}\n")
    del pos[key]
    return (p['book'], p['eng'], p['sym'], reason, round(pnl,2))

def main():
    pos = json.load(open(POS)) if os.path.exists(POS) else {}
    omega = poll_omega()
    if omega is None: print("companion: skip cycle (omega telemetry unreachable) — book untouched"); return
    rows = omega + poll_crypto()
    bar = int(time.time()) // TF_SEC
    live = {}; banked = []
    for book, eng, sym, side, entry, current, upnl in rows:
        key = f"{book}|{eng}|{sym}|{round(entry,4)}"
        if current <= 0 or entry <= 0:                         # no live mark yet -> skip, no garbage MFE
            if key in pos: live[key] = pos[key]["last_upnl"]
            continue
        live[key] = upnl
        fav = ((current - entry) / entry if side.upper().startswith("L") else (entry - current) / entry) * 100.0
        if key not in pos:
            pos[key] = {"book":book,"eng":eng,"sym":sym,"side":side,"entry":round(entry,4),
                        "open_ts":int(time.time()),"open_bar":bar,"mfe_pct":fav,"ext_bar":bar,"last_upnl":upnl}
        p = pos[key]
        if fav > p["mfe_pct"] + 1e-9: p["mfe_pct"] = fav; p["ext_bar"] = bar
        p["stall"]     = bar - p["ext_bar"]
        p["fav"]       = fav
        p["last_upnl"] = upnl
        armed = p["mfe_pct"] >= GATE_PCT                       # profit-gate cleared
        if armed and p["stall"] >= N_STALL:                                   # VALIDATED H4 stall
            banked.append(_close(pos, key, "STALL_CLIP", upnl, bar)); continue
        if armed and fav <= p["mfe_pct"] * (1.0 - REV_GB):                    # fast reversal (give-back from peak)
            banked.append(_close(pos, key, "REVERSAL_CLIP", upnl, bar)); continue
    for key in [k for k in pos if k not in live]:                             # real trade closed first
        banked.append(_close(pos, key, "ENGINE_EXIT", pos[key]["last_upnl"], bar))
    json.dump(pos, open(POS,"w"), indent=1)
    # roll-up: realized bank (sum of closed ledger) + open companions, per engine + per reason
    realized_total = 0.0; per = {}; by_reason = {}
    if os.path.exists(CLOSED) and os.path.getsize(CLOSED) > 0:
        for i, ln in enumerate(open(CLOSED)):
            if i == 0: continue
            c = ln.strip().split(",")
            if len(c) < 10: continue
            e = per.setdefault(c[3], {"open":0,"closed":0,"realized":0.0})
            e["closed"] += 1; e["realized"] += float(c[7]); realized_total += float(c[7])
            by_reason[c[2]] = round(by_reason.get(c[2],0.0) + float(c[7]), 2)
    for p in pos.values():
        e = per.setdefault(p["eng"], {"open":0,"closed":0,"realized":0.0}); e["open"] += 1
    roll = {"updated":datetime.datetime.now(datetime.timezone.utc).strftime("%Y-%m-%d %H:%M UTC"),
            "stall_bars":N_STALL,"gate_pct":GATE_PCT,"reversal_giveback":REV_GB,
            "open_companions":len(pos),"realized_total":round(realized_total,2),
            "by_reason":by_reason,"per_engine":per,
            "open_detail":[{"book":p["book"],"eng":p["eng"],"sym":p["sym"],"side":p["side"],"entry":p["entry"],
                            "mfe_pct":round(p["mfe_pct"],2),"stall":p["stall"],"upnl":round(p["last_upnl"],2),
                            "eligible": p["mfe_pct"]>=GATE_PCT} for p in pos.values()]}
    json.dump(roll, open(STATE,"w"), indent=1)
    json.dump(roll, open(STATE_LEGACY,"w"), indent=1)
    json.dump(pos,  open(TRK_LEGACY,"w"), indent=1)
    # push state to the VPS so the Omega desk GUI (:7779 /api/companion -> loadFile C:\Omega\) can nest the
    # companion under each live trade. Best-effort; reuses the same ssh ControlMaster as the telemetry poll.
    try:
        subprocess.run(["scp","-o","ConnectTimeout=6","-o","BatchMode=yes","-o","ControlMaster=auto",
            "-o","ControlPath=/tmp/ssh-omega-stall-%r@%h:%p","-o","ControlPersist=120",
            STATE, "omega-vps:C:/Omega/companion_state.json"], capture_output=True, timeout=15)
    except Exception as e:
        print("companion: VPS push failed", e)
    print(f"companion: open {len(pos)} | banked-now {banked} | realized ${round(realized_total,2)} | by_reason {by_reason}")

if __name__ == "__main__":
    main()
