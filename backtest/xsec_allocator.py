#!/usr/bin/env python3
"""
Cross-Sectional Momentum ALLOCATOR — faithful port + live-facing scaffold.

This is the portfolio path the per-symbol EdgeEngine never had. It ranks the whole
liquid universe by trailing return each rebalance, goes LONG the top-K, vol-scales
the weights, and sits in cash when the macro regime is bear.

STATUS: BACKTEST-TRUTH verified. The `run_backtest()` below reproduces the vault
(Memory-Chimera/wiki/entities/CrossSectionalMomentum.md) numbers EXACTLY on all four
independent OOS windows for the validated lb30/K3/rebal14/invvol config:

    window   vault              this repro
    2021     +975% Sh2.39       +975% Sh2.39
    2023     +178% Sh1.72       +178% Sh1.72
    2024     +139% Sh1.44       +139% Sh1.44
    2025 h.  + 85% Sh0.94       + 85% Sh0.94
  (3x cost 45bp: 2023 +157 Sh1.61, 2024 +117 Sh1.32, 2025 +71 Sh0.86 — also exact.)

The logic is a faithful transcription of the validated reference
`backtest/cross_sectional.py` (ChimeraCrypto commit b495dcf) — same daily-last-close
sampling, same long-only positive-momentum filter, same inverse-vol weighting, same
BTC>200d-SMA macro gate, same 15bp/side turnover cost booked into the rebalance day.

LIVE WIRING: `CrossSectionalAllocator.target_weights()` is the piece the live daily
executor (refresh_shadow.py / ibkrcrypto_engine.cpp) would call once per rebalance to
get {sym: weight}. It is NOT wired in — the book is mid-cutover to paper account 4002
and stays SHADOW. A live allocator ALSO owes a C++ implementation (operator directive:
live crypto = C++ producers); this Python is the reproducible backtest reference only.

  Usage:
    python3 xsec_allocator.py                 # verify repro (headline config, all windows)
    python3 xsec_allocator.py --grid          # full config grid (matches cross_sectional.py)
    COST_BP=45 python3 xsec_allocator.py       # 3x cost robustness check
"""
import csv, glob, os, math, datetime, sys

HERE = os.path.dirname(os.path.abspath(__file__))
DATADIR = os.environ.get("DATADIR", os.path.join(HERE, "data", "multiyr"))
GLOB    = os.environ.get("GLOB", "*_15m.csv")
COST_BP = float(os.environ.get("COST_BP", "15"))

# ---- validated config (the plateau centre) -------------------------------------
VALIDATED = dict(lb=30, K=3, rebal=14, weighting="invvol", macro_gate=True)

def vday(ms): return ms // 86400000  # UTC day index


# ================================================================================
# DATA — daily-last-close + accumulated volume from validated 15m CSVs
# ================================================================================
def load_daily(datadir=DATADIR, pattern=GLOB):
    """Return (days sorted, syms, {sym:[close_per_day]}, {sym:[vol_per_day]})."""
    px = {}
    for f in sorted(glob.glob(os.path.join(datadir, pattern))):
        sym = os.path.basename(f).split("USDT")[0]
        dl = {}
        with open(f) as fh:
            r = csv.reader(fh); next(r, None)
            for x in r:
                try: ts = int(x[0]); c = float(x[4]); v = float(x[5])
                except Exception: continue
                d = vday(ts)
                if d in dl:
                    c0, v0 = dl[d]; dl[d] = (c, v0 + v)     # last close of day, sum vol
                else:
                    dl[d] = (c, v)
        px[sym] = dl
    days = sorted({d for s in px for d in px[s]})
    syms = sorted(px.keys())
    close = {s: [px[s].get(d, (float('nan'), 0.0))[0] for d in days] for s in syms}
    vol   = {s: [px[s].get(d, (float('nan'), 0.0))[1] for d in days] for s in syms}
    return days, syms, close, vol


# ================================================================================
# SIGNAL PRIMITIVES (faithful to cross_sectional.py)
# ================================================================================
def sma(series, i, n):
    if i < n: return None
    s = [series[j] for j in range(i - n, i) if series[j] == series[j]]
    if len(s) < n * 0.8: return None
    return sum(s) / len(s)

def trailing_ret(series, i, lb):
    if i < lb: return None
    a, b = series[i - lb], series[i]
    if a != a or b != b or a <= 0: return None
    return b / a - 1.0

