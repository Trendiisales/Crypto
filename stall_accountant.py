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
import json, os, subprocess, base64, time, datetime, csv
HERE  = os.path.dirname(os.path.abspath(__file__))
_DIR  = os.environ.get("COMPANION_DIR", HERE)   # override for the protection self-test sandbox
POS   = os.path.join(_DIR, "companion_positions.json")
CLOSED= os.path.join(_DIR, "companion_closed.csv")
STATE = os.path.join(_DIR, "companion_state.json")
STATE_LEGACY = os.path.join(_DIR, "stall_acct_state.json")
TRK_LEGACY   = os.path.join(_DIR, "stall_track.json")
CRYPTO = os.environ.get("CRYPTO_STATE", os.path.expanduser("~/IBKRCrypto/backtest/data/ibkrcrypto/state.json"))
SKIP_OMEGA = os.environ.get("SKIP_OMEGA","0") == "1"   # self-test: don't ssh the VPS, crypto-only

# ENGINE SELECTIVITY (operator 2026-06-29): only harvest the giveback on RIDE-WIDE engines that
# actually bleed it. EXCLUDE self-exit engines (turtles exit on Donchian-low; MR/IBS/RSIrev exit on
# revert; seasonal on fixed hold) -- backtest (tools/reversal_stop_bt.py) showed clipping them is
# redundant/slightly-negative. Substring match on engine name; env-overridable as backtests refine.
ENGINE_EXCLUDE = [s.strip() for s in os.environ.get("COMPANION_EXCLUDE",
    "IBS,Mean-Rev,MeanRev,RSIrev,RSIRev,Regime,Connors,Seasonal,Monday,Turnaround,TurnOfMonth,Turtle").split(",") if s.strip()]
def _excluded(eng):
    e = (eng or "")
    return any(x.lower() in e.lower() for x in ENGINE_EXCLUDE)

N_STALL  = int(os.environ.get("STALL_BARS", "2"))            # H4 bars with no new favourable extreme = stalled (tightened 3->2 S-2026-06-29)
GATE_PCT = float(os.environ.get("STALL_GATE_PCT", "1.5"))    # arm triggers only after capturing >= this % (tightened 2.0->1.5)
TF_SEC   = int(float(os.environ.get("STALL_TF_HOURS", "4")) * 3600)
# REVERSAL_GIVEBACK: clip when price gives back >= this FRACTION of MFE peak. Operator decision
# 2026-06-29: 0.40 was absurdly loose (gave back ~half of profit before triggering); tightened to
# 0.05 (5%). e.g. peak +5% then drop to +4.75% -> clip. Pairs with RE_TRIG below: if real engine
# keeps running and price makes a NEW high beyond the clip-point, a fresh companion re-opens to
# catch the next leg (so a tight clip doesn't permanently sideline the engine for the rest of the trade).
REV_GB   = float(os.environ.get("REVERSAL_GIVEBACK", "0.05"))
# RE_TRIG_PCT (S-2026-06-29): after a clip, if the real trade is still LIVE and its current
# favourable % exceeds the prior peak by this fraction (i.e. new MFE high made past clip-point),
# drop the key from the clipped-set and re-open a fresh companion. Banks every leg of a runner
# instead of going dark for the rest of the trade after one clip.
RE_TRIG_PCT = float(os.environ.get("COMPANION_RETRIG_PCT", "0.05"))
# S-2026-06-29: NEW trigger. The companion as designed (STALL/REVERSAL only) had NO loss-cut path
# -- a losing leg rode forever until the real engine closed. LOSS_CUT_CLIP closes the companion
# when upnl drops below this dollar threshold. Accounting-only (does NOT touch real engine).
COLD_LOSS = float(os.environ.get("COLD_LOSS_USD", "-50.0"))

_PS = ('try{$t=(Invoke-WebRequest -UseBasicParsing http://127.0.0.1:7779/api/telemetry -TimeoutSec 6).Content|'
       'ConvertFrom-Json; $t.live_trades|ForEach-Object{$_.engine+"|"+$_.symbol+"|"+$_.side+"|"+'
       '([math]::Round([double]$_.entry,4))+"|"+([math]::Round([double]$_.current,4))+"|"+'
       '([math]::Round([double]$_.live_pnl,2))}}catch{}')

def poll_omega():
    """Live Omega VPS book via ssh telemetry. Returns rows or None on poll failure (do NOT touch book)."""
    if SKIP_OMEGA: return []   # self-test sandbox: crypto-only, no VPS ssh
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
    # S-2026-06-29: write via csv.writer (QUOTE_MINIMAL) so symbols containing
    # commas -- e.g. "SOL (fut, SQF pending)" -- are properly quoted. Previous
    # raw f-string writer left them unquoted -> reader's split(",") shifted
    # columns -> realized_pnl summed entry-price instead of PnL -> fake +$151.
    with open(CLOSED, "a", newline="") as f:
        w = csv.writer(f, quoting=csv.QUOTE_MINIMAL)
        if new:
            w.writerow(["ts","book","reason","engine","symbol","side","entry",
                        "realized_pnl","mfe_peak_pct","bars_held"])
        w.writerow([int(time.time()), p['book'], reason, p['eng'], p['sym'],
                    p['side'], p['entry'], round(pnl,2), round(p['mfe_pct'],2),
                    bar - p['open_bar']])
    del pos[key]
    return (p['book'], p['eng'], p['sym'], reason, round(pnl,2))

CLIPPED = os.path.join(_DIR, "companion_clipped.json")   # keys clipped while their real trade is STILL open (sandbox-aware)

