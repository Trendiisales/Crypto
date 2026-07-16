# REAL-PARENT mimic_floor re-gate — ETH/LDO + 5 REGIME — 2026-07-16

Option-b mandate (handoff 2026-07-16): re-test the floored-mimic proposal against the
**REAL parent engine windows** (not the crude ±thr up-jump proxy the first pass used),
single floored leg, at **measured per-coin cost**. Judge STANDALONE (feedback-companion-
independent-engine). This is the "floored-g sweep that needs a parent-replay harness"
owed since the mimic_floor migration (main.cpp make_be_mimic comment).

## Harness
`Crypto/backtest/real_parent_mimic_bt.cpp` — drives the **REAL deployed engines**:
- REGIME (ADA/DOT/NEAR/SUSHI/THETA): `chimera::EdgeEngine` REGIME_SWITCH on D1
  (1h→D1 agg), make_regime config incl. **3-ATR stop + hold_bars=12** (the crude proxy
  AND the earlier `regime_mimicg_bt.cpp` reimpl both omit these → looser windows; this
  harness is the faithful one). Parent state read via `in_position()/entry_px()`.
- ETH: `UpJumpLadderCompanion` jump_floor cell **PJ7W24** (W=24 thr=7% s=400bp g=1.0),
  1h. State via new read-only `jf_in_position()/jf_entry_px()`.
- LDO: `CryptoCampaignManager` **LDO-CAMP-W8** (CW8-7.0), 1h. State via new read-only
  `campaign_open()/campaign_entry_px()`.

The mimic = independent `UpJumpLadderCompanion` mimic_floor, driven bar-by-bar off the
parent's settled `(in_pos, entry_px)` — EXACTLY the live wiring (main.cpp:1479).

**Faithfulness reconciliation** (feedback-verify-kill-replicates-mechanism): recovered the
Phase-2 harness `companion_be_mimic_bt.cpp` (git 057573a) and reproduced its headline
**ADA cap=2-ladder PF 36.85 / net +416%** exactly on certified data → the EdgeEngine
parent path is sound. The mimic_floor result differs from the old ladder because of the
**engine (cap=1 floor vs cap=2 ladder)** and, decisively, the **pre-arm loss_cut**.

## Cost (measured, not hand-quoted — feedback-crypto-cost-authoritative-depth-model)
`depth_cost_model_bt`: ETH safe_cost = **28.0bp** at ≤$100k (depth-slip ≈0). The other 6
have no bookdepth snapshots → they take the system's own `CryptoCostLedger::safe_cost_bps`
floor = fees20+spread2+latency5+dust1 = **28bp**. Gate base = **28bp**, stress leg =
**2× = 56bp** (the thin-coin depth-slip bound). Stricter than the deployed 20/40.

## ROOT-CAUSE FINDING — the deployed `loss_cut_bp = 60` is net-negative poison
The deployed cells ship `loss_cut_bp=60` (g=1.0), labelled a "Phase-2 protection verdict".
That verdict was measured on the **OLD cap=2 ladder** (DOT 1.70→2.43, ADA 21.2→36.9) and
**never re-gated after the mimic_floor migration**. On mimic_floor it churns like an
entry-stop (g-sweep finding #1) and makes **all 5 cells net-negative** at measured cost:

| deployed (g=1.0, lc=60, RT=28) | ADA | DOT | NEAR | SUSHI | THETA |
|---|---|---|---|---|---|
| net% | −18 | −18 | −4 | −20 | +1 |

Removing the cut (lc=0) flips ADA +153 / THETA +138 / SUSHI +57 at the same g=1.0.
**These 5 cells are LIVE and shadow-bleeding as configured.**

## Standalone gate (net>0 & PF≥1.3 & WF-H1>0 & WF-H2>0, base 28bp AND 2×=56bp)
A **uniform wide `loss_cut_bp=800`** bounds the pre-arm tail to −828bp (−8.3%) WITHOUT the
lc=60 churn (a tight cut ≤60 churns; a wide cut only catches genuine disasters):

| cell | kind | g | net% | PF | worst_bp | WF-H1/H2 | 2×[net,PF] | verdict |
|---|---|---|---|---|---|---|---|---|
| ADA   | REGIME D1   | 1.00 | +153 | 5.89 | −828 | +3/+150  | +151, 5.56 | **WIRE** (keep g) |
| SUSHI | REGIME D1   | 0.30 | +93  | 2.87 | −828 | +29/+64  | +89, 2.73  | **WIRE** |
| THETA | REGIME D1   | 0.50 | +157 | 7.34 | −828 | +140/+18 | +154, 6.98 | **WIRE** |
| DOT   | REGIME D1   | any  | ≤+20 | <1.35| — | **WF-H1 always neg** | fail | **RETIRE** |
| NEAR  | REGIME D1   | 0.05*| +58  | 2.30 | −1682| +54/+3   | +57, 2.26  | **RETIRE** (fragile) |
| ETH   | jump_floor  | any  | ≤+42 | ≤1.58| — | **WF-H1 always neg** | ≤+8 breakeven | **NO-GO** |
| LDO   | campaign    | 0.75 | +50  | 1.48 | −840 | −4/+55   | +27, 1.22 (<1.3) | **NO-GO** |

\* NEAR passes ONLY at g=0.05 with lc=0; any cut ≥300 makes it net-negative → its "edge"
is entirely tail-recovery on a single knife-edge g = overfit/fragile (15 clips). RETIRE.

- **ETH**: net+ at base only; **WF-H1 negative at every g** (edge concentrated in H2) and
  2× barely breakeven (PF≤1.08). Not walk-forward robust, cost-fragile. The first pass's
  "+36% standalone PASS" read only base-net, ignoring the WF + 2× legs the standalone gate
  requires. **NO-GO — confirmed against the real jump_floor parent, mechanism-verified**
  (losscut=0 doesn't rescue it; the −1008/−1329bp tails just grow).
- **LDO**: WF-H1 negative + 2× PF<1.3. The genuinely-weak one (handoff already flagged).

## g-shape (contradicts the SWEET-cell finding, as expected for regime windows)
For the REGIME cells a **lower g helps** (THETA needs g≤0.75; SUSHI best ~0.30) — regime
spans are chop/mean-reverting, a tighter trail banks the continuation before it reverses.
The SWEET self-detect up-jump cells wanted HIGH g (momentum). ADA is the exception (robust
g0.75–1.0 AND g≤0.25). Per-cell g is signal, not noise (monotone in g for THETA/SUSHI).

## Recommendation
1. `make_be_mimic`: `loss_cut_bp` 60 → **800**; add per-cell `g` (was hardcoded 1.0).
2. `_regime_basket`: keep **ADA (g1.0) / SUSHI (g0.30) / THETA (g0.50)**; **remove DOT +
   NEAR** (net-negative / fragile).
3. Do **not** wire ETH or LDO mimics.

Sample caution: 14–21 clips/cell. Verdicts rest on the ROBUST facts (lc=60 net-negative
across all cells; ADA/SUSHI pass across broad g×lc; DOT/ETH WF-H1 negative everywhere),
not knife-edge single-point passes. lc=800 chosen uniform (not per-cell tuned) to limit
overfit; it bounds the tail and all three pass.
