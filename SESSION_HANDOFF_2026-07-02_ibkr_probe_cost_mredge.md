# SESSION HANDOFF — 2026-07-02 — IBKR re-probe + Paxos cost guard + PaxosMRBasket edge

NZ time written: 02-07-2026 23.22. Work COMPLETE this session (built + committed + pushed
`a7509cb` + vault filed). Context low → handoff hard-stop. Fresh session does #1 below.

---

## OPERATOR ASK (verbatim)
"resume, plus look for more crypto edges now that we have the link working, please probe IBKR
again and check what else we can do plus ensure you have the correct costs, recheck them and
wire them in." Then: "handoff, then do 1 in next session, we will wait for the permissions."

---

## DONE THIS SESSION (do NOT re-derive)

### IBKR re-probe (gw4002 / clientId 88, whatIf non-destructive — `./build/ibkrcrypto_engine --probe-crypto 4002`)
- Crypto SQF **QTF(BTC) / QEF(ETH) STILL err201 closing-only** — 2nd permission NOT through. No change since 02-07 morning.
- NDX SQF **QNDX opens** — whatIf OK, commission **$0.47/side**, initMargin $578.83, maint $409.43.
- Spot Paxos BTC/ETH resolve (long-only). Tradeable surface UNCHANGED = **NDX SQF + long-only spot Paxos** only.

### Costs rechecked + WIRED (committed `a7509cb`, pushed)
- NDX SQF: roster 4bps ≈ real 2.35bps comm + spread ✓ (no change).
- BTC/ETH SQF 14/28bps: conservative; legs blocked anyway ✓.
- **Spot Paxos = 0.18%/side + $1.75 min/order → ~40bps on the $438 leg.** Live-route leak: route_ spot dust threshold was $5 → a $5 order pays $1.75 = **35%**. Wired cost-aware `MIN_ORDER_USD = $1.75/0.005 = $350` guard in `src/ibkrcrypto_engine.cpp` route_ spot leg (skip rebalances that would exceed 50bps, the validated cost band). Built clean.

### Edge hunt (pre-mine tombstone-checked via ~/second-brain/search.py)
- NDX intraday = graveyard → respected (daily NDX trend already in roster).
- **NEW finding [[PaxosMRBasket]]** (`backtest/paxos_mr_basket_bt.py`, committed): RSI2 bull-gated long-only MR. OOS **+23% PF1.50, 11/14 coins net-positive @35bps** (+16% PF1.33 @50bps). IBS dip-buy = DEAD (0/14).
- **Reconciled vs [[DailyMRCostWall]] tombstone** (that killed daily RSI2 MR): REPRODUCED its kill ungated (BTC/ETH/SOL FULL PF 0.93/0.91/0.89 — all <1 ✓, not re-falsifying). NEW basis = spot 18bps + BTC>200DMA gate + 14-coin breadth. Gate load-bearing: OOS PF 1.14→1.50, breadth 6→11/14.
- **Sober verdict: MARGINAL-additive.** On majors alone gated is marginal (BTC0.96/ETH1.19/SOL1.10). +23% is breadth-carried by mid-caps (LTC+74 LDO+94 BCH+44 LINK+44); DOGE−46 knife-catch loser. WEAKER than [[PaxosTrendBasket]] (emax OOS +30% PF2.07, works on majors). Higher turnover → more $1.75-floor drag.
- Backfilled **[[PaxosTrendBasket]]** entity (was validated but never filed) — re-validated emax OOS +30% PF2.07 @50bps.

Vault: index.md + log.md updated in /Users/jo/Memory-Chimera; entities PaxosMRBasket.md + PaxosTrendBasket.md written.

---

## NEXT SESSION — DO #1 (operator: "do 1 in next session")

**#1 = decide + (if yes) wire the PaxosMRBasket overlay.** This is the operator's capital-allocation call
per [[CompanionDominanceError]] discipline (judge standalone, operator allocates). Options:
- **SKIP** — it is only marginal-additive, weaker than the trend sleeve. Fully defensible.
- **WIRE as small diversifying overlay** — if operator says go:
  - New producer alongside `compute_trend_spot_targets_()` in `src/ibkrcrypto_engine.cpp`, SAME BTC>200DMA gate, **RSI2 only** (IBS dead), **exclude DOGE** (fat-tail knife-catch).
  - Own book sizing env (small), long-only, feeds `spot_tgt_usd_` (route_ already handles MKT+cashQty+IOC + the new $350 min-order guard).
  - MANDATE before merge: backtested adverse-protection verdict (new-book rule) + judge STANDALONE net-positive after cost, both WF halves. BT already shows OOS +23%/PF1.50 gated — attach that as the verdict, note trail-only (MR exits on strength, no cold cut needed; but confirm).
  - Higher turnover coin: watch the $1.75 min-commission floor per order — the $350 guard covers rebalances but MR entries must clear it too.

**#2 (BLOCKED — we WAIT):** crypto SQF QTF/QEF still err201 closing-only. Operator waiting on IB 2nd permission confirmation. Do NOT re-probe repeatedly; re-check only when operator says permission granted. When it lands: QTF/QEF perp legs (BTC/ETH long+short) unblock → the SQF perp roster (14/28bps cost) goes live.

---

## STATE / GOTCHAS
- Repo: /Users/jo/Crypto (remote git@github.com:Trendiisales/Crypto.git, main). HEAD `a7509cb` pushed.
- PAPER 4002 ONLY, never 4001. clientId 88 (probe conflicts if live cron mid-run — was free this session).
- Uncommitted noise in working tree (cron data CSV appends, companion_closed.csv, prior handoffs, doge/xsec BTs) — NOT mine, left alone. Do not bundle.
- Build: `cmake --build build --target ibkrcrypto_engine -j` (clangd shows a false missing-include for IbkrCryptoStrat.hpp — cmake compiles fine; IDE-only).
- Today BTC<200DMA → spot producer emits all-cash (gate holding) — expected, not a fault.
