#!/usr/bin/env python3
"""BTC-TREND-CORE-V1 certification battery (operator spec 2026-07-20).

Slow long-only BTC trend core: EMA bull regime + Donchian breakout + structural
ATR-clamped stop + 2R protection floor + chandelier/donchian trail + optional
single profit-funded add. Honest accounting:
  - signals/levels from COMPLETED daily bars; execution intraday on 1h bars
  - entry = buy-stop: first 1h bar touching trigger fills at max(open, trigger)
    + half-cost; gap fills beyond trigger+0.75*ATR20 are CANCELLED (chase guard)
  - stop exit fills at min(open, stop) - half-cost (gap-through honest)
  - no clamping, no anchored entries, no booking at levels
Battery: lookback {20,30,55} x EMA {100,150,200} x exit-lookback {10,15,20} x
chandelier ATR {2.5,3.0,3.5} = 81 cells, each with/without add, at RT cost
28 / 56 / 84 bp. Robustness: halves, half-year folds>=70%, PF gates, neighbor
majority, best-trade<20%, best-year<50%, delayed-entry + adverse-fill stress.
"""
import csv, os, random, sys
from collections import defaultdict

DATA = "/Users/jo/Crypto/backtest/data/BTCUSDT_1h.csv"
OUT  = "/Users/jo/Crypto/backtest/btc_trendcore_2026-07-20"
COSTS = (0.0028, 0.0056, 0.0084)          # RT tiers
FLOOR_BP = 0.0066                          # 2 x measured 28bp + 10bp
CHASE_ATR = 0.75
STOP_MIN_ATR, STOP_MAX_ATR = 1.75, 3.00

def load():
    h1 = []
    with open(DATA) as f:
        r = csv.reader(f); next(r)
        for row in r:
            ts = int(row[0]) // 1000
            h1.append((ts, float(row[1]), float(row[2]), float(row[3]), float(row[4])))
    # sanity: x1000 glitch / gap
    for i in range(1, len(h1)):
        ratio = h1[i][4] / h1[i-1][4]
        assert 0.5 < ratio < 2.0, f"1h glitch at {h1[i][0]}: {h1[i-1][4]}->{h1[i][4]}"
    return h1

def daily_of(h1):
    days = {}
    for ts, o, h, l, c in h1:
        d = ts // 86400
        if d not in days: days[d] = [ts, o, h, l, c]
        else:
            b = days[d]
            b[2] = max(b[2], h); b[3] = min(b[3], l); b[4] = c
    out = [(d, *days[d][1:]) for d in sorted(days)]
    return out   # (dayindex, o, h, l, c)

def precompute(daily, ema_n):
    """Per completed daily bar i: ema, ema20ago, atr20, donch highs/lows."""
    n = len(daily)
    ema = [0.0]*n; k = 2.0/(ema_n+1)
    ema[0] = daily[0][4]
    for i in range(1, n):
        ema[i] = daily[i][4]*k + ema[i-1]*(1-k)
    tr = [0.0]*n
    for i in range(n):
        h, l = daily[i][2], daily[i][3]
        pc = daily[i-1][4] if i else daily[i][1]
        tr[i] = max(h-l, abs(h-pc), abs(l-pc))
    atr = [0.0]*n; s = sum(tr[:20])
    for i in range(n):
        if i < 20: atr[i] = sum(tr[:i+1])/(i+1)
        else: s += tr[i]-tr[i-20]; atr[i] = s/20.0
    return ema, atr

def hh(daily, i, n):   # highest high of previous n complete bars ending at i (inclusive)
    return max(b[2] for b in daily[max(0, i-n+1):i+1])
def ll(daily, i, n):
    return min(b[3] for b in daily[max(0, i-n+1):i+1])

