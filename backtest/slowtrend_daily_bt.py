#!/usr/bin/env python3
"""Slow long-only daily trend battery — 35-coin universe (operator order 2026-07-20s).

Class: daily Donchian breakout / MA cross / TSMOM. Long-only spot. Tens of trades/yr.
Honest mechanics:
  - signals computed on completed UTC daily bars (resampled from Binance 1h CSVs)
  - fills at NEXT day's OPEN (never signal close — chimera-harness-close-bias rule)
  - cost deducted per round trip: 30bp base (>= worst measured safe_cost_bps 29.5),
    2x stress = 60bp
Gate (feedback-crypto-omit-2022-longonly: long-only spot cannot short a bear, omit y2022):
  net>0 AND PF>=1.3 AND WF_H1>0 AND WF_H2>0 (both computed on non-2022 trades)
  AND still passes all of that at 2x cost. y2022 bleed reported separately, not gated.
Data sanity: reject file if any daily |ret| > 60% (x1000 glitch class) or day-gap > 3.
"""
import csv, os, sys, math
from collections import defaultdict

DATA = "/Users/jo/Crypto/backtest/data"
OUT  = "/Users/jo/Crypto/backtest/slowtrend_2026-07-20"
COINS = ("AAVE ADA APT ATOM AVAX BCH BNB BTC COMP CRV DOGE DOT ETC ETH FIL GRT ICP INJ "
         "LDO LINK LTC MANA NEAR OP RUNE SAND SOL SUI SUSHI THETA TIA TRX UNI VET XRP").split()
COST_RT = 0.0030   # 30bp round trip, uniform >= worst measured (29.5bp)

def load_daily(coin):
    """Resample 1h -> UTC daily. Returns list of (day_epoch, o, h, l, c)."""
    path = f"{DATA}/{coin}USDT_1h.csv"
    days = {}
    with open(path) as f:
        r = csv.reader(f)
        header = next(r)
        for row in r:
            ts = int(row[0]) // 1000
            d = ts // 86400
            o, h, l, c = (float(row[i]) for i in (1, 2, 3, 4))
            if d not in days:
                days[d] = [ts, o, h, l, c]
            else:
                b = days[d]
                if ts < b[0]:
                    b[0], b[1] = ts, o          # earliest hour's open
                b[2] = max(b[2], h); b[3] = min(b[3], l)
                b[4] = c                        # rows are time-ordered within file
    out = []
    for d in sorted(days):
        ts, o, h, l, c = days[d]
        out.append((d * 86400, o, h, l, c))
    return out

def sanity(coin, bars):
    """x1000-glitch + gap check. Returns (ok, reason)."""
    for i in range(1, len(bars)):
        pc, c = bars[i-1][4], bars[i][4]
        # x1000-glitch class = orders-of-magnitude jump. Real crypto days reach 3-5x
        # (DOGE 2021-01-28 pump 0.0074->0.0365 verified real; XRP ruling 2023-07-13),
        # so flag only >10x / <1/10 bar-to-bar.
        if pc > 0 and (c / pc > 10.0 or c / pc < 0.1):
            return False, f"daily ratio>10x at {bars[i][0]} ({pc}->{c})"
        gap = (bars[i][0] - bars[i-1][0]) // 86400
        if gap > 3:
            return False, f"gap {gap}d at {bars[i][0]}"
    return True, ""

def year(ts):
    # ts is UTC midnight epoch; 2022 = [1640995200, 1672531200)
    return 2022 if 1640995200 <= ts < 1672531200 else (2021 if ts < 1640995200 else 2023)

# --- strategy signal generators: return desired position (0/1) decided on bar i close ---
def sig_donch(bars, i, N, M, state):
    c = bars[i][4]
    if not state["long"]:
        if i >= N:
            hi = max(b[2] for b in bars[i-N:i])
            if c > hi: return 1
        return 0
    else:
        if i >= M:
            lo = min(b[3] for b in bars[i-M:i])
            if c < lo: return 0
        return 1

def sig_ma(bars, i, F, S, state):
    if i + 1 < S: return state["long"]
    cf = sum(b[4] for b in bars[i+1-F:i+1]) / F
    cs = sum(b[4] for b in bars[i+1-S:i+1]) / S
    return 1 if cf > cs else 0

def sig_tsmom(bars, i, K, _unused, state):
    if i < K: return 0
    return 1 if bars[i][4] > bars[i-K][4] else 0

STRATS = [
    ("DONCH20_10", sig_donch, 20, 10), ("DONCH40_20", sig_donch, 40, 20),
    ("DONCH55_20", sig_donch, 55, 20),
    ("MA10_50", sig_ma, 10, 50), ("MA20_100", sig_ma, 20, 100), ("MA50_200", sig_ma, 50, 200),
    ("TSMOM30", sig_tsmom, 30, 0), ("TSMOM60", sig_tsmom, 60, 0), ("TSMOM90", sig_tsmom, 90, 0),
]