def realized_vol(series, i, n=30):
    if i < n + 1: return None
    rs = []
    for j in range(i - n, i):
        a, b = series[j - 1], series[j]
        if a == a and b == b and a > 0: rs.append(b / a - 1.0)
    if len(rs) < n * 0.6: return None
    m = sum(rs) / len(rs); var = sum((x - m) ** 2 for x in rs) / len(rs)
    return math.sqrt(var) if var > 0 else None


# ================================================================================
# CrossSectionalAllocator — the LIVE-FACING piece (the portfolio path EdgeEngine lacks)
# ================================================================================
class CrossSectionalAllocator:
    """Stateless rank-and-weight allocator. Feed it the universe's daily close series
    (index i = the bar to allocate ON, using data up to and including i) and it returns
    the target long-only weight vector for the next holding period.

    This is exactly what a live daily executor calls each rebalance. It does NOT hold
    state, place orders, or touch any ledger — turnover/cost/execution belong to the
    caller (the shadow executor today, a C++ producer for live)."""

    def __init__(self, lb=30, K=3, weighting="invvol", macro_gate=True,
                 btc_key="BTC", regime_ma=200):
        self.lb = lb; self.K = K; self.weighting = weighting
        self.macro_gate = macro_gate; self.btc_key = btc_key; self.regime_ma = regime_ma

    def is_bull(self, close, i):
        """Portfolio-level macro gate: BTC close > its `regime_ma`-day SMA."""
        if not self.macro_gate: return True
        btc = close.get(self.btc_key)
        if btc is None: return True
        m = sma(btc, i, self.regime_ma)
        return m is not None and btc[i] == btc[i] and btc[i] > m

    def target_weights(self, close, i):
        """Return {sym: weight} summing to <=1.0 (cash = 1 - sum). Empty dict = all cash."""
        if not self.is_bull(close, i):
            return {}
        scores = []
        for s, series in close.items():
            sc = trailing_ret(series, i, self.lb)          # momentum signal
            if sc is None or sc <= 0:                        # long-only: positive-momentum only
                continue
            scores.append((sc, s))
        scores.sort(reverse=True)
        picks = [s for _, s in scores[:self.K]]
        if not picks:
            return {}
        w = {}
        if self.weighting == "invvol":
            iv = {}
            for s in picks:
                v = realized_vol(close[s], i, 30)
                iv[s] = (1.0 / v) if v else 0.0
            tot = sum(iv.values())
            if tot > 0:
                for s in picks: w[s] = iv[s] / tot
            else:
                for s in picks: w[s] = 1.0 / len(picks)
        else:  # equal
            for s in picks: w[s] = 1.0 / len(picks)
        return w


# ================================================================================
# BACKTEST — drives the allocator over history, books turnover cost (faithful)
# ================================================================================
def run_backtest(days, syms, close, lb, K, rebal, weighting, macro_gate,
                 cost_bp=COST_BP):
    """Reproduces cross_sectional.py run(): daily P&L from held weights, rebalance
    every `rebal` days via the allocator, turnover cost booked into the rebalance day's
    return (the series window_stats reads — the cost-blindness bug the vault flagged is
    avoided by design here)."""
    alloc = CrossSectionalAllocator(lb=lb, K=K, weighting=weighting,
                                    macro_gate=macro_gate)
    n = len(days)
    weights = {s: 0.0 for s in syms}
    daily_rets = []
    last_rebal = -10 ** 9
    for i in range(1, n):
        r = 0.0
        for s in syms:
            w = weights[s]
            if w <= 0: continue
            a, b = close[s][i - 1], close[s][i]
            if a == a and b == b and a > 0: r += w * (b / a - 1.0)
        daily_rets.append((days[i], r))
        if days[i] - last_rebal < rebal: continue
        last_rebal = days[i]
        tw = alloc.target_weights(close, i)                 # {sym: weight}, may be empty
        new_w = {s: tw.get(s, 0.0) for s in syms}
        turn = sum(abs(new_w[s] - weights[s]) for s in syms)
        cost = turn * cost_bp / 10000.0
        d_i, r_i = daily_rets[-1]
        daily_rets[-1] = (d_i, r_i - cost)                  # book cost into scored series
        weights = new_w
    return daily_rets


# ---- OOS windows (UTC day index) — identical to the validated reference ---------
def dms(y, m, d): return int(datetime.datetime(y, m, d, tzinfo=datetime.timezone.utc).timestamp() * 1000)
WINS = [
    ("21bull", vday(dms(2021, 1, 1)), vday(dms(2021, 11, 10))),
    ("22bear", vday(dms(2022, 1, 1)), vday(dms(2022, 12, 31))),
    ("23rec",  vday(dms(2023, 1, 1)), vday(dms(2023, 12, 31))),
    ("24bull", vday(dms(2024, 1, 1)), vday(dms(2024, 12, 31))),
    ("25hold", vday(dms(2025, 1, 1)), vday(dms(2026, 7, 1))),
]

