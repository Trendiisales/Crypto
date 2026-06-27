#!/usr/bin/env python3
# giveback_saver.py — SIMPLE per-trade giveback accounting (operator-asked 2026-06-27).
#
# ONE rule, easy to reason about:
#   * Track each live trade's PEAK profit ($) as it rides.
#   * Once peak >= GATE, arm a TIGHT trail.
#   * When profit gives back >= TRAIL of the peak, LOCK the profit ONCE ("saved").
#   * "giveback_saved" = peak - locked  (what the wide ride bled that a trail would have kept).
#
# PURE PAPER ACCOUNTING. It NEVER sends an order and NEVER touches the real position — the
# engine keeps riding wide, so the edge is 100% unaffected by construction. This only MEASURES
# what a tight trail would have banked vs the wide ride, per trade, in a clean ledger.
#
# Honest limitation: it can only track the peak from when it STARTS running. It cannot recover
# a peak that happened while it wasn't scheduled. So it must run reliably (cron) to be useful.
import json, os, subprocess, base64, time, datetime

HERE   = os.path.dirname(os.path.abspath(__file__))
STATE  = os.path.join(HERE, "saver_state.json")     # per-trade peak + locked status (persisted)
LEDGER = os.path.join(HERE, "saver_ledger.csv")     # one row per locked trade
CRYPTO = os.path.expanduser("~/IBKRCrypto/backtest/data/ibkrcrypto/state.json")

TRAIL     = float(os.environ.get("SAVER_TRAIL", "0.20"))    # REVERSAL: lock when profit gives back >= this fraction of peak
GATE      = float(os.environ.get("SAVER_GATE_USD", "20"))   # don't arm until peak profit >= $20
STALL_SEC = float(os.environ.get("SAVER_STALL_SEC", "3600")) # STALL: lock if no NEW peak for this long (default 1h)

_PS = ('try{$t=(Invoke-WebRequest -UseBasicParsing http://127.0.0.1:7779/api/telemetry -TimeoutSec 6).Content|'
       'ConvertFrom-Json; $t.live_trades|ForEach-Object{$_.engine+"|"+$_.symbol+"|"+$_.side+"|"+'
       '([math]::Round([double]$_.live_pnl,2))+"|"+([math]::Round([double]$_.entry,4))+"|"+'
       '$_.size+"|"+$_.tick_value}}catch{}')

def poll_omega():
    """Live Omega trades via ssh telemetry -> [(book,eng,sym,side,upnl,notional)]. None on poll failure."""
    enc = base64.b64encode(_PS.encode("utf-16-le")).decode()
    try:
        out = subprocess.run(["ssh","-o","ConnectTimeout=6","-o","BatchMode=yes",
            "-o","ControlMaster=auto","-o","ControlPath=/tmp/ssh-omega-saver-%r@%h:%p","-o","ControlPersist=120",
            "omega-vps","powershell -NoProfile -EncodedCommand "+enc],
            capture_output=True, text=True, timeout=20).stdout
    except Exception as e:
        print("saver: omega telemetry poll failed", e); return None
    rows = []
    for ln in out.strip().splitlines():
        p = ln.split("|")
        if len(p) < 4: continue
        try:
            upnl = float(p[3])
            notional = abs(float(p[4]) * float(p[5]) * float(p[6])) if len(p) >= 7 else 0.0
            rows.append(("OMEGA", p[0], p[1], p[2], upnl, notional))
        except Exception: pass
    return rows

def poll_crypto():
    rows = []
    try: d = json.load(open(CRYPTO))
    except Exception: return rows
    for s in d.get("slots", []):
        if not s.get("pos"): continue
        side = "LONG" if s["pos"] > 0 else "SHORT"
        upnl = float(s.get("unreal_usd") or 0); pct = float(s.get("unreal_pct") or 0)
        notional = abs(upnl / (pct / 100.0)) if abs(pct) > 1e-6 else 0.0
        rows.append(("CRYPTO", s.get("strat","?"), s.get("sym","?"), side, upnl, notional))
    return rows

GUI_LOCAL = os.path.expanduser("~/stall-accountant/companion_state.json")   # crypto GUI :8090 reads this

def write_gui_state(st, locked_profit_total):
    """Write companion_state.json in the shape the GUIs render (open_detail keyed eng|sym,
    book=OMEGA, side, mfe_pct), so the desk GUI :7779 nests a REAL live row under each trade —
    NOT a stale placeholder. Push to the VPS for the desk GUI + keep local for the crypto GUI."""
    nowt = int(time.time())
    detail = []
    for s in st.values():
        notional = s.get("notional") or 0
        peak_pct = round(s["peak"] / notional * 100.0, 2) if notional else 0.0
        cur_pct  = round(s["cur"]  / notional * 100.0, 2) if notional else 0.0
        stall_min = int((nowt - s.get("peak_ts", nowt)) / 60)        # mins since last new peak (GUI shows "stall N")
        detail.append({
            "book": s.get("book"), "eng": s.get("eng"), "sym": s.get("sym"), "side": s.get("side"),
            "mfe_pct": peak_pct, "cur_pct": cur_pct, "stall": stall_min,
            "peak_usd": round(s["peak"], 2), "upnl": round(s["cur"], 2),
            "locked": s["locked"], "saved": s.get("saved"),
            "armed": (not s["locked"] and s["peak"] >= GATE),
        })
    roll = {
        "updated": datetime.datetime.now(datetime.timezone.utc).strftime("%Y-%m-%d %H:%M UTC"),
        "source": "giveback_saver", "stall_bars": int(STALL_SEC/60), "gate_pct": GATE,
        "reversal_giveback": TRAIL, "open_companions": len(st),
        "realized_total": round(locked_profit_total, 2), "open_detail": detail,
    }
    try:
        json.dump(roll, open(GUI_LOCAL, "w"), indent=1)
        subprocess.run(["scp","-q","-o","ConnectTimeout=8","-o","BatchMode=yes",
            "-o","ControlMaster=auto","-o","ControlPath=/tmp/ssh-omega-saver-%r@%h:%p","-o","ControlPersist=120",
            GUI_LOCAL, "omega-vps:C:/Omega/companion_state.json"], capture_output=True, timeout=15)
    except Exception as e:
        print("saver: GUI push failed", e)

