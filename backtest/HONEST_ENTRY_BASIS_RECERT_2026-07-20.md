# HONEST ENTRY-BASIS RE-CERT — BECASC fleet — S-2026-07-20 (operator-ordered)

## Order
Handoff 2026-07-20o: "run the honest-basis re-cert now" — re-certify all BECASC cells with
ENTRY AT THE CONFIRM FILL (`le*(1+confirm)`, confirm=60bp), worse-of fills, measured cost —
NOT the anchored `le=epx` shadow booking basis. PASS ⇒ cell's mirror may re-arm; FAIL stays
paused (acquire pause shipped S-20u `7bb43cb`).

## Method
`honest_entry_basis_bt.cpp` (this dir) — byte-copy of the S-20j `early_confirm_bt.cpp`
(840/840 booked-basis cert) driving the REAL live `MimicLadderCompanion` @ ChimeraCrypto
HEAD, with ONE change: every ClipRecord is re-based in the callback to entry at the confirm
fill. Exact transform (no approximation):

    cost         = gross_bp_real − net_bp_real          (engine's own debit, unchanged)
    honest_gross = ((1 + gross_bp_real/1e4)/(1 + 0.006) − 1)·1e4
    honest_net   = honest_gross − cost

Exits unchanged (the mirror exits when the shadow clips — same exit px); only the entry
basis shifts. Validated against S-20j raw output: ETH thr0.5 W1 g0.2 e1 delta =
60.2bp/clip = exactly the confirm shift.

Spec: confirm60 BE-ENTRY anchored, legs8, RT=30/60bp (stricter than measured 28),
worse-of fills (e1: confirm fill at bar HIGH, same-bar low vs fresh leg), omit-2022 gate
(net>0, PF≥1.3, both WF halves>0, base AND 2× cost).

Coverage — FULL fleet, never half the symbols:
- Ordered S-20j matrix: 35 coins × thr{0.5,1.0}% × W{1,2,4,12} × g{0.2,0.5,0.75} × e{0,1}.
- Supplementary live-spec: 35 coins × thr{1.5,2.0}% × W{1,2,4,8,12} × g{0.2,0.5,0.75} e1 —
  covers every live BECASC cell config (base W4/g0.5, FAST W2, SLOW W8/W12, BTC/ETH/alt
  lowthr quads). 380 live BECASC cells extracted from main.cpp and joined 1:1; 0 unmapped.

Raw outputs: `honest_basis_2026-07-20/<COIN>_thr<thr>_e{0,1}.txt` + runner scripts.

## Result — the booked edge does not survive the honest entry basis (live mode)

**e1 (intrabar confirm = LIVE behavior): 8/840 matrix cells PASS (S-20j booked: 840/840).**
**Live-cell verdict: 372/380 live BECASC cells FAIL. 8 survive:**

| survivor | config | honest net (base/2×) | PF | H1/H2 |
|---|---|---|---|---|
| DOGE-MIM05-BECASC-W4  | thr0.5 W4 g0.75 | +7252% / +3904% | 1.73 | +5761/+1490 |
| DOGE-MIM05-BECASC-W12 (g0.5 lane also passes) | thr0.5 W12 g0.75 | +9668% / +6985% | 2.38 | +8460/+1208 |
| DOGE-MIM10-BECASC-W2  | thr1.0 W2 g0.75 | +6419% / +3367% | 1.68 | +5372/+1047 |
| DOGE-MIM10-BECASC-W4  | thr1.0 W4 g0.75 | +6827% / +4041% | 1.76 | +5701/+1126 |
| DOGE-MIM10-BECASC-W12 | thr1.0 W12 g0.5+0.75 | +8921% / +6714% | 2.47 | +8103/+817 |
| DOGE-MIM15-BECASC-S   | thr1.5 W8 g0.75 | +7638% / +5513% | 2.19 | +6286/+1351 |
| RUNE-MIM05-BECASC-W4  | thr0.5 W4 g0.75 | +4853% / +2602% | 1.86 | +3073/+1779 |
| RUNE-MIM15-BECASC-F   | thr1.5 W2 g0.75 | +3060% / +1708% | 1.72 | +2056/+1003 |

All survivors are wide-giveback (g≥0.5) big-tail cells — the handoff prediction exactly
("floor-churn cells flip red; cells carried by big-run tail may pass"). Survivors are
top-heavy (H2 ≪ H1) but pass the full gate incl. 2× cost.

Character of the kill: honest mean clip ≈ booked − 60bp; the FLOOR_TRAIL churn population
(booked ≈ +0 by anchor construction) goes ≈ −38..−60bp/clip and floods every tight-g cell.
LIVE CONFIRMS the model: 13 real BE-exits avg −36bp/leg, zero positive (handoff 20o item 2).

## Second finding — intrabar confirm is the churn source, close-confirm mostly survives

**e0 (close-confirm, pre-S-20j behavior): 747/840 PASS at the honest basis.**
The S-20j intrabar-confirm change ("cannot hurt" — certified 840/840 at the BOOKED basis)
adds thousands of fade entries per cell whose honest economics are −60bp churn; at the
honest basis it is the difference between a mostly-viable fleet and a dead one.
e0-PASS → e1-fail at honest basis: massive (vs ZERO at the booked basis — the S-20j cert
conclusion was an artifact of the entry-basis error).

**CAVEAT (honesty):** the e0 honest numbers are an UPPER bound. A close-confirm real fill
lands at the crossing CLOSE, which can overshoot `le*(1+confirm)`; the engine does not
record the actual fill px (`ClipRecord.entry_px = le` by design, header L30), so e0 was
re-based to the confirm level like e1. A faithful close-fill re-cert needs an engine-side
fill-px record first. Do NOT re-arm anything off the e0 column without that re-cert.

## Verdict / recommendation (operator decides)
1. **Mirror stays PAUSED** (`acquire_pause_substr="BECASC"`, 7bb43cb) — 372/380 cells fail
   the ordered honest gate. No re-arm shipped this session.
2. The 8 survivors are re-armable per the order, but a substr pause cannot express an
   8-cell allowlist — re-arming them needs a small allowlist mechanism in LiveMimicMirror.
   Given 8/380 and top-heavy halves, recommend leaving paused pending operator call.
3. Structural options, in order of expected value:
   a. Record the actual confirm fill px in the engine + book shadow clips from it
      (fixes MimicShadowEntryBasisError at the source, makes shadow=real by construction).
   b. Re-cert close-confirm (e0) at the true close-fill basis; if it holds, reverting the
      S-20j intrabar confirm turns most of the fleet honest-viable again.
   c. Raise confirm above 60bp (wider BE margin absorbs the basis gap) — needs its own sweep.
4. Shadow BECASC ledgers keep booking from epx until (a) ships — treat every shadow BECASC
   figure as overstated by ≈ confirm until then (feedback-shadow-entry-basis-honesty).

## Files
- `honest_entry_basis_bt.cpp` — harness (this dir)
- `honest_basis_2026-07-20/` — 210 raw outputs + `run_all.sh` + `run_live_thr.sh`
- Live-cell join: 380 cells ← main.cpp BECASC tables, 0 unmapped
