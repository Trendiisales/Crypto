# ONDO jump_floor spot-long — NO-GO (in-sample, 2026-07-16)

**Verdict: DO NOT WIRE an always-armed ONDO `jump_floor` det cell.** Operator call
(S-2026-07-16r): "Skip ONDO — don't wire." Honors BACKTEST_TRUTH.

## Context
Prior session (handoff) queued "wire ONDO as a floored jump_floor spot long like
AAVE/ETH/GRT" after operator saw ONDO **+15.86%** live surging and asked *"why can we
not trade a spot long that is surging."* The handoff assumed ONDO would look like the
viable PJ family. It does not.

## Data
- Pulled ONDO 1h via `fetch_coin_1h.py` → `data/ONDOUSDT_1h.csv`, 11,059 rows,
  **2025-04-11 → 2026-07-16 (~15 months)**. Integrity clean (min 0.2251, max 1.1431,
  zero 3× jumps).
- Price path = **structural downtrend**: 0.91 (2025-04) → peak 1.14 → **0.26 (2026-02)**
  → range-bound 0.26–0.37 for the last ~6 months. Down ~60% over the window. The
  +15.86% the operator saw is **intraday chop at the lows**, not a trend.

## In-sample sweep (upjump2pct_be_bt, ONDO added to COINS[])
- `percoin` (936-lever plateau-gated sweep): **NO-CELL** — no plateau-validated PASS.
- Manual grid W∈{1,6,24} × thr∈{5,7,10}% × s∈{4,6}% × g=1.0 (18 configs): **18/18 net-NEGATIVE.**
  - Best (least-bad): W24/thr7%/s4%/g1.0 → n=83, net **−1677bp**, PF 0.82, WF-H1 +2779 /
    **WF-H2 −4456** (fails walk-forward). Still net-negative.
  - Typical: W6/thr5%/s4% → n=84, **23%** of entries hit the pre-BE stop, net −5429bp.

## Why the floor does NOT rescue it
The BE floor bounds *each armed trade* to ≥0 after it covers cost. It does **not** stop
death-by-a-thousand-cuts: on a downtrending coin every up-jump reverses, so 20–140
false-breakout entries each take a bounded pre-BE loss (−420/−620bp) that **sum
net-negative**. Floor = per-trade catastrophe cap; it is not an edge. ETH/AAVE/GRT wired
because they PASSED the gate (net-positive, WF both halves, 2× cost); ONDO fails all of it.

## The distinction that keeps the operator's ask honest
An **always-armed det cell** (what `_pj_cells` is) fires on EVERY historical up-jump →
bleeds here. A **one-shot discretionary floored long** on a single live surge is a
different object: one bounded-risk position, risk = the pre-BE stop, not a statistical
edge. The always-armed cell is the thing this NO-GO rejects. (Operator did not elect the
one-shot path either this session — skip.)

## Disposition
- No ChimeraCrypto code change. Nothing wired. No deploy.
- Harness change kept: ONDO added to `upjump2pct_be_bt.cpp` COINS[] (NC 19→20) so ONDO is
  sweepable in future runs. ONDO 1h data committed.
- Non-gate-valid note: ONDO Binance history is 15 months, single regime — a statistical
  engine cannot gate it regardless; this NO-GO is about the in-sample bleed, not only the
  short history.
