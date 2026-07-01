# IBKRCrypto faithful BT — first pass (2026-06-24)

Harness: `ibkrcrypto_bt.cpp` (CRTP + struct-of-arrays). Daily bars 2021-2026.
Faithful: decide@close → enter@next-open, round-trip cost 6bps + 1bp half-spread×2,
daily TFA financing (10%/yr carry modelled; real Binance funding overlaid where it
exists ~2025-26). PERP UNLOCK = LONG **and** SHORT enabled (old book was spot-only).

## Edge shortlist (both-regime, PF>1.1, real trade count)
| rank | instrument | strategy (L/S) | FULL PF | 2022-bear PF | note |
|---|---|---|---|---|---|
| 1 | SOL | IBS mean-rev | 1.25 (n491) | 1.26 | every window PF 1.06-1.38 — most robust |
| 2 | BTC | TSMom50 trend | 1.50 (n116) | 2.06 | best bear PF anywhere (shorts) |
| 3 | ETH | TSMom50 trend | 1.82 (n98) | 1.13 | strong, bull-tilted |
| - | SOL | TSMom20 trend | 2.21 (n206) | 1.10 | bull-beta, bear-weak — NOT clean |
| dead | XRP | all | noisy | — | 2024 IBS -97%, TSMom 2023 -114% |
| unproven | all | Funding carry | — | — | only 1yr funding data; weak/neg, thin n |

## The perp unlock (long+short vs old long-only) — 2022 bear
| instrument | TSMom20 LONG-ONLY bear | TSMom20 LONG+SHORT bear |
|---|---|---|
| BTC | PF0.25 (-65%) | PF0.77 |
| SOL | PF0.06 (-90%) | PF1.10 |
| ETH | PF0.49 (-41%) | (TSMom50 1.13) |
Shorts rescue the bear regime the spot-only book had no answer for
(chimera-bear-spot-no-edge-deadend). That is the concrete unlock.

## Caveats (NOT a deploy gate)
- SQF-on-SPOT proxy (SQF trade at spot; correct underlying). Live SQF fill model + real CME TFA still to add.
- Raw signal edges, unlevered % — maxDD 60-230% → need vol-target/sizing.
- Daily bars; no param-sweep/cross-val yet → in-sample risk; some FULL PF fat-tail-driven (SOL 2021).
- Funding-carry data too short to judge.