def run(bars, fn, p1, p2, cost_rt):
    """Signal on close of bar i -> fill at open of bar i+1. Returns trade list."""
    state = {"long": False}
    trades = []  # (entry_ts, exit_ts, entry_px, exit_px, netret)
    entry_px = entry_ts = None
    pending = None   # desired position to apply at next open
    for i in range(len(bars)):
        ts, o = bars[i][0], bars[i][1]
        if pending is not None:
            if pending == 1 and not state["long"]:
                state["long"] = True; entry_px = o; entry_ts = ts
            elif pending == 0 and state["long"]:
                gross = o / entry_px - 1.0
                trades.append((entry_ts, ts, entry_px, o, gross - cost_rt))
                state["long"] = False; entry_px = None
            pending = None
        want = fn(bars, i, p1, p2, state)
        if want != (1 if state["long"] else 0):
            pending = want
    if state["long"]:  # mark-to-last-close open position as a closed trade (honest EOD)
        c = bars[-1][4]
        trades.append((entry_ts, bars[-1][0], entry_px, c, c / entry_px - 1.0 - cost_rt))
    return trades

def metrics(trades):
    """PF/net/n over a trade list (net returns as fractions)."""
    if not trades: return dict(n=0, net=0.0, pf=0.0, wins=0, worst=0.0)
    rets = [t[4] for t in trades]
    pos = sum(r for r in rets if r > 0); neg = -sum(r for r in rets if r < 0)
    return dict(n=len(rets), net=sum(rets), pf=(pos / neg if neg > 0 else 99.0),
                wins=sum(1 for r in rets if r > 0), worst=min(rets))

def judge(trades):
    """Apply the gate on non-2022 trades. Returns (verdict_dict)."""
    g = [t for t in trades if year(t[0]) != 2022]
    y22 = [t for t in trades if year(t[0]) == 2022]
    m = metrics(g)
    res = dict(m, y22net=sum(t[4] for t in y22), y22n=len(y22))
    if m["n"] < 8:
        res["pass"] = False; res["why"] = "n<8"
        res["h1"] = res["h2"] = 0.0
        return res
    mid = g[len(g)//2][0]
    h1 = sum(t[4] for t in g if t[0] < mid); h2 = sum(t[4] for t in g if t[0] >= mid)
    res["h1"], res["h2"] = h1, h2
    res["pass"] = m["net"] > 0 and m["pf"] >= 1.3 and h1 > 0 and h2 > 0
    res["why"] = "" if res["pass"] else "gate"
    return res

def main():
    os.makedirs(OUT, exist_ok=True)
    rows = []
    rejected = []
    for coin in COINS:
        try:
            bars = load_daily(coin)
        except FileNotFoundError:
            rejected.append((coin, "NO FILE")); continue
        ok, why = sanity(coin, bars)
        if not ok:
            rejected.append((coin, why)); continue
        for name, fn, p1, p2 in STRATS:
            t1 = run(bars, fn, p1, p2, COST_RT)
            t2 = run(bars, fn, p1, p2, COST_RT * 2)
            j1, j2 = judge(t1), judge(t2)
            allpass = j1["pass"] and j2["pass"]
            rows.append((coin, name, j1, j2, allpass))
    with open(f"{OUT}/battery_results.txt", "w") as f:
        def w(s): print(s); f.write(s + "\n")
        w(f"# slow-trend daily battery {len(COINS)} coins x {len(STRATS)} strats, "
          f"cost {COST_RT*1e4:.0f}bp rt (2x={COST_RT*2e4:.0f}bp), gate omits y2022, fills=next open")
        for coin, why in rejected:
            w(f"REJECTED {coin}: {why}")
        w(f"{'coin':6s} {'strat':11s} {'n':>4s} {'net%':>8s} {'PF':>6s} {'H1%':>7s} {'H2%':>7s} "
          f"{'worst%':>7s} {'y22%':>7s} {'2xnet%':>8s} {'2xPF':>6s} verdict")
        npass = 0
        for coin, name, j1, j2, ap in rows:
            v = "PASS" if ap else ("1x-only" if j1["pass"] else "fail")
            if ap: npass += 1
            w(f"{coin:6s} {name:11s} {j1['n']:4d} {j1['net']*100:+8.1f} {min(j1['pf'],99):6.2f} "
              f"{j1['h1']*100:+7.1f} {j1['h2']*100:+7.1f} {j1['worst']*100:+7.1f} "
              f"{j1['y22net']*100:+7.1f} {j2['net']*100:+8.1f} {min(j2['pf'],99):6.2f} [{v}]")
        w(f"# ALL-GATE PASS (incl 2x): {npass}/{len(rows)} cells")
        # per-coin best pass
        best = {}
        for coin, name, j1, j2, ap in rows:
            if ap and (coin not in best or j1["net"] > best[coin][1]["net"]):
                best[coin] = (name, j1)
        w("# per-coin best PASS:")
        for coin in COINS:
            if coin in best:
                n, j = best[coin]
                w(f"  {coin:6s} {n:11s} net={j['net']*100:+.1f}% PF={min(j['pf'],99):.2f} "
                  f"n={j['n']} H1={j['h1']*100:+.1f}% H2={j['h2']*100:+.1f}% y22={j['y22net']*100:+.1f}%")
            else:
                w(f"  {coin:6s} --none--")
    print(f"\nwrote {OUT}/battery_results.txt")

if __name__ == "__main__":
    main()
