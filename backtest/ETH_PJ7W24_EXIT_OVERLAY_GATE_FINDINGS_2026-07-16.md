# ETH PJ7W24 wide/patient exit overlay (#2) — GATE FAIL, do NOT wire (2026-07-16)

**Verdict: DO NOT WIRE the wide/patient exit overlay onto ETH PJ7W24. ETH keeps the live
ride-to-flip exit (#1).** The `UpMoveTrailLossMitigation` frontier (trail5/stop3/ts48/ck3) is a
BASKET + Donchian-entry edge; it does **NOT transfer** to the single-name ETH +7%/24h jump parent.
The gate killed it before any live change — harness only, zero risk taken.

Harness: `eth_pj7w24_exit_overlay_bt.cpp` (A/B on the ONE real contract's exit — permitted overlay,
NOT a separate clip). BASELINE = the REAL deployed `UpJumpLadderCompanion` PJ7W24 jump_floor cell
(g=1.0 ride-to-flip, pre-BE 400bp) driven bar-by-bar (parity by construction, no re-impl). OVERLAY
brakes only ever exit earlier / skip → applied by walking the real price path from each real entry.
ETH 1h 2021→2026 (48,425 bars). MEASURED cost 28bp base / 56bp 2× (`CryptoCostLedger` ETH depth floor).

## Headline (base cost 28bp)
| arm | n | net% | PF | worst_bp | 2024 | 2025 bull | verdict |
|-----|---|------|----|----|------|-----------|---------|
| **BASELINE ride-to-flip** | 214 | **+78** | 1.28 | −824 | +14 | **+109** | ref |
| OVL-A (brakes on ride) | 208 | −38 | 0.85 | −729 | +24 | −11 | FAIL |
| OVL-B (5ATR trail frontier) | 208 | −41 | 0.83 | −729 | +24 | −11 | FAIL |

**GATE base=FAIL 2x=FAIL.** Overlay collapses the bull (2025 +109 → −11) — the exact fat-tail
amputation the frontier doc itself warns of ("BE/tight-stop flips bull +1160% → −1015%").

## Mechanism attribution (isolated each lever, base cost)
| lever | net% | PF | 2025 bull | read |
|-------|------|----|-----------|------|
| BASELINE | +78 | 1.28 | +109 | — |
| **time-stop 48b only** | **−14** | 0.95 | −13 | ☠️ **THE POISON** — caps the ride, amputates runners |
| time-stop 240b (10d) | −48 | 0.82 | −4 | ☠️ even patient-er fails — ETH jump rides run >>10d |
| hard-stop 3ATR only | +61 | 1.23 | +108 | mild net drag, keeps tail, catches disaster (worst −824→−729) |
| **ck3 entry-filter only** | **+86** | 1.32 | +114 | ✅ only base-positive lever (skips 6 low-ATR marginals) |

**The time-stop is the kill mechanism** — ETH PJ7W24 rides run far past 48h (even a 240-bar/10-day
cap fails); any time-cap chops the fat-tail winners that pay for every loser. Confirmed on the REAL
parent, `feedback-verify-kill-replicates-mechanism` satisfied (the kill is the mechanism, not a proxy).

## Why the one base-positive lever (ck3) still NO-GOs
ck3 entry-filter alone: **+86% base (PF 1.32)** — marginally better than baseline +78, tail intact
(it's an entry filter, never touches runners). BUT at **2× cost** the gate (ATR ≥ 3×cost) rightly
tightens → skips 131/204 entries, survivors **−88%**. ck3 = **base-only positive, fails the mandatory
2×-cost leg** — identical failure signature to the falsified mimic. NO-GO.
(At 2× cost the whole ETH up-jump edge is already marginal: baseline +52, worst −840. Any overlay
makes it worse.)

## Bottom line / what to do
- **ETH keeps the live PJ7W24 ride-to-flip (#1).** It is the best ETH profit-keeper in the data
  (+78% PF1.28, both WF halves ≥0 at base). Do not add trade-level brakes.
- The frontier's basket win came from the Donchian-entry + basket combination; the ts48 that helped
  the basket is exactly what kills single-name ETH jump rides.
- Re-open bar: a genuinely NEW mechanism or cost structure — NOT another param grid on these levers.
- Optional (operator call, marginal): ck3 entry-filter is base-positive and tail-safe, but since it
  fails 2× cost it does not clear the standalone gate → not recommended.

Spec: `ETH_PJ7W24_PATIENT_EXIT_OVERLAY_SPEC_2026-07-16.md` (§5 gate → this FAIL). Harness +
findings committed. No live change made.
