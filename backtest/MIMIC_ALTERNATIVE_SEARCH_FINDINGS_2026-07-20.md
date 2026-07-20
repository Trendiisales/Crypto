# MIMIC ALTERNATIVE-DESIGN SEARCH — FINDINGS — S-2026-07-20q

Operator order (handoff 20p): **"if be cascade is not working find me an alternative that works"**
— search for ANY mimic design that passes at the HONEST basis (own-fill booking, no
entry-basis artifact). Result: **NONE FOUND. Every alternative certifies dead.**

Harness: `cascade_increment_bt.cpp` (extended: `UM_SINGLE`, `UM_E0`) — drives the REAL
`MimicLadderCompanion` @ ChimeraCrypto HEAD, `confirm_anchor_epx=false` → le = OWN FILL →
booking honest by construction (no transform). Gate everywhere: omit-2022, net>0, PF≥1.3,
both WF halves>0, base (RT=30bp) AND 2× cost (RT=60bp).

## Experiments (all fleet-wide: 35 coins × thr{0.5,1.0,1.5,2.0}%)

| # | Design | Drive | Cells | PASS |
|---|--------|-------|------:|-----:|
| 1 | **Single-leg mimic** (`UM_SINGLE=1`, mimic_stagger=false → engine's own plain mimic_floor = ONE managed T1/event; lc{30,60} × W{2,4,8,12} × g{0.2,0.5,0.75,0.9}) | OHLC tick path (fair own-fill) | 4,480 | **0** |
| 2 | **e0-EXACT close-confirm BECASC** (`UM_E0=1`, live design: stagger_mode=1 BE-cascade be_bp=20, uniform confirm60, legs 8; confirm only at close, le = REAL close fill; lc{30,60} × W{1,2,4,8,12} × g{0.2,0.5,0.75,0.9}) | stop_check_only(low) + observe(close) | 5,600 | **0** |
| 3 | **Wide-g shallow cascade** (legs{2,3} × g{0.75,0.9} × inc{38,60,100} × lc{30,60} × W{2,4,8,12}) | OHLC tick path | 13,440 | **0** |
| — | (prior, 20p) increment cascade legs8 | OHLC tick path | 10,080 | 1 marginal |
| — | (prior, c944c79) LIVE anchored BECASC at honest re-base | anchored e1 | 380 live | 8 (7 DOGE + 1 RUNE) |

Raw outputs: `single_leg_2026-07-20/`, `e0_exact_2026-07-20/`, `wideg_cascade_2026-07-20/`
(one file per coin×thr; runner scripts committed alongside).

## Key kills

1. **e0-exact 0/5600 — the 747/840 e0-honest number is HOLLOW.** The honest_entry_basis
   e0 result (747/840, flagged UPPER BOUND in 20p) assumed fill exactly at the confirm
   level via re-base. At the REAL close fill (le=close, anchor=0 — exactly what a reverted
   close-confirm engine would book), close-confirm BECASC fails everywhere. Reverting
   intrabar confirm (option 2a) buys NOTHING — the engine-side real-fill-px record +
   intrabar-confirm revert is now pointless for rescue purposes. Tails also honest here:
   worst cells −1148..−1213bp (gap-through below close fills; DOGE crash bars).
2. **Single-leg doesn't fix churn — churn is per-event confirm crossing, not cascade
   multiplication.** PREBE cuts remain 55–70% of clips (each −(lc+RT)); nPREBE identical
   across g (churn is pre-arm, giveback never touches it). Best cell fleet-wide:
   RUNE thr1.5 lc30 W4 g0.75 +121% base PF1.40 → −106% at 2× cost.
3. **Every near-miss is H1-carried.** All base-gate-passing cells (12 single-leg, 12
   wide-g) have H2 ≈ +1..+47 vs H1 hundreds — the edge is the 2021-era bull leg, decayed
   since. BNB thr2.0 lc30 W8 (best family, 2×net > 0) re-run at RT=60: net +4%, PF 1.00,
   H2 −252% → honest fail, not a gate artifact.

## Conclusion for the operator

The mimic/BECASC design space is exhausted at the honest basis:
- fine-bar mimic churn (confirm crossings ×lc cut) costs more than the trend tail pays, at
  ANY leg count (1, 2, 3, 8), ANY confirm cadence (intrabar, close), ANY g (0.2–0.9);
- the ONLY cells alive anywhere are the **8 certified survivors of the live anchored
  design (7 DOGE + 1 RUNE, wide-g tail lanes** — `HONEST_ENTRY_BASIS_RECERT_2026-07-20.md`).

**Decision needed (operator):**
- (a) build the allowlist mechanism in LiveMimicMirror (substr pause can't express
  8 cells) and re-arm ONLY the DOGE/RUNE survivors; or
- (b) retire the BECASC family outright (mirror stays paused → decommission; shadow
  `emit_clip_` epx-basis ledgers retired with it).
Mirror stays PAUSED (josgp1 `7bb43cb`) until the verdict.
