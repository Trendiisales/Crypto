# Handoff — ETH UpJump companion cadence + giveback sweep + Omega deploy — S-2026-07-05

Caveman session. Context-budget HARD-STOP fired; stopped per `feedback-context-low-handoff-warning`.

## DONE this session (do NOT redo)
1. **Omega VPS deploy `5d7c0453` — LIVE + verified.** HEAD 341d8870→5d7c04534; `Git hash confirmed
   5d7c045==running`; `[GPB][SEED] 1485 H1 bars` present; seed gate clean; `deploy COMPLETE`.
   (GoldPanicBounce intra-bar, shadow.) Omega vault-on-deploy for GPB still owed if not filed.

2. **ETH "not firing / waiting on 1h" — DIAGNOSED, not a fault.**
   - Live C++ `UpJumpCompanionEngine` on josgp1 (chimera-direct = LIVE; `reference-chimera-boxes-alias-trap`).
     State `data/crypto_companion_state.json` fresh every ~60s. ETH armed, peak 3.86%, clips=1, bank 151bp.
   - Python `upjump_eth` mac mirror = **DISABLED today (revert-pydup)** — redundant w/ C++ + violates
     `feedback-no-python-working-system`. Its frozen 10:14 state ≠ fault.
   - **Reversal clip = per-tick / intra-bar** in the running binary (call site main.cpp ~7534, `mid`,`now_ms`).
     Only STALL is H1 (6 bars = 6h, by design). `.hpp` docstring "once per H1 bar" is STALE.
   - Only ETH clip ever = STALL_CLIP 07-04 02:00 (pre-22:28-boot, rehydrated). No clip since boot =
     no 50% reversal trip + no 6h stall. Correct per config (arm2/stall6/rev0.50/reclip0.05).

3. **SEPARATION CONFIRMED (operator's core rule).** Per-tick observe is **read-only** on the parent:
   `observe(bool,double,double,int64)` takes parent state BY VALUE (no handle). Zero `par->`/`.open`/`.close`
   inside the companion. Call site only reads `par->in_position()`/`entry_px()` (const). The up-jump engine
   trades byte-identically; only the companion's SAMPLING RATE changed. Fully compliant with
   [[CompanionDominanceError]] / `feedback-companion-independent-engine`.

4. **Faithful giveback sweep (operator asked "as little giveback as possible").**
   - Harness: `Crypto/backtest/eth_upjump_clip_sweep.py`. Parent from `ibkrcrypto_bt --dump-trades UpJump8`
     (added env-gated `--dump-trades` mode to `Crypto/src/ibkrcrypto_bt.cpp`, harmless). Parent fidelity
     gate: **n=65, net+391%, matches FULL row**. Overlay replicates `observe()` line-for-line, per-leg P&L
     ("banks each leg"), cost 0.20% RT, STANDALONE all-6. Fidelity corroborated: arm2 R50=+192, R70=+225
     brackets vault +211.
   - **VERDICT: tighter giveback = WORSE on ETH.** Reversal peak at LOOSE R60-70; <R45 fails all-6 (net-neg
     bull). Stall: longer better (S8+75→S6+44→S1−82). "Zero giveback" turns companion net-negative. ETH
     up-jump = trend-rider; giveback is the price of the ride (same as 2026-06-17 swing-protect lesson).
   - Profit-max direction = reversal-only, loose, STALL OFF (+192 vs live S6 +32). Operator NOT adopting.

5. **Operator decision: "commit intra-bar fix only."** Committed `a509d76` on josgp1 branch **`xsec-deploy`**
   (companion-side, 37 lines, NO engine code). Running binary (built 22:28) already had it. **PUSH REJECTED**
   (xsec-deploy behind origin) — needs careful `git pull --rebase` on the live box before push. Commit is
   safe LOCALLY on josgp1.

## OPEN ACTIONS (next session)
1. **Push `a509d76`**: on josgp1 `cd ChimeraCrypto; git pull --rebase origin xsec-deploy` (CAUTION: messy
   working tree — deleted klines CSVs, M gui/config/CMakeLists; do NOT bundle them) then `git push`.
   OR leave local if the rebase is risky — confirm with operator.
2. **Operator wants 3-5 companions open concurrently.** This is GATED by parent up-jump positions, not a
   companion setting. Only ETH's parent is in a trade now (others not jumping). Options to raise count:
   (a) market-driven (wait for more coins to up-jump), or (b) LOOSEN parent up-jump thresholds — a
   PARENT-ENGINE change needing its own faithful backtest (separate from companion). Operator's framing
   "if this works we should be only making profit" → set expectation: companion is additive per armed
   parent; count follows parent activity.
3. Omega incidental bugs from prior handoff still open (CLAUDE.md ConnorsRSI2 line; US500.F multiplier;
   g_nas_turtle_d1 routing) — see `outputs/SESSION_HANDOFF_2026-07-05b-cadence-deploy.md`.
4. File Memory-Chimera vault: update `[[CryptoUpJumpCompanion]]` — the "CADENCE FIX commit 995ac7e" claim is
   WRONG (was uncommitted; now `a509d76`). Add the giveback-sweep verdict (tighter=worse, R50 near-optimal).

## RULES IN PLAY
`feedback-companion-independent-engine` (separation PROVEN), `feedback-verify-kill-replicates-mechanism`
(overlay replicates observe() exactly, parent n=65 validated), BACKTEST_TRUTH, `feedback-no-python-working-system`,
`feedback-context-low-handoff-warning` (why this doc exists), `reference-chimera-boxes-alias-trap`.
