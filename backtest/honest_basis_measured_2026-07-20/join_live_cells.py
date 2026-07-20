#!/usr/bin/env python3
# S-2026-07-20 live-cell join: main.cpp BECASC tables x measured-cost re-cert results.
# Regenerates the 369-FAIL/11-PASS verdict. Usage: python3 join_live_cells.py (from backtest/).
import re, glob, os
src = open('/Users/jo/ChimeraCrypto/src/main.cpp').read()
cells = []
for m in re.finditer(r'\{"(\w+)",\s*"(MIM[0-9]+-BECASC)",\s*"(\w+)usdt",\s*(0\.\d+),\s*(-?[\d.]+)\}', src):
    cells.append((f"{m.group(1)}-{m.group(2)}", m.group(1), float(m.group(4)), 4, 0.5))
for m in re.finditer(r'\{"(\w+)",\s*"([\w-]+-BECASC[\w-]*)",\s*"(\w+)usdt",\s*(0\.\d+),\s*(\d+),\s*(0\.\d+),\s*(-?[\d.]+)\}', src):
    cells.append((m.group(2), m.group(1), float(m.group(4)), int(m.group(5)), float(m.group(6))))
passset = {}
for fn in glob.glob('honest_basis_measured_2026-07-20/*_thr*_e1.txt'):
    b = os.path.basename(fn); coin = b.split('_')[0]; thr = float(b.split('_thr')[1].split('_')[0])
    for ln in open(fn):
        mm = re.match(r'(\d+)\s+([\d.]+)\s+(\d+)\s+\|.*\|\s+(PASS|fail)\s*$', ln.strip())
        if mm: passset[(coin, thr, int(mm.group(1)), float(mm.group(2)))] = (mm.group(4)=='PASS', ln.strip())
surv=[]; fail=[]; unmapped=[]
for ctag, coin, thr, W, g in cells:
    k=(coin,thr,W,g)
    if k not in passset: unmapped.append((ctag,k)); continue
    (surv if passset[k][0] else fail).append((ctag,k,passset[k][1]))
print(f"VERDICT: {len(fail)} FAIL / {len(surv)} PASS / {len(unmapped)} unmapped of {len(cells)}")
for ctag,k,ln in sorted(surv): print(f"  {ctag:28s} thr{k[1]} W{k[2]} g{k[3]} | {ln}")
