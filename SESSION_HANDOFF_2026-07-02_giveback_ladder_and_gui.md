# SESSION HANDOFF — 2026-07-02 — Giveback-ladder fill + reclip + GUI fix (crypto + Omega companions)

NZ time written: 02-07-2026 22.33. Prior session ran out of context budget mid-plan (hard-stop
handoff rule). NOTHING was built/edited/committed this session — diagnosis only. Fresh session
picks up the BUILD queue below.

---

## OPERATOR ASK (verbatim intent)

From the IBKRCrypto GUI (:8090) screenshot the operator wanted:
1. Understand why the crypto book is underwater + whether protections work / are in place.
2. Answer the "pending" item ("should not be pending if everything is working").
3. Look into the reclipping + companion engines.
4. Giveback ladder: "we start at 30% on crypto, ensure we have **every 10**, same as the Omega
   system, and also the retri[g]/reclip engines." → build giveback-fraction rungs **every 10% from
   30%** (30/40/50/60/70/80/90) on BOTH systems, plus reclip variants.
5. Then: "**do all of them** but ensure we dont go over session. I want the GUI fixed — look at
   how we rebuild those engines or fix them, **test every lever/setting**."

So the fresh session must: build the ladder fill (both systems), reclip variants, AND fix the GUI —
each companion rung **backtested STANDALONE all-6 before any cron** (mandate).

---

## DIAGNOSIS ALREADY DONE (do not re-derive)

### Q1 — why underwater (daily book −$182, screenshot −$165.6)
By design, NOT a fault. Every open leg is a SHORT (ETH/BTC/SOL) except NDX-long. BTC/ETH/SOL
ticked UP intraday (BTC 61041 vs entry ~60260, ETH 1643 vs 1613, SOL 79.71 vs 75.16) → shorts red.
Legs are short because [[PerpTrendRegimeGate]] holds trend legs short while BTC < 200DMA (bear
regime). Book has **no hard stop by design** ([[UpMoveTrailLossMitigation]]: stops/BE destroy the
trend edge). Protection = vol-target sizing + exit-on-turn + low-correlation downsize. Underwater
open leg = normal state of a stopless trend book. −2.5% of $7,360 deployed = noise.

### Q2 — protections: GREEN except one stale display
- `protection_selftest.py` GREEN 22:15 (all 6 pass). Regime gate wired ([[PerpTrendRegimeGate]] +
  [[MacroGateAltDecoupling]]). Daily companion clip running (`/tmp/companion_clip.log`: open 4,
  realized $71.01 — 100% ENGINE_EXIT, ZERO giveback clips → arm=10 is inert, see Q4 catch).
- **REAL GAP:** the intraday book shows `companion bank +$86.92` but that state file
  (`/Users/jo/Crypto/companion_intraday/companion_state.json`) is **FROZEN at 30-Jun 15:08**. The
  intraday-companion cron is **DISABLED** (crontab line ~20, commented S-2026-06-30 — correct: BT
  showed it LOSES, WF-H2 −82% / bear −185% / 1h book −328%, [[IntradayMRCostWall]]). So the
  +$86.92 is a **dead relic displayed as live** — a dead protection reading as alive. THIS is the
  "protection not in place" the operator sensed. GUI fix required (Q item 4 below).

### Q3 — "pending" = external IBKR, NOT a bug
"SQF pending" / "QSOL pending" is a **hardcoded display label** (`backtest/refresh_shadow.py:22-35`,
`refresh_shadow_intraday.py:49-59`), not a failing probe. Means IBKR instrument permission pending
([[CryptoLiveCutover4002]], probed 02-07):
- QTF(BTC)/QEF(ETH) SQF → `err201 closing-only` (awaiting 2nd IBKR grant).
- SOL → doubly blocked: QSOL contract **not listed** on venue yet + SQF closing-only.
- Only QNDX(NDX) SQF opens; spot Paxos eligible (long-only/IOC).
"Pending" is honest — upstream of us. Won't clear until IBKR lifts closing-only + lists QSOL.

---

## BUILD QUEUE (all 4 — operator said do all)

### LOAD-BEARING GOTCHA (read first — [[GivebackLadderSweep]])
The **ARM gate (STALL_GATE_PCT), not the giveback fraction, decides if a rung does anything.** At
arm=10% favorable excursion, 0/92 index trades ever arm → clip INERT → every giveback rung
collapses to the identical "hold-to-engine-exit" book. Crypto daily companion currently runs
**arm=10** → its gv50 never giveback-clips (log confirms). **Do NOT spray gv30/40/60/70 crons at
arm=10 — that creates inert DUPLICATE books (protection theatre).** Must arm-scale to the
instrument's median peak excursion so the fraction bites. Omega index arm=2, MGC/GoldPanic arm=1.

### MANDATE (hard, both systems — CLAUDE.md + memory feedback-companion-independent-engine)
Companion = SEPARATE independent additive book. Judge **STANDALONE all-6** (net>0, PF>1, both WF
halves>0, both regimes≥0). **NEVER compare clip vs WIDE** (dominance error, operator furious on
recurrence). No unbacktested clip ships. Each wired rung cites its BT figures in the cron comment.

### Item 1 — Omega ladder fill: gv30, gv70, gv90
- Currently wired: gv40/50/60/80 (SPX turtle, arm 2). **Missing 30, 70, 90.**
- gv30 ALREADY tested PASS in [[GivebackLadderSweep]] (SPX arm2: net+56 PF1.88 MAR6.27 all-6-PASS) —
  just never wired. gv70/gv90 need a sweep run.
