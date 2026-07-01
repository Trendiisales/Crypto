# IBKRCrypto — Paper-Live Cutover (IB Gateway 4002)

Status as of 2026-06-30 (S, this session). Account basis: **NZ$5000**.

## What is DONE (this session)

- **Safety backup**: full crypto suite snapshot at `~/cryptobackup/` (Crypto + IBKRCrypto +
  stall-accountant). `BACKUP_STAMP.txt` inside. Restore = copy back.
- **Account sized to NZ$5000**: NZDUSD 0.584 (latest tick 2026-04-10, `~/Tick/NZDUSD`;
  no live NZDUSD feed) → **USD ~2920**.
  - Live engine `~/Crypto/src/ibkrcrypto_engine.cpp`: `account_usd()` default 2920,
    override env `RISK_USD`. Port **4002** (paper) default, clientId **88**.
  - Daily executor `~/Crypto/src/shadow_refresh.cpp`: `POOL_USD` default 2920 (was 10000),
    override env `POOL_USD`. Rebuilt.
- **Books zeroed**: daily/intraday/luke `state.json` flattened (slots/closed/realized=0),
  `ledger.csv` + `crypto_inbound.csv` header-only. Pre-zero archive:
  `~/IBKRCrypto/backtest/data/_zero_archive_<ts>/`. Daily rebuilds fresh at $2920 on the
  1am cron (still SHADOW until Gateway up).

## What is NOT done — operator prereqs (cannot be automated)

The crypto book is **still SHADOW**. No real orders route yet. To reach paper-live on 4002:

1. **Install TWS API** (public download from IBKR). Note the path
   `<TWSAPI>/source/cppclient/client`.
2. **Launch IB Gateway** in **paper** mode, API enabled, listening on **port 4002**.
   Requires IBKR login + 2FA → operator-only.
3. **Build the live engine** — ✅ DONE (2026-07-01). Real IBKR path compiles + links + runs:
   ```
   cd ~/Crypto
   cmake -S . -B build -DCRYPTO_WITH_IBKR=ON -DTWSAPI=/Users/jo/Omega/third_party/twsapi/client
   cmake --build build --target ibkrcrypto_engine -j
   ```
   Links integer-exact bid64 shim (`Crypto/third_party/bid64_integer.cpp`) for TWS
   DecimalFunctions (no Intel RDFP lib needed). Binary inits RiskManager $2920, attempts
   real TWS connect. Without `-DCRYPTO_WITH_IBKR=ON` it's still a no-op stub.
4. **Bridge signals → orders** — ✅ WIRED (2026-07-01). Chosen: **engine-as-order-router**
   (it owns the roster). Entries `placeOrder` on `--live` in `decide_()`. Exits/protection:
   NEW roster-scoped **panic flatten** (`--flatten`) — `reqPositions` → `position()` closes
   ONLY our SQF legs on the shared clientId-88 account (`crypto_ib_syms_`), never another
   fleet member's positions. Fixed latent bid64 garbage-quantity bug on `totalQuantity`.
   SHADOW by default; real paper orders only with `--live`.
5. **SQF contract resolution + market-data sub** — confirm CME Spot-Quoted-Futures
   conId/tradingClass per symbol live (vault: "real SQF data owed").
6. **Protection on the live path** — companion clip + BE-ratchet + regime gates currently act
   on paper state. On live they must fire **real flatten orders**. The protection self-test
   only covers shadow today.

## Go sequence (once #1–#6 ready)

1. Gateway paper **4002** up, API on.
2. Dry run shadow (no `--live`): `~/Crypto/build/ibkrcrypto_engine 4002` → confirms connect +
   `account=$2920` + slots subscribe.
3. Paper-live: `RISK_USD=2920 ~/Crypto/build/ibkrcrypto_engine --live 4002`. Verify a real
   paper fill + protection fires a real flatten on a forced adverse case.
4. Only after paper-live proven: 4001 (real money) is a separate go-decision.

## Scope note — single account vs 3 books

NZ$5000 = the live account. Set on the **daily** book ($2920). **Intraday** and **luke**
remain SHADOW research at their own pools (intraday still 10000, luke 10000) — NOT part of the
NZ$5000 live account. Confirm whether to fold/disable them before going live, else the GUI
total will overstate the live account.
