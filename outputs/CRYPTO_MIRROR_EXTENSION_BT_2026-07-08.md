# CRYPTO MIRROR + LADDER-EXTENSION BACKTESTS — 2026-07-08

RESEARCH ONLY. Nothing wired, nothing committed, no live process touched.
Three studies: (1) wave-parent x2 mirror, (2) UpJump ladder coin extension,
(3) dmove-parent x2 mirror. All books judged **STANDALONE** (never vs riding WIDE —
CompanionDominanceError). No BE-floors, no DMA gates anywhere.

**HEADLINE: all three FAIL. Nothing is wire-worthy.**

| # | Study | Verdict |
|---|-------|---------|
| 1 | WAVE-PARENT MIRROR (BTC+ETH) | **FAIL** — 0/36 grid cells pass; best cell +5.9% PF 1.05; dies at 2x cost everywhere |
| 2 | LADDER COIN EXTENSION (12 next-tier coins) | **FAIL** — 0/12 wire-worthy; 2 formal passes (XLM, HBAR) overturned by episode-concentration + bear-attribution stress |
| 3 | DMOVE-PARENT MIRROR (BTC+ETH) | **FAIL** — 0/36 cells pass; every cell net-negative or PF<1 |

---

## Data provenance (LOCAL FILES ONLY — nothing downloaded)

| Source | Files | Range | Integrity |
|---|---|---|---|
| `/Users/jo/Crypto/backtest/data/{BTC,ETH}USDT_1h.csv` | 48,326 bars each | 2021-01-01 → 2026-07-08 | repo-standard files (live book inputs) |
| `/Users/jo/Crypto/backtest/data/multiyr/*USDT_15m.csv` | 12 coins resampled 15m→1h (scratchpad `data_ext/`) | 2020-01-01 (per-coin listing date) → 2026-06-14 | `backtest/integrity_gate.py`: **12/12 clean** |

Extension coins + resampled ranges: XRP/LINK/LTC/ATOM/XLM/HBAR 2020-01-01→2026-06-14 (~56.5k 1h bars);
AVAX 2020-09-22, DOT 2020-08-18, UNI 2020-09-17, FIL 2020-10-15, INJ 2020-10-21, ICP 2021-05-11 (→2026-06-14).
All cover the 2022 bear (ICP most of it).

**Costs:** 20bp round-trip per clip (repo standard: `RT_BP=20.0` in the validated ladder sweep;
`COST_RT=0.002` in wave_companion.cpp / crypto_dmove_companion.py — Binance spot taker).
2x-cost gate = 40bp. For the ladder the 2x run is a FULL re-sim (the self-funding spawn
condition sees the doubled cost), not a column recompute.

## Validation anchors (before any verdict)

1. **GOLDEN parent parity:** python replication of the parent drifted vs `GOLDEN_UPJUMP.txt`
   (BTC 389 vs 330 at 4h/2%) → per the fixture rule, STOPPED and switched to the ONE canonical
   tool `backtest/ibkrcrypto_bt` — **10/10 coins exact GOLDEN match** (330/397/551/507/382/492/340/577/546/361).
   (Extension coins use the validated python `parent()` at roster thresholds ≥5%, where it is
   documented byte-exact vs the live C++ — the 2% drift cell is not used anywhere.)
2. **Ladder mechanism parity:** `Leg`/`run_trade` recovered VERBATIM from git `df71919`
   (`crypto_upjump_tiered_ladder_sweep.py`, byte-exact-validated vs native `UpJumpLadderCompanion`,
   5477 clips / 8 coins). Reproduction check: ETH 4h/+5%, tight a3/s0/g50 wide a8/s0/g50 rc5 cap5 →
   **n=393 net=+494% — EXACT match** to the 07-05 roster line.
3. **Cost-sign sanity:** net at 2x cost is WORSE than net at 1x in every single cell of all three
   studies (never improves) → no sign-flipped cost arithmetic.
4. **arm=1% sanity control:** in both mirror studies all 12 arm=1% cells are NET NEGATIVE
   (as every prior book predicted). In the ladder study the a1 tight-tier control never passes and
   PF degrades vs a2/a3 (note: ladder "arm" is an MFE-arm on a 50%-giveback clip, not an entry
   threshold, so a1 is not negative *by construction* there — the strict all-negative tell applies
   to the mirror books, where it held).

Pass bar (all-6, per task): net>0 after 20bp | PF≥1.3 | both WF halves (data-midpoint split) >0 |
calendar-2022 net>0 or empty-by-avoidance | net ex-best clip >0 | net>0 at 40bp.