- Harness: `Omega/backtest/clip_giveback_ladder.py` + emitters `clip_path_idx_turtle.cpp` (SPX/DJ30
  D1) / `clip_path_mgc_fastdon.cpp` (XAU M30). Path CSVs already exist: `/tmp/idx_spx.csv` (92),
  `/tmp/idx_dj30.csv` (85), `/tmp/mgc_fastdon_path.csv` (356). Overlay math system-of-record:
  `Crypto/backtest/standalone_clip_overlay.py`.
- Wire passing rungs as new cron lines (COMPANION_DIR=..._gvNN, SKIP_CRYPTO=1, PAPER-only,
  COMPANION_PUSH_STATE=0) mirroring the gv40/60/80 lines in `stall-accountant/_new_clip_cron.txt`.

### Item 2 — Crypto ladder 30–90, ARM-SCALED
- Currently: gv50 only, arm=10 (inert). Build 30/40/50/60/70/80/90.
- FIRST sweep crypto trade **excursion distribution** to pick the arm where the fraction
  differentiates (crypto trends move far more than indices — likely arm ~3-5%, but MEASURE it;
  don't guess). Harness: `crypto_companion_trigger_bt.py`, `crypto_companion_lever_sweep.py`,
  `crypto_companion_coldcut_sweep.py`, `crypto_companion_regime_gate_bt.py` +
  `standalone_clip_overlay.py`. Trend legs harvested: EMAx ETH/BTC/SOL + Roc20 BTC (NDX TSMom50
  EXCLUDED — [[NdxCompanionClip]]: clip destroys the NDX trend runner).
- CAUTION vault says crypto trend clip LOSES to WIDE at the arms tested so far → the arm-scaled
  sweep must SHOW an all-6-PASS before wiring, else report "no viable crypto rung" and wire nothing.
- Cron template: line ~25 in crontab (`COMPANION_DIR=/Users/jo/Crypto/companion_clip ...
  SKIP_OMEGA=1 STALL_GATE_PCT=<scaled> REVERSAL_GIVEBACK=0.NN ...`), one dir per rung, PAPER-only.

### Item 3 — reclip / RE_TRIG variants
- `RE_TRIG_PCT` in `stall_accountant.py:~214`, code default 0.05, cron default 0. Harness:
  `Omega/backtest/clip_reclip_sweep.py` (committed 8e9fdc8b; multi-leg clip walker, faithful to
  single-clip at retrig=0). [[ReclipRetrigSweep]]: mildly additive, never hurts, best retrig 2-5%,
  ALL levels PASS all-6. Add COMPANION_RETRIG_PCT=0.02–0.05 on the passing rungs from Items 1-2.
  Note D1 hosts: retrig widens DD (SPX/DJ30 fine; XauTFD1 retrig0 better risk-adj).

### Item 4 — GUI FIX (operator: "I want the GUI fixed... test every lever/setting")
Problem: intraday book displays frozen dead-companion bank +$86.92 as if live (Q2). Options:
- **Fix display**: `Crypto/gui/server.py` (`/api/state_intraday`) + `refresh_shadow_intraday.py` —
  detect the companion state is stale (updated age > threshold, same [[GUIStalenessGuard]] pattern)
  and render the intraday companion bank as `DISABLED`/blank + badge, so a dead protection cannot
  read as alive. Mirror the existing `_freshness()`/`_fresh` staleness layer already in server.py.
- **OR rebuild/retire**: the intraday companion was killed by BT (WF-H2 −82%) — decide with operator
  whether to (a) permanently retire + remove its GUI panel, or (b) rebuild it with a viable
  config found by re-sweeping `crypto_companion_intraday_bt.py`. Default recommendation: retire the
  display (it's a proven-losing engine), keep the panel only if a re-sweep finds an all-6 config.
- "Test every lever/setting": run the full lever sweep (`crypto_companion_lever_sweep.py`) across
  arm × giveback × retrig × cold-cut so the GUI reflects only backtested-live protections.

---

## KEY PATHS
- Crypto repo: `/Users/jo/Crypto` (companion_clip/, companion_intraday/, gui/, backtest/, config/).
- Omega companions: `/Users/jo/stall-accountant/` (per-host clip dirs + `_new_clip_cron.txt`).
- Shared engine: `/Users/jo/stall-accountant/stall_accountant.py` (both systems call it; SKIP_CRYPTO
  / SKIP_OMEGA select side).
- Crontab: `crontab -l` — crypto lines 2-25, Omega clip ladder lines 28-42.
- Vault (Chimera): `/Users/jo/Memory-Chimera` — file ANY new rung/finding here (entity + index.md +
  log.md, NZ time). Omega companion findings → `/Users/jo/Memory-Omega`.
- Logs: `/tmp/companion_clip.log`, `/tmp/companion_intraday.log` (frozen), `/tmp/protection_selftest.log`.

## VAULT ENTITIES TO READ
[[GivebackLadderSweep]], [[ReclipRetrigSweep]], [[CompanionDominanceError]], [[NdxCompanionClip]],
[[IndexTurtleCompanionClip]], [[GUIStalenessGuard]], [[CryptoLiveCutover4002]],
[[PerpTrendRegimeGate]], [[IntradayMRCostWall]], [[UpMoveTrailLossMitigation]].

## DO-NOT
- Do NOT wire any clip cron without an all-6 STANDALONE BT verdict in its comment.
- Do NOT compare companion clip vs WIDE (dominance error — operator furious).
- Do NOT wire giveback rungs at an arm where they're inert (duplicate books).
- Do NOT touch tombstoned/disabled engines beyond the GUI-display fix without asking.
- On next context-low warning: STOP + handoff (this rule).
