# IBKRCrypto (working name)

Reuse of the **ChimeraCrypto** engine, re-pointed to **IBKR** for crypto signal +
execution, trading **CME Spot-Quoted Futures (SQF)** — CME's perp-like product
(spot-quoted, daily TFA financing, CFTC-regulated, centrally cleared).

> **Status: SCAFFOLD / SHADOW.** Architecturally drop-in; **no trading edge proven.**
> Do not size. Gated by BACKTEST_TRUTH.

## What is reused vs new

| layer | source | reuse |
|---|---|---|
| Signal engines (285) | `ChimeraCrypto include/core/EdgeEngine.hpp` | **unchanged** — `on_tick(price,ts)` is asset-agnostic |
| Funding harvest | `ChimeraCrypto MultiSymbolFundingFilter` | reuse; repoint Binance funding → CME daily **TFA** |
| Risk | `ChimeraCrypto CapitalControlLayer` (funding-aware) | unchanged |
| IBKR connectivity | `Omega/omega/include/omega/IbkrClient.h` + `Omega/third_party/twsapi` | **reuse Omega's proven client** |
| **Feed (new)** | `include/IbkrCryptoFeed.hpp` | mirrors `chimera::BinanceWSFeed` → emits `MarketTick` |
| **Executor (new)** | `include/CmeSqfExecutor.hpp` | mirrors `chimera::SpotExecutor::execute` → CME SQF order |
| **Contracts (new)** | `include/CmeSqfContracts.hpp` | symbol → IB `FUT`/`CME` contract + size (BTC 0.01, ETH 0.20) |

## The seam
Chimera live loop: `feed.set_callback(t → engine.on_tick(mid,ts))` and
`executor.execute(sym,is_buy,qty,px)`. Both are swapped 1:1:
`BinanceWSFeed → IbkrCryptoFeed`, `SpotExecutor → CmeSqfExecutor`. Engines untouched.

## Build flag
`IBKRCRYPTO_WITH_IBKR` (mirrors Omega's `OMEGA_WITH_IBKR`): off = SHADOW/replay
(compiles, no TWS); on = live IB Gateway data + `placeOrder`. SHADOW default.

## Viability
- **Architecture: VIABLE** — clean seams, asset-agnostic engines, Omega IB client exists.
- **Data: CONDITIONAL** — needs IB market-data sub for CME crypto/SQF (RT live; EOD/intraday for BT). Not entitled today.
- **Edge: UNPROVEN** — *directional* SQF inherits `chimera-bear-spot-no-edge-deadend` (spot-long bear-dead). **TFA/basis carry** is the untested, distinct thesis. No faithful BT yet → no edge claim.

## Next step (before any code goes live)
Pull CME SQF + dated-basis history via `omega::IbkrClient.fetchHistorical` →
faithful BT of TFA carry **net of daily financing + IBKR cost** → only then wire live.
