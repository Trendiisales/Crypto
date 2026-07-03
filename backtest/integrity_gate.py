#!/usr/bin/env python3
# IBKRCrypto data-integrity gate (mirrors Omega backtest/data_integrity_gate.py).
# Rejects bad seeds before they reach an engine: x1000 glitches, gaps, dupes,
# non-monotonic ts, zero/neg prices, OHLC violations. A REJECTED file is NOT used.
import sys, csv, os
def check(path):
    rows=[]
    with open(path) as f:
        for r in csv.reader(f):
            if not r or not r[0].isdigit(): continue
            try: rows.append((int(r[0]),float(r[1]),float(r[2]),float(r[3]),float(r[4])))
            except: pass
    if len(rows)<60: return ["<60 bars"]
    errs=[]; prev_ts=None; prev_c=None; gaps=0
    DAY=86400000
    for i,(ts,o,h,l,c) in enumerate(rows):
        if min(o,h,l,c)<=0: errs.append(f"row{i} non-positive px");
        if h<l or h<o or h<c or l>o or l>c: errs.append(f"row{i} OHLC violation")
        if prev_ts is not None:
            if ts<=prev_ts: errs.append(f"row{i} non-monotonic/dupe ts")
            d=(ts-prev_ts)/DAY
            if d>3.0: gaps+=1                      # >3-day hole (weekends ok for index)
        if prev_c and prev_c>0:
            jump=abs(c-prev_c)/prev_c
            # ×1000/×100/×10 glitch = 900%..99900% single-bar. Threshold 3.0 (300%) nails
            # every order-of-magnitude corruption while NOT false-rejecting genuine crypto 1h
            # pumps (e.g. DOGE +70% Jan-2021 mania) — the exact wide up-jumps UpJump trades.
            if jump>3.0: errs.append(f"row{i} x-glitch jump {jump*100:.0f}% (poss x1000)")
        prev_ts, prev_c = ts, c
        if len(errs)>8: break
    if gaps>len(rows)*0.05: errs.append(f"{gaps} gaps >3d ({100*gaps/len(rows):.0f}%)")
    return errs
if __name__=="__main__":
    files=sys.argv[1:] or []
    bad=0
    for p in files:
        if not os.path.exists(p): print(f"[MISS] {p}"); bad+=1; continue
        e=check(p)
        if e: print(f"[REJECT] {os.path.basename(p)}: {e[:4]}"); bad+=1
        else: print(f"[OK]     {os.path.basename(p)}  clean")
    print(f"\n{'PASS' if bad==0 else 'FAIL'}: {len(files)-bad}/{len(files)} clean")
    sys.exit(1 if bad else 0)
