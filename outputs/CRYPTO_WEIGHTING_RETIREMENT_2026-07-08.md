# CRYPTO COMPANION WEIGHTING + AUTO-RETIREMENT + IMPROVEMENT SWEEP — 2026-07-08

Session scope: the 8 LIVE UpJump ladder companion books (ChimeraCrypto `7878eac`
NO-FLOOR roster: per-coin W/thr detector + roster tiers + stacked arms +2/+4/+6% g50
+ self-funding ladder cap 8, reclip 5%, RT 20bp). Code committed in `/Users/jo/Crypto`
+ `/Users/jo/ChimeraCrypto` (mac trunk). **NOTHING deployed — operator session deploys.**

Books judged **STANDALONE** (never vs riding WIDE — CompanionDominanceError).
No BE-floors, no DMA gates anywhere. Long-only, jump-triggered, regime-agnostic.

## Fidelity chain (before any verdict)

1. **Harness = the live engine.** `backtest/upjump_weighting_bt.cpp` drives the REAL
   `ChimeraCrypto/include/core/UpJumpLadderCompanion.hpp` (validate_ladder.cpp pattern);
   the parametric SimBook used for sweep variants is asserted **byte-exact vs the live
   engine on all 8 coins** (203/915/346/2459/1002/2219/786/2965 clips) on every run.
2. **Anchor:** reproduces `upjump_concurrent_arms_2026-07-07.txt` — 2tier cap5
   n=**5477 EXACT** net +10258% (pub +10283%), winner n=**10895 EXACT** net +18326%
   (pub +18360%); net delta = the extra data day's MTM on open windows.
3. **Data:** `integrity_gate.py` PASS 8/8 (+AAVE/OP), 2021-01-01 → 2026-07-08 1h.
4. **Stall-unit note:** the engine counts stall in epoch-H1 bars, the validated
   mechanism (python + validate_ladder) in BAR STEPS; identical on the gapless live
   feed, divergent across historical data gaps → harness feeds index-based ts
   (exactly like the 07-05 byte-exact validation did).

**Honest metric set** (07-08 mirror-report lessons baked in): clips stamped at their
actual bar ts (not parent-exit ts); bear-2022 attributed by episode ENTRY year;
ex-best-EPISODE (not clip); 2x-cost = full 40bp re-sim; 20-seed random-entry control
(same episode count + durations, non-overlapping).

---

## TASK 1 — Per-coin BT of the LIVE config + weighting

```
coin      n     net%     PF       H1       H2     y2022  exbestEpi  bestEpi%   2xcost  maxDDbp   all6
BTC     203     +668   2.06     +195     +473     -52/2        +22       97%     +627    15882   FAIL (y2022; 97% one episode)
ETH     915     +791   1.28     +327     +464     +41/24      +294       63%     +591    50420   FAIL (PF 1.28 < 1.3, hair)
SOL     346    +5980   5.78    +3641    +2339    +258/4     +3593       40%    +5841    39236   PASS
DOGE   2459    +4323   1.60    +3669     +654    +192/45    +1377       68%    +3837    70601   PASS
BNB    1002    +2128   1.94    +1727     +401     -83/13    +1621       24%    +1909    26084   FAIL (y2022)
ADA    2219    +1594   1.31     +353    +1241    -268/33    +1163       27%    +1166    51233   FAIL (y2022)
TRX     786    +1086   1.58     +467     +620    -208/8        +1      100%     +937    42450   FAIL (y2022; 100% one episode)
NEAR   2965    +1756   1.21     +592    +1164    +530/68    +1287       27%    +1157    67940   FAIL (PF 1.21)

random-entry control (20 seeds):   z:  BTC 1.3  ETH 1.1  SOL 2.3  DOGE 1.1  BNB 0.3  ADA 2.0  TRX 1.6  NEAR 1.6
per-year (entry-year attribution): SOL 2021 +3582 / 2023 +2388 / 2024 -33 / 2025 -214 / 2026 0-windows
                                   DOGE 2021 +3608 / 2024 +934 / 2025 -49 / 2026 -302
```

**Weighting decision** (x2 bar = honest all-6 PASS **and** ≥2σ over random):

