#!/usr/bin/env python3
# ============================================================================
# parent_vs_mimic_report.py  (S-2026-07-12)
#
# Repeatable side-by-side report: for each up-jump coin, the PARENT ride-WIDE
# book (ibkrcrypto_bt --protect-sweep) vs the BE-cascade MIMIC clip book
# (upjump_earlyarm_bt grid, daily). They are USED DIFFERENTLY and coexist
# (additive) — the parent catches the big move, the mimic banks each leg at BE.
# This report keeps the two honest side by side so we deploy BOTH knowingly,
# never conflate their PFs, and can re-run after any data refresh.
#
#   PARENT gate row = vt=0.015 hstop=0.00; OOS_23-26 (fair long-only window).
#   MIMIC = BE-cascade grid cell (best thr), daily, real-fill column.
#   Long-only spot, 2022 omitted, no 200DMA.
#
# Run:  cd /Users/jo/Crypto/backtest && python3 ../ops/parent_vs_mimic_report.py
# ============================================================================
import subprocess, os, re, sys

BT   = "/Users/jo/Crypto/build/ibkrcrypto_bt"
MBIN = "/Users/jo/Crypto/backtest/upjump_earlyarm_bt"
DATA = "/Users/jo/Crypto/backtest/data"
TFMS = {"1d": 86400000, "4h": 14400000, "1h": 3600000}

# coin -> (parent strat, tf) at its winning config
COINS = [
    ("NEAR", "UpJump8",    "1d"), ("AVAX", "UpJump5",    "1d"),
    ("LINK", "UpJump8",    "1d"), ("BCH",  "UpJump4x48", "1d"),
    ("UNI",  "UpJump8",    "1d"), ("LDO",  "UpJump3",    "1d"),
    ("OP",   "UpJump3",    "4h"),
]

def parent_oos(coin, strat, tf):
    """PARENT ride-WIDE: OOS_23-26 net/PF from --protect-sweep vt=0.015 hstop=0."""
    csv = f"{DATA}/{coin}USDT_{tf}.csv"
    if not os.path.exists(csv): return (None, None)
    env = {**os.environ, "BT_TF_MS": str(TFMS[tf]), "COSTBPS": "18"}
    try:
        out = subprocess.run([BT, coin, csv, "--protect-sweep", strat],
                             capture_output=True, text=True, env=env, timeout=120).stdout
    except Exception:
        return (None, None)
    for ln in out.splitlines():
        p = ln.split()
        if len(p) >= 9 and p[0] == "0.015" and p[1] == "0.00" and "OOS_23-26" in ln:
            try: return (float(p[4]), float(p[7]))   # net%, PF
            except Exception: return (None, None)
    return (None, None)

def mimic_best(coin):
    """MIMIC BE-cascade: best-thr net%/PF from the daily grid mode."""
    if not os.path.exists(MBIN): return (None, None, None)
    env = {**os.environ, "UJW_TF": "1d"}
    try:
        out = subprocess.run([MBIN, "grid"], capture_output=True, text=True,
                             env=env, timeout=180, cwd=f"{DATA}/..").stdout
    except Exception:
        return (None, None, None)
    best = (None, None, None)  # net, pf, thr
    for ln in out.splitlines():
        p = ln.split()
        if len(p) >= 12 and p[0] == coin and p[-1] == "PASS":
            try:
                net, pf, thr = float(p[4]), float(p[5]), p[2]
                if best[1] is None or pf > best[1]: best = (net, pf, thr)
            except Exception: pass
    return best

def main():
    print("PARENT (ride-WIDE) vs MIMIC (BE-cascade clip) — repeatable report")
    print("  long-only spot · 2022 omitted · parent=OOS_23-26 vt0.015 · mimic=daily grid best-thr")
    print(f"{'coin':5} {'cfg':11} | {'PARENT net%/PF':>16} | {'MIMIC net(bp)/PF':>18} {'thr':>4} | winner")
    print("-"*80)
    for coin, strat, tf in COINS:
        pn, pp = parent_oos(coin, strat, tf)
        mn, mp, mt = mimic_best(coin)
        ps = f"{pn:+.0f}%/{pp:.2f}" if pp else "—"
        ms = f"{mn:+.0f}/{mp:.2f}" if mp else "—"
        win = "mimic" if (pp and mp and mp > pp) else ("parent" if pp else "—")
        print(f"{coin:5} {strat+' '+tf:11} | {ps:>16} | {ms:>18} {(mt or '—'):>4} | {win}")
    print("\nBoth deploy ADDITIVE (operator rule): parent rides WIDE, mimic clips safe — not one or the other.")

if __name__ == "__main__":
    main()
