#!/usr/bin/env python3
# live_mark.py - intraday LIVE price mark for open crypto legs (operator-mandated 2026-06-27).
# Why: the companion (stall_accountant) tracks MFE/giveback off each slot's `px`. With `px`=daily
# close it is intraday-BLIND (e.g. a SOL short showing +1.9% on the daily mark while live it's
# -5% underwater). This re-marks the OPEN crypto legs against the live Binance spot price so the
# companion's REVERSAL_CLIP can trip on real intraday give-back. Crypto only (24/7); NDX/weekend
# legs keep their existing mark (markets closed -> no live tick anyway). NON-destructive: only
# touches px/unreal of mapped open legs + the roll-up totals.
import json, os, urllib.request, datetime

HERE  = os.path.dirname(os.path.abspath(__file__))
# IBKRCRYPTO_DATADIR override lets a second instance re-mark the intraday book
# (data/ibkrcrypto_intraday/state.json) without touching the daily book.
_DD   = os.environ.get("IBKRCRYPTO_DATADIR", os.path.join(HERE, "data", "ibkrcrypto"))
STATE = os.path.join(_DD, "state.json")
# slot.key prefix -> Binance spot symbol
BINANCE = {"eth": "ETHUSDT", "btc": "BTCUSDT", "sol": "SOLUSDT"}

def live_price(binsym):
    try:
        r = json.load(urllib.request.urlopen(
            "https://api.binance.com/api/v3/ticker/price?symbol=" + binsym, timeout=8))
        return float(r["price"])
    except Exception as e:
        print(f"live_mark: {binsym} fetch failed: {e}"); return None

def main():
    if not os.path.exists(STATE):
        print("live_mark: no state.json"); return
    d = json.load(open(STATE))
    ts = datetime.datetime.now(datetime.timezone.utc).strftime("%Y-%m-%d %H:%M UTC")
    n_marked = 0
    tot_unreal_usd = 0.0; tot_unreal_pct = 0.0
    for s in d.get("slots", []):
        if s.get("pos"):
            key = (s.get("key") or "").lower()
            binsym = next((v for k, v in BINANCE.items() if key.startswith(k)), None)
            entry = float(s.get("entry_px") or 0)
            if binsym and entry > 0:
                lp = live_price(binsym)
                if lp:
                    old_pct = float(s.get("unreal_pct") or 0)
                    side = 1 if s["pos"] > 0 else -1
                    new_pct = side * (lp - entry) / entry * 100.0
                    # size $ directly from this leg's own contracts*mult (refresh_shadow.py:261
                    # formula pos*(px-epx)*mult*qty). The old ratio method (new_pct*old_usd/old_pct)
                    # zero-locked any leg whose prior USD was ~0 -- 06-24 SOL longs were stuck at $0.
                    mult = float(s.get("mult") or 0)
                    contracts = float(s.get("contracts") or 0)
                    new_usd = round(side * (lp - entry) * mult * contracts, 2)
                    s["px"] = lp
                    s["unreal_pct"] = round(new_pct, 3)
                    s["unreal_usd"] = new_usd
                    s["mark_src"] = "binance-live"
                    s["mark_ts"]  = ts
                    n_marked += 1
                    print(f"live_mark: {s.get('sym')} {('LONG' if side>0 else 'SHORT')} entry={entry} live={lp} -> unreal {new_pct:+.2f}% (was {old_pct:+.2f}%)")
            # accumulate totals across ALL open legs (live-marked or not)
            tot_unreal_usd += float(s.get("unreal_usd") or 0)
            tot_unreal_pct += float(s.get("unreal_pct") or 0)
    # refresh the roll-up totals the GUI reads
    d["open_unreal_usd"] = round(tot_unreal_usd, 2)
    d["open_unreal_pct"] = round(tot_unreal_pct, 2)
    d["total_usd"] = round(tot_unreal_usd + float(d.get("realized_usd") or 0), 2)
    d["live_mark_ts"] = ts
    # atomic write (temp + replace) so a concurrent refresh_shadow can never read/leave a partial file
    tmp = STATE + ".tmp"
    json.dump(d, open(tmp, "w"), indent=1)
    os.replace(tmp, STATE)
    print(f"live_mark: re-marked {n_marked} crypto leg(s) live @ {ts}; open_unreal=${round(tot_unreal_usd,2)}")

if __name__ == "__main__":
    main()