def main():
    pos = json.load(open(POS)) if os.path.exists(POS) else {}
    # keys already clipped while the underlying trade is still live -> do NOT re-open a new companion
    # for them every cycle (that re-clip loop is what made the realized total jump run-to-run).
    # S-2026-06-29: clipped is now a DICT {key: prior_peak_pct} so we can re-trigger when the real
    # trade keeps running and price makes a NEW favourable high past the prior peak by RE_TRIG_PCT.
    # Legacy: file may be a list-of-keys; convert (lose prior-peak history once, re-arm immediately).
    clipped = {}
    if os.path.exists(CLIPPED):
        try:
            d = json.load(open(CLIPPED))
            clipped = d if isinstance(d, dict) else {k: 0.0 for k in d}
        except Exception:
            clipped = {}
    omega = poll_omega()
    if omega is None: print("companion: skip cycle (omega telemetry unreachable) — book untouched"); return
    # RESTART/BLIP GUARD (2026-06-27): telemetry can return UP-but-EMPTY for a minute or two
    # during an Omega restart (API live, trades not yet reloaded). Without this, the ENGINE_EXIT
    # loop below phantom-banks EVERY open OMEGA companion as if it closed — that produced the
    # bogus "$269.9 banked" on a XAU short that was actually still open. If omega telemetry is
    # empty while we still hold open OMEGA companions, treat it as a blip and skip the cycle.
    if len(omega) == 0 and any(p.get("book") == "OMEGA" for p in pos.values()):
        print("companion: omega telemetry empty but holding open OMEGA companions — skip cycle (restart/blip guard)"); return
    rows = omega + poll_crypto()
    rows = [r for r in rows if not _excluded(r[1])]   # only harvest ride-wide engines that bleed giveback
    bar = int(time.time()) // TF_SEC
    live = {}; banked = []
    for book, eng, sym, side, entry, current, upnl in rows:
        key = f"{book}|{eng}|{sym}|{round(entry,4)}"
        if current <= 0 or entry <= 0:                         # no live mark yet -> skip, no garbage MFE
            if key in pos: live[key] = pos[key]["last_upnl"]
            continue
        live[key] = upnl
        fav = ((current - entry) / entry if side.upper().startswith("L") else (entry - current) / entry) * 100.0
        if key in clipped:
            # S-2026-06-29 re-trigger: real trade still live AND current fav > prior_peak * (1+RE_TRIG)
            # -> drop from clipped, allow companion to re-open and catch the next leg. Otherwise stay clipped.
            prior_peak = float(clipped.get(key, 0.0))
            if RE_TRIG_PCT > 0 and prior_peak > 0 and fav > prior_peak * (1.0 + RE_TRIG_PCT):
                del clipped[key]
                print(f"companion: RE-TRIGGER {key} fav={fav:.2f}% > prior_peak={prior_peak:.2f}% * (1+{RE_TRIG_PCT}) -> new companion will arm")
            else:
                continue
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
            peak = p["mfe_pct"]
            banked.append(_close(pos, key, "STALL_CLIP", upnl, bar)); clipped[key] = peak; continue
        if armed and fav <= p["mfe_pct"] * (1.0 - REV_GB):                    # fast reversal (give-back from peak)
            peak = p["mfe_pct"]
            banked.append(_close(pos, key, "REVERSAL_CLIP", upnl, bar)); clipped[key] = peak; continue
        # S-2026-06-29: cold-loss-cut -- closes the "rides forever underwater" hole. Independent
        # of armed-state (a never-armed losing leg also bleeds). Accounting-only.
        if upnl <= COLD_LOSS:
            peak = p["mfe_pct"]
            banked.append(_close(pos, key, "LOSS_CUT_CLIP", upnl, bar)); clipped[key] = peak; continue
    for key in [k for k in pos if k not in live]:                             # real trade closed first
        banked.append(_close(pos, key, "ENGINE_EXIT", pos[key]["last_upnl"], bar))
    # a clipped trade stays clipped only while its real trade is still live; once the engine
    # actually closes it (key leaves `live`), drop it so a future NEW trade can be tracked again.
    clipped = {k: v for k, v in clipped.items() if k in live}
    json.dump(clipped, open(CLIPPED, "w"), indent=1)
    json.dump(pos, open(POS,"w"), indent=1)
    # roll-up: realized bank (sum of closed ledger) + open companions, per engine + per reason
    # S-2026-06-29: index the LAST 3 cols (realized_pnl, mfe_peak_pct, bars_held)
    # negatively so legacy rows written before the csv.writer fix still parse
    # correctly even when their unquoted symbol field contained commas. Those
    # last 3 cols are always numeric -> no embedded commas -> safe from -1.
    realized_total = 0.0; per = {}; by_reason = {}
    if os.path.exists(CLOSED) and os.path.getsize(CLOSED) > 0:
        with open(CLOSED, newline="") as f:
            rdr = csv.reader(f)
            for i, c in enumerate(rdr):
                if i == 0: continue
                if len(c) < 10: continue
                reason = c[2]; engine = c[3]
                # negative indices: realized_pnl always 3rd-from-last
                try:
                    pnl = float(c[-3])
                except (ValueError, IndexError):
                    continue
                e = per.setdefault(engine, {"open":0,"closed":0,"realized":0.0})
                e["closed"] += 1; e["realized"] += pnl; realized_total += pnl
                by_reason[reason] = round(by_reason.get(reason, 0.0) + pnl, 2)
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