def run_cell(h1, daily, day_index, LB, EMA_N, EXIT_LB, CH_ATR, cost_rt,
             with_add, ema, atr, delay_entry=0, extra_fill_bp=0.0,
             entry_stream=None):
    """Returns trade list [(entry_ts, exit_ts, net_ret_frac, R_mult, mfe_R, hold_days, gross_move)].
    entry_stream: optional list of forced entry h1 indices (random null)."""
    half = cost_rt/2.0 + extra_fill_bp
    trades = []
    pos = None   # dict: e, qty(1.0), stop, R, mfe, peak, protected, floor, adds
    cooldown_until_day = -1
    block_until_high_above = None  # after profit exit
    last_entry_trigger = None
    pend_fill = None
    day_ptr = 0
    d4h_close = {}; h4_bars = []   # 4h resample for add trigger
    for ts, o, h, l, c in h1:
        b4 = ts // 14400
        if not h4_bars or h4_bars[-1][0] != b4:
            h4_bars.append([b4, h])
            if len(h4_bars) > 12: h4_bars.pop(0)
        else:
            h4_bars[-1][1] = max(h4_bars[-1][1], h)
        d = ts // 86400
        di = day_index.get(d - 1)     # last COMPLETED daily bar
        if di is None or di < max(LB, EMA_N, 20) + 1: continue
        A = atr[di]
        regime = (daily[di][4] > ema[di] and ema[di] > ema[di-20])
        trigger = hh(daily, di, LB) + 0.10*A

        if pos is None and entry_stream is None:
            armed = regime
            if d <= cooldown_until_day:
                # loss cooldown: early release on a materially higher fresh trigger
                if last_entry_trigger is not None and trigger >= last_entry_trigger + 0.5*A:
                    armed = armed and True
                else:
                    armed = False
            if block_until_high_above is not None:
                if hh(daily, di, LB) > block_until_high_above: block_until_high_above = None
                else: armed = False
            if armed and h >= trigger:
                fill = max(o, trigger)
                if fill > trigger + CHASE_ATR*A:
                    pass   # chase guard: cancelled this bar
                else:
                    if delay_entry:
                        pend_fill = (trigger, delay_entry)   # fill at next bar open
                    else:
                        e = fill*(1+half)
                        stop_raw = ll(daily, di, 10) - 0.10*A
                        dist = min(max(e - stop_raw, STOP_MIN_ATR*A), STOP_MAX_ATR*A)
                        pos = dict(e=e, ts=ts, stop=e-dist, R=dist, mfe=e, peak=c,
                                   protected=False, add=None, di0=di)
                        last_entry_trigger = trigger
                    continue
        elif pos is None and entry_stream is not None:
            if entry_stream and ts >= entry_stream[0]:
                entry_stream.pop(0)
                e = o*(1+half)
                stop_raw = ll(daily, di, 10) - 0.10*A
                dist = min(max(e - stop_raw, STOP_MIN_ATR*A), STOP_MAX_ATR*A)
                pos = dict(e=e, ts=ts, stop=e-dist, R=dist, mfe=e, peak=c,
                           protected=False, add=None, di0=di)
            continue
        if pos is None and pend_fill is not None:
            trig, cnt = pend_fill
            if cnt > 1: pend_fill = (trig, cnt-1)
            else:
                pend_fill = None
                e = o*(1+half)
                stop_raw = ll(daily, di, 10) - 0.10*A
                dist = min(max(e - stop_raw, STOP_MIN_ATR*A), STOP_MAX_ATR*A)
                pos = dict(e=e, ts=ts, stop=e-dist, R=dist, mfe=e, peak=c,
                           protected=False, add=None, di0=di)
                last_entry_trigger = trig
            continue
        if pos is None: continue

        # ---- in position ----
        # queued daily-close exit fills at THIS bar's open (next liquid opportunity)
        if pos.get("exit_next"):
            x = o*(1-half)
            trades.append(close_trade(pos, x, ts, cost_rt))
            if x < pos["e"]: cooldown_until_day = d + 10
            else: block_until_high_above = pos["peak"]
            pos = None
            continue
        pos["mfe"] = max(pos["mfe"], h)
        pos["peak"] = max(pos["peak"], c)
        mfe_R = (pos["mfe"] - pos["e"]) / pos["R"]
        # protection activation
        if not pos["protected"] and mfe_R >= 2.0:
            pos["protected"] = True
            pos["floor"] = pos["e"]*(1+FLOOR_BP)
            pos["stop"] = max(pos["stop"], pos["floor"])
        # daily trail update (on first 1h bar of a new day, di just advanced)
        if pos["protected"] and di > pos.get("last_trail_di", -1):
            pos["last_trail_di"] = di
            hi_close = max(b[4] for b in daily[pos["di0"]:di+1])
            chand = hi_close - CH_ATR*A
            donch = ll(daily, di, EXIT_LB) - 0.10*A
            pos["stop"] = max(pos["stop"], pos["floor"], chand, donch)
        # profit-funded single add
        if (with_add and pos["protected"] and pos["add"] is None
                and mfe_R >= 2.0 and len(h4_bars) >= 11):
            hh4 = max(x[1] for x in h4_bars[-11:-1])
            if h4_bars[-1][1] > hh4:
                ea = c*(1+half)
                # combined-stop preservation check at 2x cost
                st = pos["stop"]
                net2 = (st/pos["e"]-1-2*cost_rt) + 0.5*(st/ea-1-2*cost_rt)
                if net2*pos["e"]/pos["R"] >= 0.25:
                    pos["add"] = dict(e=ea, ts=ts)
        # stop hit (intraday, gap honest)
        if l <= pos["stop"]:
            x = min(o, pos["stop"])*(1-half)
            trades.append(close_trade(pos, x, ts, cost_rt))
            if x < pos["e"]:
                cooldown_until_day = d + 10
            else:
                block_until_high_above = pos["peak"]
            pos = None
            continue
        # daily-close exits (evaluate on last 1h bar of the day; fill next bar open)
        if (ts + 3600) // 86400 != d:
            days_in = d - (pos["ts"]//86400)
            dtoday = day_index.get(d, di)
            # time exit
            if days_in >= 15 and mfe_R < 1.0 and c <= pos["e"]*(1+cost_rt):
                pos["exit_next"] = True
            # regime exit (today's completing close vs today's EMA)
            if c < ema[dtoday]:
                pos["exit_next"] = True
    if pos is not None:
        x = h1[-1][4]*(1-half)
        trades.append(close_trade(pos, x, h1[-1][0], cost_rt))
    return trades

def close_trade(pos, x, ts, cost_rt):
    # costs live inside the fill prices (half-RT per side on every fill)
    core = x/pos["e"] - 1
    if pos["add"] is not None:
        addr = x/pos["add"]["e"] - 1
        net = (core + 0.5*addr) / 1.5    # qty-weighted, per-notional
    else:
        net = core
    Rm = (x - pos["e"]) / pos["R"]
    mfe_R = (pos["mfe"] - pos["e"]) / pos["R"]
    hold = (ts - pos["ts"]) / 86400.0
    return (pos["ts"], ts, net, Rm, mfe_R, hold, pos["add"] is not None)

def stats(trades):
    if not trades: return dict(n=0, net=0, pf=0, avgwin=0, nwin=0)
    rets = [t[2] for t in trades]
    pos = sum(r for r in rets if r > 0); neg = -sum(r for r in rets if r < 0)
    wins = [r for r in rets if r > 0]
    return dict(n=len(rets), net=sum(rets), pf=pos/neg if neg > 0 else 99.0,
                avgwin=(sum(wins)/len(wins) if wins else 0.0), nwin=len(wins),
                best=max(rets), grosspos=pos)

def folds_ok(trades, t0, t1):
    """half-year folds: >=70% of populated folds positive."""
    if not trades: return 0.0
    fold = defaultdict(float)
    for t in trades:
        fold[int((t[0]-t0)//(182*86400))] += t[2]
    vals = list(fold.values())
    return sum(1 for v in vals if v > 0)/len(vals)

def year_net(trades):
    y = defaultdict(float)
    for t in trades:
        import datetime
        y[datetime.datetime.utcfromtimestamp(t[0]).year] += t[2]
    return dict(y)

def main():
    os.makedirs(OUT, exist_ok=True)
    h1 = load()
    daily = daily_of(h1)
    day_index = {daily[i][0]: i for i in range(len(daily))}
    t0, t1 = h1[0][0], h1[-1][0]
    LBs, EMAs, EXs, CHs = [20,30,55], [100,150,200], [10,15,20], [2.5,3.0,3.5]
    pre = {e: precompute(daily, e) for e in EMAs}
    results = {}
    for LB in LBs:
        for EN in EMAs:
            ema, atr = pre[EN]
            for EX in EXs:
                for CH in CHs:
                    key = (LB,EN,EX,CH)
                    cell = {}
                    for wa in (False, True):
                        for ci, cost in enumerate(COSTS):
                            tr = run_cell(h1, daily, day_index, LB, EN, EX, CH,
                                          cost, wa, ema, atr)
                            cell[(wa,ci)] = tr
                        # stress at 1x only
                        cell[(wa,'delay')] = run_cell(h1, daily, day_index, LB, EN, EX, CH,
                                                      COSTS[0], wa, ema, atr, delay_entry=1)
                        cell[(wa,'adv')] = run_cell(h1, daily, day_index, LB, EN, EX, CH,
                                                    COSTS[0], wa, ema, atr, extra_fill_bp=0.0010)
                    results[key] = cell
    # judge
    lines = []
    passing = []
    for key, cell in sorted(results.items()):
        for wa in (False, True):
            s1, s2, s3 = (stats(cell[(wa,i)]) for i in range(3))
            tr1 = cell[(wa,0)]
            if s1["n"] == 0:
                continue
            f = folds_ok(tr1, t0, t1)
            mid = tr1[len(tr1)//2][0] if tr1 else t0
            h1n = sum(t[2] for t in tr1 if t[0] < mid); h2n = sum(t[2] for t in tr1 if t[0] >= mid)
            yn = year_net(tr1)
            besty = max(yn.values()) if yn else 0
            sd, sa = stats(cell[(wa,'delay')]), stats(cell[(wa,'adv')])
            gates = dict(
                halves = h1n > 0 and h2n > 0,
                folds  = f >= 0.70,
                pf1    = s1["pf"] >= 1.35,
                pf2    = s2["pf"] >= 1.15,
                net3   = s3["net"] > -0.10,
                besttr = (s1["best"] < 0.20*s1["grosspos"]) if s1["grosspos"]>0 else False,
                besty  = (besty < 0.50*s1["net"]) if s1["net"]>0 else False,
                delay  = sd["net"] > 0,
                adv    = sa["net"] > 0,
            )
            npass = sum(gates.values())
            allp = all(gates.values())
            avg_hold = sum(t[5] for t in tr1)/len(tr1)
            tag = f"LB{key[0]}/EMA{key[1]}/EX{key[2]}/CH{key[3]}{'+ADD' if wa else ''}"
            lines.append(f"{tag:32s} n={s1['n']:3d} net={s1['net']*1e4:+7.0f}bp PF={min(s1['pf'],99):5.2f} "
                         f"2xPF={min(s2['pf'],99):5.2f} 3xnet={s3['net']*1e4:+7.0f} folds={f:.0%} "
                         f"H1={h1n*1e4:+6.0f} H2={h2n*1e4:+6.0f} avgwin={s1['avgwin']*1e4:+5.0f}bp "
                         f"hold={avg_hold:4.1f}d gates={npass}/9{' ALLPASS' if allp else ''}")
            if allp: passing.append((key, wa, s1))
    # neighbor test on passing cells
    def neighbors(key):
        out = []
        for dim, vals in enumerate((LBs, EMAs, EXs, CHs)):
            i = vals.index(key[dim])
            for j in (i-1, i+1):
                if 0 <= j < len(vals):
                    k2 = list(key); k2[dim] = vals[j]; out.append(tuple(k2))
        return out
    final = []
    for key, wa, s1 in passing:
        nb = neighbors(key)
        pos = sum(1 for k in nb if stats(results[k][(wa,0)])["net"] > 0)
        ok = pos > len(nb)/2
        final.append((key, wa, s1, f"{pos}/{len(nb)}", ok))
    with open(f"{OUT}/cert_results.txt", "w") as fo:
        fo.write("\n".join(lines) + "\n\n== ALLPASS cells + neighbor majority ==\n")
        for key, wa, s1, nbs, ok in final:
            fo.write(f"LB{key[0]}/EMA{key[1]}/EX{key[2]}/CH{key[3]}{'+ADD' if wa else ''} "
                     f"net={s1['net']*1e4:+.0f}bp PF={s1['pf']:.2f} neighbors+{nbs} "
                     f"{'STABLE' if ok else 'ISOLATED'}\n")
    print("\n".join(lines[-40:]))
    print(f"\nALLPASS: {len(passing)} / {len(lines)} cell-variants; stable: {sum(1 for *_x, ok in final if ok)}")
    print(f"full: {OUT}/cert_results.txt")

if __name__ == "__main__":
    main()
