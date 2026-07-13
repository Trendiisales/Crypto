# CRYPTO CAMPAIGN PARENT+MIMIC — PER-COIN LEVERS TABLE (2026-07-13)

Operator directive SESSION_HANDOFF_2026-07-13j §2.13 + Part 4 task 1. NEW `campaign` mode in
`upjump_earlyarm_bt.cpp` (parent lot + funded mimic sim). Data: Binance H1 2023–26 (2022 excluded,
standing rule). Costs: RT 20bp + 3bp slip + 2bp reserve in funding equation; acceptance re-sims at
30bp + 40bp (full re-sim, §2.12). Stops tested on bar LOW, exits at stop px (pessimistic).

## Mechanism (as simmed)
- PARENT: enters on the wired SWEET cell's up-jump window (W/thr) at CONFIRMED +20bp from window
  entry (validated entry class — no immediate entry). ONE parent per window. Geometry scales with
  stop S: fee-BE arm at MFE 0.9×S (stop → entry+RT+3bp); net-lock at 1.8×S (locks 0.4×S net);
  HWM trail (ptrail bp) active from 2.0×S. trail=0 ⇒ ride-to-reversal (window exit), floors on.
- MIMIC (25% of parent): opens ONLY when funded — parent locked net ≥ 0.25×(mstop+RT+slip+reserve)
  — AND parent MFE ≥ mact AND fresh continuation (pullback ≥ reset then new campaign high).
  Fast ladder: fee-BE 1.7×mstop, lock 3.1×mstop (0.6×mstop net), HWM trail from 3.75×mstop.
  Max 3 sequential, one at a time, flush with parent. Never mimics a losing parent (funding gate).

## Gates applied (§2.12): PF≥1.30@20bp, net>0@30bp, H1>0 AND H2>0, ex-best-episode>0,
## plateau (±neighbors), 1-bar delayed-entry survival. Mimic judged STANDALONE.

## PASS — campaign parent cells (4)

| cell | entry | pstop | ptrail | n | net@20 | PF | H1/H2 | medWin | @30 | @40 | exBest | delay1 | verdict |
|---|---|---|---|---|---|---|---|---|---|---|---|---|---|
| UNI-W1 | W1 +3.5% conf20 | 135 | 270 | 57 | +74% | 2.53 | +30/+43 | 96bp | +69 | +65 | +54 | +62 | **PASS** (162/270 = +107/PF3.25 also on plateau) |
| UNI-W2 | W2 +4.0% conf20 | 216 | 270 | 87 | +156% | 2.65 | +82/+74 | 397bp | +144 | +136 | +135 | +104 | **PASS strong** |
| TRX-W8 | W8 +3.5% conf20 | 111 | 0 (ride) | 35 | +92% | 4.88 | +14/+77 | 44bp | +77 | +75 | **+11** | +78 | **MARGINAL** — best episode = 88% of profit; small size / shadow only |
| LDO-W8 | W8 +7.0% conf20 | 411 | 342 | 77 | +68% | 1.39 | +29/+39 | 553bp | +61 | +55 | +39 | +91 | **PASS** (trail plateau 274–411 all positive) |

All levers in bp. Derived thresholds per cell (geometry): BE arm = 0.9×pstop; net-lock at 1.8×pstop
(locks 0.4×pstop net); trail active from 2.0×pstop; reset = 0.35×medTR
(UNI 38, TRX 13, LDO 48). Med H1 TR 2023+: UNI 108, TRX 37, LDO 137bp.

## FAIL — campaign parent adds nothing (6 cells)

| cell | best parent row | why |
|---|---|---|
| BNB-W1 4.0% | n=7 | too thin to validate (7 windows since 2023) |
| NEAR-W1 4.0% | +37% PF1.84 | H1 −20 (all positive rows fail one half) |
| AAVE-W3 4.5% | +61% PF2.06 | H2 −23 (every row H2-negative) |
| ADA-W8 3.5% | +72% PF1.31 | H2 −24; 30bp/40bp collapse on most rows |
| DOGE-W1 5.5% | n=15 all ≤ +2% | dead |
| LINK-W8 4.5% | +18% PF1.13 | PF < 1.30 everywhere |

For these six the EXISTING SWEET BE-cascade mimic books remain the instruments — unchanged, they
already passed the full 13j stack. Campaign parent is NOT a replacement anywhere; it is a separate
additional book class (companion-independence rule: judged standalone, additive).

## MIMIC LOT VERDICT (honest): no robust standalone edge on H1

Best cells: LDO mstop48/mtrail103/mact904 → +10%/PF2.08 n=18; TRX mstop18/mtrail28 → +2%/PF1.35
n=18; UNI ≤ +7% marginal. NONE survive 1-bar delayed entry (LDO +10→−12, TRX +2→−1). The funded
structure works MECHANICALLY as specced — combined worst clip = parent worst clip in every cell
(mimic can never take the campaign negative; funding equation held) — but the mimic's own book is
noise at H1 granularity. Spec's 12–20bp mimic stops assume tick/microstructure management; H1 alt
bars are 37–137bp. **Wire mimic OFF (parent-only campaigns); revisit mimic at tick granularity
when the CryptoCampaignManager exists (§2.11 build).**

## Portfolio note (§2.9)
UNI W1+W2 = ~59% of 4-cell aggregate profit → correlated-alt cap applies (max 2 correlated
campaigns; UNI cells share the symbol — ONE campaign per symbol rule means W1/W2 signals FUSE into
one UNI campaign in the live build, not two positions).

## Not run
BTC/ETH/SOL/XRP/XLM/GRT/OP/BCH/AVAX/LTC: excluded with stated reason — 13j absolute-truth verdict
(dead on every window 2023–26, random-entry z≤0.9); no entry anchor exists to campaign on.

Repro: `CP_MIMIC=0 CP_W=2 CP_THR=0.040 CP_PSTOP=216 CP_PTRAIL=270 CP_RESET=38 ./upjump_earlyarm_bt campaign UNI`