---

## 1) WAVE-PARENT MIRROR — FAIL

Parents = the wave detector ported verbatim from `src/wave_companion.cpp`
(STEP 1% above trailing-low ref → wave; exit on 15% reversal-from-peak or stagnation
BTC 48h / ETH 24h; ref resumes at exit price). BTC: 442 rides, ETH: 875 rides (2021→2026-07).

Mirror (single leg; "x2" = two identical legs = pure linear scaling, leverage-not-alpha, so one
leg is simmed): FLAT until close-fav vs parent entry ≥ arm% → buy at that close; trail = giveback
gb% from mirror peak (close-eval); optional retrigger when close ≥ last-clip-peak × 1.02;
flush at parent ride end. Grid: arm {1(ctl),2,3}% × gb {0.5,0.75,1.0}% × retrig {off,on}.

**Result: 0/36 non-control cells pass (0/72 incl. controls).**

```
BTC WAVE  arm% gb%  rtrg |    n    net%    PF     H1      H2   bear22  exbest  2xcost
best cell  3.0 0.50   n  |  204    +5.9  1.05   +9.4    -3.5    -9.5    -1.8   -34.9  fail
           3.0 1.00   Y  |  338    +8.1  1.03  -14.9   +23.0   -19.6    -1.7   -59.5  fail
(all other cells net<=0 or fail multiple gates; arm=1 ctl: -21.7 .. -83.1, all negative)

ETH WAVE  best cell 2.0/0.75 n | 459 +11.7 1.04 +14.7 -3.0 -22.2 -11.2 -80.1  fail
(all 18 cells fail; arm=1 ctl: -26.1 .. -149.6, all negative)
```

Full grids: scratchpad run `task13_mirror.py` (t13_full.txt). Diagnosis: a 0.5–1.0% giveback
trail clips gross moves of the same order as the 20bp cost → cost eats the book; retrigger
consistently WORSENS net (reproduces the DmoveTightTrailCompanion "RETRIG kills" finding).
The only trail family that has ever survived real fills on these parents remains the
fractional-of-MFE giveback ladder (gv50), not tight absolute-% givebacks.

## 2) LADDER COIN EXTENSION — FAIL (no wire-worthy coin)

Live-book mechanism, verbatim: 2 base tiers (TIGHT + WIDE a8/s0/g50) + self-funding ladder
(cost-covered clip spawns one WIDE leg, cap 5), reclip 5%, RT 20bp. Per-coin parent W/thr picked
by the documented live rule (best-net PARENT standalone passing all-6, from the roster menu
W∈{4,6,8} × thr∈{5,8,12}%). Companion tiers FIXED at the modal live config (no per-coin tier
tuning → no fresh overfit): C3 = tight a3/s0/g50, C2 = tight a2/s0/g50, C1 = a1 control.

| coin | parent | C2 net/PF | C3 net/PF | all-6 | notes |
|------|--------|-----------|-----------|-------|-------|
| XRP  | — | — | — | FAIL | no parent W/thr passes (best 4h/+8%: +743% PF2.53 but bear22 −82) |
| LINK | 4h/+5% | +362% / 1.13 | +395% / 1.14 | FAIL | PF≪1.3, bear22 −49..−60 |
| AVAX | 6h/+5% | +236% / 1.07 | +282% / 1.08 | FAIL | PF, bear22 neg, C2 dies at 2x |
| DOT  | — | — | — | FAIL | parent stage (best: +170% PF1.34, H2 −30, bear −36) |
| LTC  | — | — | — | FAIL | parent stage (H2 neg / bear neg) |
| ATOM | — | — | — | FAIL | parent stage (H2 neg) |
| UNI  | 4h/+8% | −158% / 0.93 | −261% / 0.88 | FAIL | companion net-negative outright |
| XLM  | 4h/+12% | +1583% / 2.29 (fail H1) | **+1727% / 2.43 formal PASS** | **OVERTURNED** | see stress below |
| FIL  | — | — | — | FAIL | parent stage (bear −10..−108) |
| ICP  | — | — | — | FAIL | parent stage (net negative) |
| INJ  | 4h/+8% | +623% / 1.21 | +666% / 1.22 | FAIL | PF < 1.3, bear22 −144..−154 |
| HBAR | 4h/+12% | +622% / 1.49 (fail H2) | **+967% / 1.76 formal PASS** | **OVERTURNED** | see stress below |