| coin | mult | why |
|------|------|-----|
| **SOL** | **x2** | only coin clearing both bars: all-6 PASS, PF 5.78, exbest-episode +3593, 2x-cost +5841, z=2.3. Disclosed: 2024/25 windows mildly negative (−33/−214 on a +5980 book — few big SOL jumps recently, not a bleed); monthly re-test demotes if it persists. |
| DOGE | x1 | all-6 PASS but only **1.1σ** over random (HBAR-precedent: <2σ = bull-beta risk), H2 fade (+654 vs H1 +3669), 2025/26 negative. |
| BTC, ETH, BNB, ADA, TRX, NEAR | x1 | all-6 FAILs are modest 2022 bleeds / PF-hairline, all books strongly net-positive + 2x-cost-positive → stay at baseline. |
| rank-out | **none** | rank-out rule = BT-net-negative book; none is. **ADA verified:** BT +1594% / PF 1.31 / 2x +1166. The forward −$244 (−244bp) was the `seed_open` mid-event rehydrate artifact (legs re-opened OPEN at 0.186050, backdated floor) — already root-caused 07-07, machinery fix below kills the class. |

Wired: `Config.size_mult` (stamped per clip as `"mult"`, weighted bank
`bank_bp_real_w = Σ net_bp_real × mult` persisted/rehydrated/emitted) + `rank_out`
machinery (no new windows, state preserved — for the monthly re-test to use) + boot
split line `[CLIP-WEIGHTS] x2={SOL} x1={BTC,ETH,DOGE,BNB,ADA,TRX,NEAR} rank-out={} retired={}`.
Raw real column untouched (retirement + parity stay unweighted).

## TASK 2 — Auto-retirement (Omega-side semantics ported)

- Each book restores its banked forward REAL net at boot by summing its own closed-clip
  ledger (`data/companion_trades.json`, already the rehydrate source).
- When `banked_bp_real <= retire_bp` → **retired** (one-shot latch): stops arming new
  windows (detector-enter suppressed, base-leg init suppressed, ladder spawns suppressed);
  open legs manage + flush normally; loud one-shot `[CLIP-RETIRE]` log; also checked at
  boot right after rehydrate.
- **Un-retire = deliberate operator act:** list the tag in `data/companion_unretire.flags`
  (one tag per line) and restart — logs `[CLIP-RETIRE-OVERRIDE]`; or archive the ledger
  (PnL reset, the 07-07 precedent).

**Thresholds = −2× the worst per-book drawdown episode in the validated 2021-2026 BT**
of the exact live config (peak-to-trough of the cumulative real-net curve, raw per-leg bp).
Justification: the worst validated episode is the deepest hole the mechanism dug in 5.5yr
including the 2022 bear at 20bp cost; 2× that is a level the validated edge has never
produced — reaching it means live behavior has diverged from everything validated,
so new arming stops while the operator reviews. (Same "beyond-validated-DD" basis as the
Omega-side books shipped today.)

| coin | BT maxDD (bp) | retire_bp |
|------|---------------|-----------|
| BTC  | 15,882 | −32,000 |
| ETH  | 50,420 | −101,000 |
| SOL  | 39,236 | −78,500 |
| DOGE | 70,601 | −141,000 |
| BNB  | 26,084 | −52,000 |
| ADA  | 51,233 | −102,500 |
| TRX  | 42,450 | −85,000 |
| NEAR | 67,940 | −136,000 |

Smoke-tested (scratchpad `test_retire.cpp`): mult stamping + weighted bank ✓, boot-restored
bank triggers retire + blocks arming ✓, override flag ✓, det-state round-trip ✓.

## TASK 3 — Periodic re-test automation (committed, cron NOT installed)

- `tools/run_weighting_retest.sh` — idempotent: integrity-gates every input (fail-closed),
  rebuilds the harness from source, re-runs anchor+parity / live all-6 / per-year /
  random-control / extension (AAVE, OP, any new `*_1h.csv`), writes dated
  `outputs/CRYPTO_WEIGHTING_RETEST_<date>.md`. Verified end-to-end this session
  (`outputs/CRYPTO_WEIGHTING_RETEST_2026-07-08.md`). Flags rank-out candidates
  (net ≤ 0), x2-bar drift, and maxDD growth >25% (threshold re-derivation trigger).
- `tools/install_weighting_retest_cron.sh` — idempotent installer (backup → strip own
  tag → append → install; restore = `crontab /tmp/ct.bak.<ts>`), monthly `17 2 1 * *`.
  **NOT run — operator session installs.**

## TASK 4 — Improvement sweep (same rigor; bless only plateau-robust all-6)

