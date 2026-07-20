# INCREMENT-GATED CASCADE — operator proposal cert — S-2026-07-20

## Proposal (operator, verbatim intent)
"If the cost per BE is 38bp then surely we can arm the next mimic once we have attained the
38bp level and so on and then keep incrementing until the trade reverses."
→ Each new cascade leg arms only after the run has advanced +inc bp past the prior arm level,
so every increment is pre-funded; churn cost bounded per increment.

## Exact spec tested (honest by construction — no booking transform needed)
Real `MimicLadderCompanion` @ ChimeraCrypto HEAD, escalating per-tier confirm ladder
(60, 60+inc, 60+2·inc, … legs8) — the engine's own S-2026-07-16 stagger design ("each opens
at a DIFFERENT price and floors at its OWN fill"). `confirm_anchor_epx=false` ⇒ `le` = own
fill ⇒ floor, pre-arm cut AND booking all run from the real fill. Pre-arm cut `loss_cut_bp`
∈ {30,60} at fill·(1−lc) books PREBE_CUT (mirror o2-band parity); floor arms at fill+RT,
giveback g trail above. reclip OFF. RT=30/60bp, omit-2022 gate (net>0, PF≥1.3, both WF
halves, base AND 2× cost).

Drive: canonical OHLC tick path (up bar O→L→H→C, down bar O→H→L→C, ≤15bp/tick) — fills land
at the confirm level as live ticks do. NOTE the anchored certs' "open@high, then low"
worse-of is a mechanism-killing bias under own-fill basis (charges every fill the pre-fill
low); first runs under it showed 96% PREBE at exactly −90bp = drive artifact, not strategy.

Matrix: 35 coins × thr{0.5,1.0,1.5,2.0}% × lc{30,60} × inc{38,60,100}bp × W{2,4,8,12} ×
g{0.2,0.5,0.75} × base/2× cost = **10,080 gated cells**. Raw: `cascade_inc_2026-07-20/`.

## Result: 1/10,080 PASS
DOGE thr1.5 lc30 inc38 W8 g0.5 — n=6313, net +3373%, PF 2.31, but H2 = **+17%** (all edge in
H1) and 2× +1617%. A scrape, not a cell. Everything else fails; best non-DOGE cells PF≤1.27.
ETH representative: net −400..−1100%, PF 0.55–0.98 across the whole grid; PREBE clips = 55–70%
of all clips everywhere.

## Why the arithmetic doesn't close (the load-bearing findings)
1. **A failed increment costs −(lc+RT) = 60–90bp net, not 38bp.** The −38bp from the honest
   re-cert was the MEAN of floor clips (winners included) — not the price of a failed attempt.
   Setting lc tight enough to cap the cost at 38bp just multiplies the attempt count.
2. **Attaining +inc banks nothing.** The prior legs only monetize the increment if they EXIT
   there; while the cascade holds, one reversal takes back g×run from EVERY trailed open leg
   AND kills the newest leg at full cut — one reversal ≈ N correlated losses vs ONE increment
   of new ground. Funding is ~1:N against, not 1:1.
3. **Oscillation churn:** every detector window that wobbles around an arm level re-fires
   arms; the ladder pays the cut each time (PREBE sum −1400..−6400%/cell).
4. Consistent with the honest entry-basis re-cert: only fat-tail coins (DOGE/RUNE) come
   close — the tail is the only thing that ever covered honest churn.

## Not tested (design DOF, note for completeness)
The live o2 BE-point exit (exit at `le·(1+RT)` break, clamped ≥ fill−2RT) under own-fill le
would exit at ~net-0 instead of −(lc+RT) but at far higher frequency + gap tails — a
different pre-arm exit model. Nothing in the churn arithmetic above suggests it flips the
verdict (frequency ↑ offsets cost ↓), but it has not been certified. Ask before building.

## Verdict
Increment-gated arming does NOT rescue the cascade at the honest basis. The problem is not
WHEN legs arm — it is that every failed arm costs real bp and reversals hit all open legs
together. Mirror stays paused. Recommendation unchanged from
`HONEST_ENTRY_BASIS_RECERT_2026-07-20.md`: fix the shadow to book from the real fill
(own-fill basis) engine-side, then re-cert the close-confirm (e0) fleet — that combination
is the only branch that showed broad honest viability (747/840, upper bound).

## Files
- `cascade_increment_bt.cpp` — harness (drives real engine, own-fill honest basis)
- `cascade_inc_2026-07-20/` — 140 raw outputs + `run_all.sh`
