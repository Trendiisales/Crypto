# ALT-FLEET LOW-THRESHOLD mimic — BTC 18ab/ac + ETH 18ag rollout applied to the other 33 roster coins — S-2026-07-18ai

## Ask (operator, 2026-07-18)
"rollout the same low thresholds for the other cryptos after you check which it will be
viable for" — the BTC (S-18ab/ac) + ETH (S-18ag) low-thr quad rollout applied to the rest
of the live companion roster.

## Method (identical to BTC/ETH cert)
Harness `eth_ujmimic_15_becascade_bt` rebuilt vs ChimeraCrypto HEAD `f159b94` (honest
S-17f fills; drives the REAL UpJumpLadderCompanion). Per coin `UM_COIN=<C>`, confirm60
BE-ENTRY anchored (= 2× the 30bp min cost, ≥ 2× the fleet's measured 28bp safe_cost_bps),
legs8, RT=30/60bp (stricter than measured 28 — feedback-crypto-cost-authoritative).
Data: `<COIN>USDT_1h.csv` refreshed to 2026-07-18 latest complete hour via
`append_1h_tails.py` (APPEND-ONLY direct-klines tail — the fetch_coin_1h clobber trap
avoided by construction; all 33 coins +8..192 rows, 0 gap-steps, 0 continuity aborts).

Coins (33 = live roster minus BTC/ETH, never-half-the-symbols):
AAVE ADA APT ATOM AVAX BCH BNB COMP CRV DOGE DOT ETC FIL GRT ICP INJ LDO LINK LTC MANA
NEAR OP RUNE SAND SOL SUI SUSHI THETA TIA TRX UNI VET XRP

Grid per coin: thr {0.5,0.75,1.0,1.25,1.5,2.0}% × W {1,2,4,8,12,24} × g {0.2,0.5,0.75},
legs8 — 108 cells/coin, each at base AND 2× cost.

## Result: ALL 33 coins × 108 cells PASS the full standalone gate — zero fails fleet-wide
Gate = net>0 ∧ PF≥1.3 ∧ WF-H1>0 ∧ WF-H2>0 at base 30bp AND 2×=60bp, OMIT-2022.
Margin floor across all 3,564 cell-runs (nowhere near the gate):
- min PF **3.51** (ATOM thr2.0 W1 g0.75)
- min WF-half **+479%** (ETC thr2.0 W1 g0.75)
- min 2×-cost net **+854%** (ATOM thr2.0 W1 g0.75)
Same deep plateau as BTC/ETH. **VIABLE = all 33 coins.** Newer-listing coins (APT/SUI/TIA
etc., data from 2022-2023) gate on their full listed history; 2021/2022 columns print +0
where unlisted, gate unaffected (omit-2022 anyway).

Full raw outputs: `lowthr_sweep_2026-07-18/<COIN>.txt`; picks table `picks.tsv`.

## Rollout scope — LOW thresholds only (operator's ask), 8 cells/coin
Per coin: UJ05 (0.5%) W{1,2,4,12} + UJ10 (1.0%) W{1,2,4,12} = 264 new cells.
UJ15 W-lane fill-ins NOT rolled to alts (each alt already runs UJ15 F/S at W2/W8;
the UJ15 W-lane extension stays majors-only — keeps the fleet at 423, not 500+).
Picks per W lane = best 2×-cost net; g0.20 preferred when within 5% of lane max
(PF 2–3× higher, sumNeg ~half — BTC/ETH precedent). retire_bp = 2× own backtested worst.

## Honest tail framing (S-17f discipline)
nNeg > 0 on every cell; the floor REDUCES the pre-BE tail, it does not eliminate it.
Never restate as nNeg=0.

## Wiring (S-2026-07-18ai, ChimeraCrypto `_bc_alt_lowthr_cells` block)
`src/main.cpp` after the ETH block, own feed vector `_bc_alt_lt_feeds` (reserve-safe),
factory `make_becascade_cell`: confirm60 anchored, mimic_floor, BE_CASCADE stagger,
reclip 0, cap8, catchup 24h. `_grid.reserve` 220→520 (under-reserve = silent
pointer-invalidation UB). 159→423 grid cells. Mac build green, ctest 10/10 PASS.
All SHADOW companion books (paper), additive, judged STANDALONE
(companion-independent-engine).