| lever | verdict | key numbers |
|-------|---------|-------------|
| (a) cap raise 8→10/12 | **FAIL — not wired** | Net rises monotonically (SOL +5980→+7134→+8270) but it is leverage-not-alpha: PF 5.78→5.25, net/maxDD 15.2→14.0 at cap10; DOGE *degrades* (+4323→+4194→+4143); more legs = more notional in ONE move sharing the same reversal. SOL aggression delivered via the x2 mult (doubles the *validated* shape) instead — wiring cap10 on top would stack 2.4x notional into single moves. ETH formally turns PASS at cap10 (PF 1.32) but z=1.4 + boundary-crossing PF → not blessed. |
| (b) notional ∝ jump size | **FAIL — dead** | Monotonicity test (episode net by entry-jump tercile): NOT monotone on 7/8 coins; biggest-jump tercile is often the WORST (BTC T3 −7.6 vs T1 +318.6; DOGE T3 −4.6; NEAR T3 −4.7 — big jumps = blowoff tops). Do not scale notional by jump size. |
| (c) window-extension leg on 2nd jump | **FAIL — not wired** | Extra WIDE leg on fresh mid-window re-trigger: negative on 5/8 coins (BTC −38, ETH −65, SOL −31, TRX −22, NEAR −83); DOGE +584 / BNB +85 / ADA +22. A DOGE-only wire would be per-coin cherry-picking (fresh overfit knob); flagged for the monthly re-test to watch, not blessed. |
| (d) per-coin thr retune 2024-26 | **FAIL — keep shipped** | No plateau anywhere: grids are spiky/sign-flipping between neighbors (BTC 6h/8% +115 next to 6h/10% −79; NEAR 8h/5% −538 next to 8h/10% +261). Shipped cells sometimes negative on 2024-26 (BTC −84, SOL −214, DOGE −354) but every "better" cell is an isolated spike = exactly what the plateau rule forbids. Recency softness folded into the weighting decision instead (kept DOGE at x1, disclosed on SOL). |
| (e) code leaks | **1 REAL BUG FOUND + FIXED** | see below |

### (e) Restart-path bug (the Omega-watermark class) — FOUND + FIXED

`seed_open()` seeded every det_w companion **from the live PARENT's position** — but the
parents run the UNIFORM 4h/+2% window (52c0d31) while the companions trigger on the
roster per-coin W/thr detector (e.g. TRX 8h/+8%): a **different window family** (the exact
"never conflate" violation, on the restart path). Every restart while a parent was in-pos:
- **injected a phantom window** the roster detector never opened (det_in_ forced true,
  entry = the parent's entry), whose exit then hung on a cold W-bar detector ring;
- conversely a genuine in-flight detector window was **eaten** (pending arms lost) when
  its parent happened to be flat.

Fix (ChimeraCrypto): detector state (det_in/det_entry/bar/close + H1 ring) is now
**persisted every H1 close** (`data/companion_det_state.json`, atomic tmp+rename) and
**restored verbatim at boot** (`restore_det_state()`, legs re-open FLAT through the gated
step path — rehydrate-FLAT, no backdated le); `seed_open()` **refuses det_w books**
(`[CLIP-SEED-SKIP]`) as defense-in-depth; first boot after upgrade cold-starts honestly
(no state file → `[CLIP-DETSEED] no ...` log). No double-booking found in the ledger
rehydrate (append-only, archived-on-reset) or the dual bar/tick observe drivers
(stale-bar guard + post-clip re-arm gate hold).

Also audited, clean: cost debits (model==real in ladder mode, cost applied once per clip,
2x-cost strictly worse in all 180+ cells), `-CLIP` tag collapse in totals, flush-MTM
(no abandon path).

---

## Deploy steps (operator session — NOT done here)

1. mac: push both repos (committed this session): `/Users/jo/Crypto` (harness + tools +
   reports) and `/Users/jo/ChimeraCrypto` → `origin/main`.
2. Box (chimera-direct, josgp1 checkout): fetch + **ff-only** merge to origin/main,
   rebuild (`cmake --build build` per scripts/deploy.sh §2), restart the chimera service.
   NOTE: `scripts/deploy.sh` still hard-codes `origin/xsec-deploy`, which no longer exists
   on origin (trunk is `main` since the 07-05 consolidation) — deploy manually ff-only or
   fix the script first.
3. Verify boot log: 8× `[CLIP-INIT] ... mult=x… retire@…bp NO-FLOOR shadow=1`,
   the `[CLIP-WEIGHTS] x2={SOL} x1={BTC,ETH,DOGE,BNB,ADA,TRX,NEAR} rank-out={} retired={}`
   line, and `[CLIP-DETSEED]`/`no companion_det_state.json` (first boot = honest cold-start,
   NOT `[CLIP-SEED] ... from live parent`).
4. Vault: Memory-Chimera CryptoUpJumpCompanion updated this session; stamp the verified
   running SHA after deploy.

*Session S-2026-07-08 (weighting/retirement). Committed, not deployed, no ssh, no crontab edits.*
