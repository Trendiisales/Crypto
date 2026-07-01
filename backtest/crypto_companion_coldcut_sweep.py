#!/usr/bin/env python3
"""
Crypto companion CATASTROPHE COLD-CUT sweep (S-2026-07-01).

Q (operator, from the live SOL companion at MFE 0% / -$6.40 / "rides wide"): if a
crypto companion opens and goes red and NEVER works, or a re-open goes red, we need
a cut-off. But the faithful BTs show every *clip* policy LOSES to WIDE on crypto trend
([[RegimeSwitchTransfer]]). A clip and a CATASTROPHE cut are different objects: a clip
scalps the giveback (kills the runner edge); a catastrophe cut only bounds the tail
disaster that never went green.

This sweeps WIDE + an MFE+time-gated catastrophe cut against pure WIDE, on the SAME
faithful intraday book (imports crypto_companion_intraday_bt so the signal/cost/regime
port is byte-identical). The cut fires ONLY when:
  (a) peak favourable excursion so far <= MFE_EPS  (never went green), AND
  (b) held >= MINHOLD bars                         (not a 1-bar wick), AND
  (c) signed fav <= -cut                           (direction-aware; LONG and SHORT).
A position that showed ANY strength first is EXEMPT -> the cut cannot scalp the runner.

WIRE gate (per CLAUDE.md Adverse-Protection + operator's companion-standalone rule):
ship a non-zero cut ONLY if WIDE+cut keeps tot% flat-or-up vs WIDE with worst%/maxDD
improved, WF both halves, both regimes. Judge the companion book STANDALONE, never vs
"ride wide earns more" -- the cut IS the ride-wide book, just tail-bounded.
"""
import sys
import crypto_companion_intraday_bt as B

MINHOLD = 3
MFE_EPS = 0.003
GRID = [0.0, 0.05, 0.08, 0.12, 0.20, 0.30]


def cold_exit(c, ent, flip, s, cut, minhold=MINHOLD, mfe_eps=MFE_EPS):
    """Catastrophe cut index for a single position, or None. Direction-aware via s."""
    if cut <= 0.0:
        return None
    entry = c[ent]; peak = 0.0
    for k in range(ent + 1, flip + 1):
        fav = s * (c[k] / entry - 1.0)      # signed favourable excursion
        if fav > peak:
            peak = fav
        held = k - ent
        if peak <= mfe_eps and held >= minhold and fav <= -cut:
            return k
    return None


def run_cut(sym, mode, tf, cost, cut):
    ms, c, trades, cost = B.signals(sym, mode, tf, cost)
    wide = []; wcut = []; ncut = 0; cutpnl = []
    for ent, flip, s, bull in trades:
        wx = flip
        wide.append((ms[wx], B.pnl_of(c, ent, wx, s, cost), bull))
        cx = cold_exit(c, ent, flip, s, cut)
        x = flip if cx is None else cx
        p = B.pnl_of(c, ent, x, s, cost)
        wcut.append((ms[x], p, bull))
        if cx is not None:
            ncut += 1; cutpnl.append(p)
    return wide, wcut, ncut, cutpnl


def book_for_cut(cut, legs, costmap):
    wide = []; wcut = []; ncut = 0; cutpnl = []
    for sym, mode, tf in legs:
        w, wc, nc, cp = run_cut(sym, mode, tf, costmap[sym], cut)
        wide += w; wcut += wc; ncut += nc; cutpnl += cp
    return wide, wcut, ncut, cutpnl


def _stats(rows):
    rows = sorted(rows, key=lambda x: x[0]); pnl = [r[1] for r in rows]
    wins = sum(p for p in pnl if p > 0); loss = sum(-p for p in pnl if p < 0)
    pf = wins / loss if loss > 0 else float('inf')
    wr = 100 * sum(1 for p in pnl if p > 0) / len(pnl)
    eq = pk = dd = 0.0
    for p in pnl:
        eq += p; pk = max(pk, eq); dd = min(dd, eq - pk)
    worst = min(pnl) if pnl else 0.0
    return dict(n=len(pnl), pf=pf, wr=wr, tot=100 * sum(pnl),
                worst=100 * worst, dd=100 * dd)


def _line(lbl, s):
    print(f"{lbl:20s} n={s['n']:4d} PF={s['pf']:5.2f} WR={s['wr']:3.0f}% "
          f"tot={s['tot']:8.1f}% worst={s['worst']:7.1f}% maxDD={s['dd']:8.1f}%")


def _half(rows):
    rows = sorted(rows, key=lambda x: x[0]); m = len(rows) // 2
    return rows[:m], rows[m:]


def main():
    scen = sys.argv[1] if len(sys.argv) > 1 else "ladder"
    costmap = B.COST[scen]
    legs = [(s, mode, tf) for tf in ("4h", "1h")
            for mode in ("KELT", "EMAX") for s in ("BTC", "ETH", "SOL")]
    print(f"=== CRYPTO companion CATASTROPHE COLD-CUT sweep | cost={scen} | "
          f"MFE_EPS={MFE_EPS} MINHOLD={MINHOLD} grid={GRID} ===")
    print("WIDE+cut vs pure WIDE (cut=off). WIRE only if tot flat-or-up + worst/maxDD "
          "better, WF-both, both-regime.\n")

    print("--- WHOLE INTRADAY BOOK (12 legs) ---")
    base = None
    for cut in GRID:
        wide, wcut, ncut, cutpnl = book_for_cut(cut, legs, costmap)
        s = _stats(wcut if cut > 0 else wide)
        tag = "WIDE (off)" if cut == 0 else f"WIDE+cut{cut*100:.0f}%"
        if cut == 0:
            base = s
        _line(tag, s)
        if cut > 0 and cutpnl:
            ca = sum(cutpnl) / len(cutpnl) * 100
            print(f"                     cuts={ncut} avg={ca:+.1f}% "
                  f"dtot={s['tot']-base['tot']:+.1f} dworst={s['worst']-base['worst']:+.1f} "
                  f"dmaxDD={s['dd']-base['dd']:+.1f}")

    for tf in ("4h", "1h"):
        legs_tf = [l for l in legs if l[2] == tf]
        print(f"\n--- {tf} book ---")
        base = None
        for cut in GRID:
            _, wcut, ncut, _ = book_for_cut(cut, legs_tf, costmap)
            s = _stats(wcut)
            if cut == 0:
                base = s
            _line(("WIDE (off)" if cut == 0 else f"WIDE+cut{cut*100:.0f}%"), s)

    print("\n--- WF HALVES + REGIME @ best-looking cuts ---")
    for cut in (0.0, 0.20, 0.30):
        _, wcut, _, _ = book_for_cut(cut, legs, costmap)
        h1, h2 = _half(wcut)
        bull = [x for x in wcut if x[2]]; bear = [x for x in wcut if not x[2]]
        tag = "off" if cut == 0 else f"{cut*100:.0f}%"
        print(f"cut={tag}")
        _line("  H1", _stats(h1)); _line("  H2", _stats(h2))
        _line("  BULL", _stats(bull)); _line("  BEAR", _stats(bear))


if __name__ == "__main__":
    main()
