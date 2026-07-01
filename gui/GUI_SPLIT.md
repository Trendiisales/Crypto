# GUI split — one IBKR login, two dashboards

Operator requirement: **same IBKR gateway**, but **separate GUIs** — one for Omega,
one for Crypto. Clean because the crypto engine is already a separate fleet process.

```
                    ┌─────────────────────────────┐
                    │   IB Gateway (ONE login)     │  port 4001 live / 4002 paper
                    └──────────────┬──────────────┘
            clientId 11/86/87 ◄────┤────► clientId 88
                    │                          │
        ┌───────────▼──────────┐   ┌───────────▼───────────┐
        │  Omega.exe + ibkr    │   │  IbkrCryptoEngine     │
        │  fleet (CFD/equity)  │   │  (CME SQF crypto)     │
        └───────────┬──────────┘   └───────────┬───────────┘
                    │ writes                     │ writes
        Omega telemetry/ledger        data/ibkrcrypto/state.json + daily_ledger.csv
                    │                            │
        ┌───────────▼──────────┐   ┌───────────▼───────────┐
        │  OMEGA GUI           │   │  CRYPTO GUI           │
        │  (OmegaApiServer /   │   │  gui/index.html       │
        │   telemetry, exist.) │   │  gui/server.py :8090  │
        └──────────────────────┘   └───────────────────────┘
```

## What splits
- **Data feed**: shared (one gateway). No duplication — both consume the same IB session via distinct clientIds (IB allows many clients per login; Omega already runs ~4).
- **State/output**: SEPARATE. Crypto engine writes its OWN `state.json` + `daily_ledger.csv` under `data/ibkrcrypto/`. Omega writes its own. Never mixed.
- **GUI**: SEPARATE. Omega keeps its existing dashboard. Crypto gets its own on **:8090** (`gui/server.py` serving `gui/index.html`, polling `/api/state`).

## Run (scaffold)
```
python3 gui/server.py          # crypto dashboard -> http://127.0.0.1:8090
```
Reads `IBKRCRYPTO_STATE` (default `backtest/data/ibkrcrypto/state.json`). The
IbkrCryptoEngine refreshes that JSON on every decision (`write_state_()`).

## Production
Embed the HTTP server in `IbkrCryptoEngine.cpp` (like ChimeraCrypto's QuadEngine owns
:8080) so the crypto book is fully self-contained: one binary = engine + its GUI on :8090.
Omega's GUI is untouched. Account-level risk still shared via `KILL_SWITCH.lock` +
an account daily-loss watcher across the fleet (per-process RiskManagers don't see each other).