def main():
    st = json.load(open(STATE)) if os.path.exists(STATE) else {}
    omega = poll_omega()
    if omega is None:
        print("saver: omega unreachable — skip cycle (no state change)"); return
    # blip guard: empty omega while we hold open OMEGA trades = restart / weekend-quiet, NOT all-closed.
    # Do NOT skip the whole cycle (that froze the crypto companions on weekends) — just PRESERVE the
    # OMEGA positions (don't prune/bank them this cycle) and keep processing crypto normally.
    omega_blip = (len(omega) == 0 and any(k.startswith("OMEGA|") for k in st))
    if omega_blip: print("saver: omega telemetry quiet — preserving OMEGA state, processing crypto")
    rows = omega + poll_crypto()

    now = int(time.time())
    live = set()
    newly_locked = []
    for book, eng, sym, side, upnl, notional in rows:
        key = f"{book}|{eng}|{sym}"
        live.add(key)
        s = st.setdefault(key, {"peak": upnl, "peak_ts": now, "cur": upnl, "locked": False, "saved": None,
                                "side": side, "book": book, "eng": eng, "sym": sym, "notional": notional})
        # always backfill identity fields (setdefault won't update pre-existing entries)
        s["book"], s["eng"], s["sym"], s["side"] = book, eng, sym, side
        s["cur"] = upnl; s["notional"] = notional or s.get("notional", 0)
        if upnl > s["peak"]: s["peak"] = upnl; s["peak_ts"] = now    # new high -> reset the stall clock
        if s["locked"]: continue
        if s["peak"] < GATE: continue                                # not enough profit to bother arming
        reversal = upnl <= s["peak"] * (1.0 - TRAIL)                  # gave back >= TRAIL of peak
        stall    = (now - s.get("peak_ts", now)) >= STALL_SEC         # no new peak for STALL_SEC
        if reversal or stall:
            reason = "REVERSAL" if reversal else "STALL"
            # bank at the TRAIL LEVEL (peak - TRAIL), not the gapped poll price. A 20% trail conceptually
            # fires at peak*(1-TRAIL); locking at wherever the next poll happens to be (e.g. SOL at -$39
            # after a swing) is a polling artifact, not what the trail captured. Never bank below the
            # trail level. STALL stalls near the peak so its current price is honest.
            trail_level = round(s["peak"] * (1.0 - TRAIL), 2)
            locked_val = trail_level if reason == "REVERSAL" else max(round(upnl, 2), trail_level)
            s["locked"] = True; s["saved"] = locked_val
            upnl = locked_val                                            # ledger + gb use the trail level
            gb = round(s["peak"] - locked_val, 2)
            newly_locked.append((reason, book, eng, sym, round(s["peak"],2), locked_val, gb))
            new = not (os.path.exists(LEDGER) and os.path.getsize(LEDGER) > 0)
            with open(LEDGER, "a") as f:
                if new: f.write("ts,reason,book,engine,symbol,side,peak_usd,locked_usd,giveback_saved_usd\n")
                f.write(f"{now},{reason},{book},{eng},{sym},{side},{round(s['peak'],2)},{round(upnl,2)},{gb}\n")

    # real trade gone from telemetry -> drop it (a future trade re-enters fresh).
    # BUT during an omega blip (weekend/restart), keep OMEGA positions so we don't lose their peak.
    for k in [k for k in st if k not in live]:
        if omega_blip and k.startswith("OMEGA|"): continue
        del st[k]
    json.dump(st, open(STATE, "w"), indent=1)

    locked_profit_total = 0.0; giveback_total = 0.0; n_locks = 0
    if os.path.exists(LEDGER) and os.path.getsize(LEDGER) > 0:
        for i, ln in enumerate(open(LEDGER)):
            if i == 0: continue
            c = ln.strip().split(",")
            if len(c) >= 9:
                try: locked_profit_total += float(c[7]); giveback_total += float(c[8]); n_locks += 1
                except Exception: pass
    write_gui_state(st, locked_profit_total)        # <-- push REAL data to the GUIs (not a placeholder)
    print(f"saver: tracking {len(live)} trade(s); newly-locked {len(newly_locked)} {newly_locked}")
    print(f"saver: LOCKED PROFIT (wins) ${round(locked_profit_total,2)} across {n_locks} lock(s); giveback rescued ${round(giveback_total,2)}")
    for k, s in st.items():
        if s["locked"]:
            print(f"  LOCKED {k}: saved=${s['saved']:.2f} (peak was ${s['peak']:.2f})")
        elif s["peak"] >= GATE:
            mins_since_peak = int((now - s.get("peak_ts", now)) / 60)
            print(f"  ARMED  {k}: peak=${s['peak']:.2f} cur=${s['cur']:.2f} -> lock if dips to ${s['peak']*(1.0-TRAIL):.2f} OR no new high for {int(STALL_SEC/60)}min (last high {mins_since_peak}min ago)")
        else:
            print(f"  watch  {k}: peak=${s['peak']:.2f} cur=${s['cur']:.2f} (peak<${GATE:.0f}, not armed)")

if __name__ == "__main__":
    main()