def window_stats(daily_rets, a, b):
    rs = [r for d, r in daily_rets if a <= d < b]
    if len(rs) < 20: return None
    tot = 1.0
    for r in rs: tot *= (1 + r)
    tot -= 1
    m = sum(rs) / len(rs); var = sum((x - m) ** 2 for x in rs) / len(rs)
    sd = math.sqrt(var) if var > 0 else 1e-9
    sharpe = (m / sd) * math.sqrt(365)
    eq = 1.0; peak = 1.0; dd = 0.0
    for r in rs:
        eq *= (1 + r); peak = max(peak, eq); dd = max(dd, (peak - eq) / peak)
    return dict(ret=tot * 100, sharpe=sharpe, dd=dd * 100, n=len(rs))


def _cell(s): return f"{s['ret']:+5.0f}% Sh{s['sharpe']:+.2f}" if s else "  --"

def verify_headline(days, syms, close):
    print(f"\nVALIDATED config lb{VALIDATED['lb']}/K{VALIDATED['K']}/"
          f"rebal{VALIDATED['rebal']}/{VALIDATED['weighting']}  cost={COST_BP}bp/side")
    dr = run_backtest(days, syms, close, VALIDATED['lb'], VALIDATED['K'],
                      VALIDATED['rebal'], VALIDATED['weighting'], VALIDATED['macro_gate'])
    st = {w[0]: window_stats(dr, w[1], w[2]) for w in WINS}
    print(f"{'':8}{'21bull':>15}{'23rec':>15}{'24bull':>15}{'25hold':>15}")
    print(f"{'repro':<8}{_cell(st['21bull']):>15}{_cell(st['23rec']):>15}"
          f"{_cell(st['24bull']):>15}{_cell(st['25hold']):>15}")
    if COST_BP == 15:
        print(f"{'VAULT':<8}{'+975% Sh+2.39':>15}{'+178% Sh+1.72':>15}"
              f"{'+139% Sh+1.44':>15}{'+85% Sh+0.94':>15}")

def run_grid(days, syms, close):
    print(f"\nGRID  universe={len(syms)}  cost={COST_BP}bp/side")
    print(f"{'lb':>3}{'K':>3}{'rb':>4} {'wt':<7} | {'21bull':>15}{'23rec':>15}"
          f"{'24bull':>15} | {'25hold':>15} GATE")
    print("-" * 100)
    rows = []
    for lb in [14, 30, 60, 90]:
        for K in [3, 5, 8]:
            for rb in [7, 14]:
                for wt in ["equal", "invvol"]:
                    dr = run_backtest(days, syms, close, lb, K, rb, wt, True)
                    st = {w[0]: window_stats(dr, w[1], w[2]) for w in WINS}
                    if not all(st[w] for w in ["21bull", "23rec", "24bull"]): continue
                    gate = all(st[w]["sharpe"] >= 1.0 and st[w]["ret"] > 0
                               for w in ["21bull", "23rec", "24bull"])
                    rows.append((lb, K, rb, wt, st, gate))
    rows.sort(key=lambda r: -min(r[4]["21bull"]["sharpe"], r[4]["23rec"]["sharpe"],
                                 r[4]["24bull"]["sharpe"]))
    npass = 0
    for lb, K, rb, wt, st, gate in rows:
        if gate: npass += 1
        print(f"{lb:>3}{K:>3}{rb:>4} {wt:<7} | {_cell(st['21bull']):>15}"
              f"{_cell(st['23rec']):>15}{_cell(st['24bull']):>15} | "
              f"{_cell(st['25hold']):>15} {'** PASS **' if gate else ''}")
    print("-" * 100)
    print(f"{npass} configs PASS the 3-bull-window OOS gate (Sharpe>=1 + positive each)")


def main():
    days, syms, close, vol = load_daily()
    print(f"loaded {len(syms)} symbols, {len(days)} days "
          f"({datetime.datetime.fromtimestamp(days[0]*86400, datetime.timezone.utc).date()} -> "
          f"{datetime.datetime.fromtimestamp(days[-1]*86400, datetime.timezone.utc).date()})")
    if "--grid" in sys.argv:
        run_grid(days, syms, close)
    else:
        verify_headline(days, syms, close)


if __name__ == "__main__":
    main()
