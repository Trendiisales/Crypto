# ETH LOW-THRESHOLD mimic — BTC 18ab/ac rollout applied to ETH — S-2026-07-18ag

## Ask (operator, 2026-07-18)
"roll the same setting out for eth now" — the BTC low-thr quad rollout (S-18ab UJ05 +
S-18ac UJ15/UJ10, `BTC_LOWTHR_MIMIC_FINDINGS_2026-07-18.md`) applied to ETH.

## Method (identical to BTC cert)
Harness `eth_ujmimic_15_becascade_bt` rebuilt vs ChimeraCrypto HEAD `01a30d2` (honest S-17f
fills). UM_COIN=ETH, confirm60 BE-ENTRY anchored (= 2× the operator 30bp min cost),
legs8, RT=30/60bp (stricter than ETH's measured ~28bp — feedback-crypto-cost-authoritative).
Data: ETHUSDT_1h 2021-01-01→2026-07-18 (48,569 rows; 143-bar fresh tail appended via direct
Binance klines — NOT fetch_coin_1h.py, which clobbers pre-2023 history). Integrity gate
CERTIFIED CLEAN.

**Baseline reproduction FIRST:** at cert params (thr1.5/RT28/confirm60/anchor) the harness
reproduces the wired ETH F/S figures — worst −997.5bp (F, W2/g0.20) and −1086.1bp
(S, W8/g0.75) to the digit; n/net drift (+8 trades on F) = the fresh 6-day tail only. Faithful.

Grid: thr {0.5,0.75,1.0,1.25,1.5,2.0}% × W {1,2,4,8,12,24} × g {0.2,0.5,0.75}, legs8.

## Result: ALL 108 cells PASS the full standalone gate
Gate = net>0 ∧ PF≥1.3 ∧ WF-H1>0 ∧ WF-H2>0 at base 30bp AND 2×=60bp, OMIT-2022. Zero fails —
same deep plateau as BTC. Net rises as thr drops (more triggers, same floored mechanics);
n saturates below ~0.5% (confirm60 binds), so 0.5% is the practical floor.

## Picks — 11 new ETH cells (best 2×-cost net per W lane; g0.20 preferred when within 5%
of lane max: PF 2–3× higher, sumNeg ~half — the BTC "g0.20 beat g0.75 on PF/tail at
near-equal net" precedent). UJ15 W2/W8 lanes NOT duplicated (live ETH-UJ15-BECASC-F/S
already cover them).

| cell | thr | W | g | n | net% | PF | worst_bp | 2×net% | retire (2×worst) |
|---|---|---|---|---|---|---|---|---|---|
| ETH-UJ05-BECASC-W1  | 0.5% | 1  | 0.20 | 7671 | +10097 | 14.75 | −966.2  | +7816 | −1932 |
| ETH-UJ05-BECASC-W2  | 0.5% | 2  | 0.20 | 7828 | +10974 | 17.14 | −999.5  | +8647 | −1999 |
| ETH-UJ05-BECASC-W4  | 0.5% | 4  | 0.20 | 7937 | +11577 | 18.14 | −883.8  | +9214 | −1768 |
| ETH-UJ05-BECASC-W12 | 0.5% | 12 | 0.75 | 5895 | +9030  | 7.35  | −1077.0 | +7341 | −2154 |
| ETH-UJ10-BECASC-W1  | 1.0% | 1  | 0.20 | 5142 | +7479  | 16.49 | −966.2  | +5950 | −1932 |
| ETH-UJ10-BECASC-W2  | 1.0% | 2  | 0.20 | 6115 | +9266  | 17.32 | −999.5  | +7449 | −1999 |
| ETH-UJ10-BECASC-W4  | 1.0% | 4  | 0.20 | 6352 | +9467  | 17.07 | −883.8  | +7560 | −1768 |
| ETH-UJ10-BECASC-W12 | 1.0% | 12 | 0.75 | 4782 | +8153  | 7.80  | −1077.0 | +6511 | −2154 |
| ETH-UJ15-BECASC-W1  | 1.5% | 1  | 0.20 | 3042 | +4849  | 15.83 | −966.2  | +3942 | −1932 |
| ETH-UJ15-BECASC-W4  | 1.5% | 4  | 0.20 | 4779 | +7190  | 17.58 | −813.9  | +5771 | −1628 |
| ETH-UJ15-BECASC-W12 | 1.5% | 12 | 0.20 | 4022 | +5931  | 19.14 | −883.8  | +4719 | −1768 |

W12 lanes: g0.75 materially better (g0.20 at 94.6%/90.0% of max) → g0.75 kept there.

## Honest tail framing (S-17f discipline)
nNeg > 0 on every cell (e.g. UJ05-W4: 1149 neg clips, sumNeg −675.5%, worst −883.8bp).
The floor REDUCES the tail, it does not eliminate it. Never restate as nNeg=0.

## Wiring (shipped S-2026-07-18ag, ChimeraCrypto `48383b5`)
`src/main.cpp` `_bc_eth_lowthr_cells` block after the BTC block, own feed vector
`_bc_eth_lt_feeds` (reserve-safe). Factory `make_becascade_cell`: confirm60 anchored,
mimic_floor, BE_CASCADE stagger, reclip 0, cap8, catchup 24h. 144→155 companions
(`_grid.reserve(220)` OK). Mac build green, 10/10 suites PASS. Deployed josgp1 same
session: boot `RUNTIME MODE = LIVE`, `[MIMIC-FLOOR-GATE] 155/155 floored 0 VIOLATION`,
`[PROFIT-LOCK-GATE] 157 locked 0 VIOLATION`, `build=48383b5` == repo HEAD.
ETH total: UJ2 parent-cascade + UJ15 F/S + 11 low-thr = 14 books. All SHADOW companion
books (paper), additive, judged STANDALONE (companion-independent-engine).