**Why the two formal passes are overturned (the important part):**

- **Episode concentration** (the reclip ladder banks many clips per parent episode, so the
  ex-best-CLIP gate is weak; stressed ex-best-EPISODE instead):
  - XLM C3: **108% of the whole book is ONE episode** (entered 2024-08-05, 10 clips, +1864%).
    Net EX-BEST-EPISODE = **−137% (NEGATIVE)**. Yearly: 2020 +465, **2021 −368, 2022 −75**,
    2024 +1727, **2025 −3, 2026 −19**. A one-rally book, negative in 4 of 6 years.
  - HBAR C3: 74% of book = one episode (2020-01-04, +711%); ex-best-episode +256% survives, BUT —
- **Bear-2022 "empty-by-avoidance" was an attribution artifact:** the validated harness stamps
  every clip at the parent-EXIT bar ts, so 2022-entered episodes that exit later escape the 2022
  window. By episode ENTRY year: **HBAR 2022 = −173%, XLM 2022 = −75%** — both books actually
  BLED in the bear. Honest bear gate: FAIL for both.
- **Random-entry control (bull-beta guard, 20 seeds, same episode count+durations, non-overlap):**
  - XLM: actual +1727 vs random mean +347 (sd 437) → +1380 over-random (~3σ) — but moot given the
    one-episode dependence.
  - HBAR: actual +967 vs random mean **+575 (sd 808)** → +392 over-random = **0.5σ, insignificant**.
    An HBAR book of random entries with the same holding pattern makes most of the money → bull-beta,
    not edge.

Verdict: **the next liquidity tier adds nothing.** 6/12 coins die at the parent stage, 4 die on
PF/bear, and the 2 formal passes are single-rally bull-beta. Consistent with the vault record —
the live 8-coin roster was already the survivor set of a wider hunt.

## 3) DMOVE-PARENT MIRROR — FAIL

Parents = the dmove companion's hosts, ported from `Omega/backtest/crypto_dmove_companion.py`
`gen_parents` (ungated, per operator 2026-07-03 spec): LONG when close − close-24h-ago ≥ $100
(BTC) / $15 (ETH); ride until an equal $ down-jump. BTC: 1593 hosts, ETH: 1106 hosts.
(At 2025-26 prices $100/$15 ≈ 8bp — noise-level triggers, hosts are near-continuous; that is the
spec'd parent, tested as specified.) Same mirror grid as study 1.

**Result: 0/36 cells pass. Every single cell is net-NEGATIVE (best: BTC 3.0/0.75 retrig −8.5%
PF 0.96; ETH 3.0/0.75 no-retrig −3.5% PF 0.98). arm=1% controls: −37.7 .. −93.2, all negative.**
2x cost roughly doubles the bleed everywhere (sign sane). The %-arm/tight-giveback mirror on
$-move parents reproduces the DmoveTightTrailCompanion tombstone: tight trails on these hosts
are cost-dominated; retrig makes it worse.

---

## Full grids / raw evidence

- Study 1+3 full 72-cell grid: `task13_mirror.py` output (t13_full.txt, scratchpad
  `/private/tmp/claude-501/-Users-jo-Omega/4788ad1b-eefb-447e-8166-4d57aa3c6888/scratchpad/`)
- Study 2 full output incl. per-coin parent sweeps + controls: `task2_ladder_ext.py` (t2_full.txt)
- Episode stress: `t2_episode_stress.py`
- Harness sources kept in the same scratchpad (verbatim mechanism from git `df71919`).

## Sim-artifact notes (things that smelled, and what was done)

1. **Python parent drift at thr=2%** (389 vs GOLDEN 330 on BTC): the plain `c[i]/c[i-4]-1 ≥ 2%`
   replication over-fires vs the canonical C++ UpJump2. Not chased — canonical binary used for
   GOLDEN work; extension coins use thresholds ≥5% where python parity is documented byte-exact.
2. **Clip timestamp = parent-exit ts** in the validated ladder harness (kept verbatim for parity)
   makes window-based regime slices leak: it manufactured the XLM/HBAR "no 2022 clips" mirage.
   Corrected by episode-entry-year attribution in the stress step (changed the verdict).
3. **Ex-best-clip is the wrong single-event guard for reclip ladders** — one rally = many clips.
   Ex-best-EPISODE is the honest version and killed XLM.
4. No cost-sign anomaly anywhere (2x cost strictly worse in all 180 sim cells).

*Session S-2026-07-08, research sandbox. No commits, no deploys, no cron edits.*
