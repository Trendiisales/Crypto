# Handoff — Crypto book paper-live on IBKR 4002 (mid-rewire, DOES NOT COMPILE)

## HARD STOP reason
Operator typed "handoff". Stopped immediately per standing rule. **`/Users/jo/Crypto/src/ibkrcrypto_engine.cpp` is mid-edit and will NOT compile** (see "FILE STATE" — 3 edits remain). No build, no orders, book still SHADOW. Finish the 3 remaining edits FIRST, then verify, then flip paper-live.

## The job (from prior handoff, unchanged)
Get the crypto/Chimera book executing live on IBKR alongside Omega, as one interleaved system. Target = **PAPER gateway 4002** (NOT real-money 4001 — that's a separate go-decision). Omega side is already LIVE+paper (commit `93f2e402`). This session = the crypto side.

## KEY FINDINGS this session (do NOT re-derive)
1. **Blockers 1 & 2 are RESOLVED** (per `Memory-Chimera/wiki/entities/CryptoLiveCutover4002.md`): eConnect works (VPS reboot freed clientId 88; `127.0.0.1:4002` is an SSH tunnel `ssh -fN -L 4002:127.0.0.1:4002 trader@185.167.119.59`, verified UP). SQF conId resolution works (9 resolve: QTF/QEF/QNDX front exp 20270611). CSV-warm works.
2. **The engine as-built trades ZERO even `--live`.** The vendored `RiskManager` (include/RiskManager.hpp) is a small-cap-EQUITY model (8% risk / 10% notional cap / 2× halt-gap / max-concurrent 4). On a $2920 futures book it caps notional at **$146**, below one SQF contract (ETH 0.20×$1613=$322, BTC 0.01×$60260=$603, NDX 0.10×$30276=$3028), so `coin_to_contracts` floors to **0** on every leg. Prior handoff's "just flip --live" premise was WRONG.
3. **The independent engine is a DIFFERENT book from the validated shadow book** and cannot reproduce it by recomputing signals:
   - Engine roster (`EdgeSpec ROSTER[]` in include/IbkrCryptoStrat.hpp) = 9 non-SOL SQF legs (eth/btc emax+kelt+regime, btc ibs, ndx tsmom+rsir). Shadow roster (include/crypto/Roster.hpp → state.json) = 15 legs incl **btc_roc, sol_roc, sol_emax/kelt/reg/ibs** (SOL + Roc legs the engine lacks).
   - First fix attempt (per-leg pool sizer `floor(per_leg/contract_notional)`, min-1) **over-sized ETH 3× (qty=3 vs shadow's 1)**: engine had n_open=3 → per_leg=$973 → floor(973/323)=3; shadow has n_open=5 (+SOL) → per_leg=$584 AND corr-downsize → ETH=1. Independent recompute can't match n_open or corr-downsize.
4. **OPERATOR DECISION (given this session):** pivot the live executor to **READ the validated shadow `state.json` and place delta orders to realize exactly those positions** — the only faithful path (honors "SHADOW LEDGER is the record of truth"). This is what the in-flight rewire implements.

## Current validated shadow book (backtest/data/ibkrcrypto/state.json, mode SHADOW, pool $2920)
5 open legs → **netted per IB-tradeable SQF conId**:
- QEF (ETH): eth_emax −1×1 = **SHORT 1**
- QTF (BTC): btc_emax −1×1, btc_roc −1×1 = **SHORT 2**
- QNDX (NDX): ndx_tsmom +1×1 = **LONG 1**
- SOL: sol_emax −1×1 → **SKIP (QSOL not listed on IB; shadow-only until it lists)** — live book = SQF-tradeable subset.
So the first paper-live flip (from flat) should place: **SELL 1 QEF, SELL 2 QTF, BUY 1 QNDX.**

## FILE STATE — `/Users/jo/Crypto/src/ibkrcrypto_engine.cpp` (real build: `-DCRYPTO_WITH_IBKR=ON` already in build/CMakeCache.txt; TWSAPI=/Users/jo/Omega/third_party/twsapi/client)

### DONE (compiles individually, but file as a whole does NOT — see REMAINING):
- `#include "json.hpp"` added (nlohmann 3.12 at third_party/json.hpp; use `nlohmann::json`).
- ctor: builds slots_ (9 SQF legs), then `tgt_net_ = load_state_targets_();`.
- `nextValidId` (non-flatten): sets `syncing_=true` + `reqPositions()` (position sync BEFORE routing, for idempotent delta orders).
- `position()`: if `syncing_` → `cur_pos_[c.symbol]+=q; return;` (roster-scoped record). Flatten branch unchanged below it.
- `positionEnd()`: if `syncing_` → clear flag, `cancelPositions()`, print held legs, `resolve_and_subscribe_()`.
- `historicalData`/`historicalDataEnd`: neutralized to no-ops (warm is CSV-only; no reqHistoricalData issued).
- `contractDetailsEnd`: resolve conId only, then `maybe_route_()` (warm/strat/target logic STRIPPED). Unresolved branch also calls `maybe_route_()`.
- `maybe_route_()`: `if(++cd_ends_ >= slots_.size()) route_();`.
- `load_state_targets_()`: NEW method — parses state.json slots (key/pos/contracts), maps coin→internal→`find_sqf`, nets signed contracts per ib_symbol into `tgt_net_`, records `tgt_px_[ib_symbol]=entry_px`, skips SOL (no SQF). Prints `[TGT]` lines.
- `Slot` struct: gained `int target=0;` (now UNUSED — harmless, can delete later).

### REMAINING — 3 edits, file won't compile until all 3 done:
**(A) Declare the two new members.** Find the members block:
```cpp
    std::set<std::string> crypto_ib_syms_;   // roster scope for panic flatten
    RiskManager risk_;                        // retained as a future catastrophe hook (sizing now mirrors the shadow book)
    bool syncing_=false;                      // reqPositions sync in flight (pre-route)
    int  cd_ends_=0;                          // contractDetailsEnd count -> route when == slots_
    std::unordered_map<std::string,double> cur_pos_;  // current IB net position per SQF ib_symbol
```
Add after it:
```cpp
    std::unordered_map<std::string,int>    tgt_net_;  // net signed SQF contracts per ib_symbol (from shadow state.json)
    std::unordered_map<std::string,double> tgt_px_;   // per-symbol mark for the ledger only
```

**(B) Replace `route_()`** (it still uses the OLD per-slot strat sizer `s.target`/`s.strat`/`per_leg`/`floor` — DELETE that whole body and replace with the state.json-mirror version):
```cpp
    void route_(){
        std::unordered_map<std::string,Contract> con;
        for(auto& s:slots_) if(s.has_con) con[s.sc->ib_symbol]=s.resolved;
        std::unordered_map<std::string,int> net = tgt_net_;      // targets from shadow state.json
        for(auto& kv:cur_pos_) net.emplace(kv.first,0);          // held-but-flipped-flat legs -> close
        std::printf("[IBKRCRYPTO] routing %zu SQF symbol(s) from shadow state.json live=%d\n",net.size(),live_);
        std::ofstream led(crypto::data_dir()+"/daily_ledger.csv",std::ios::app);
        for(auto& kv:net){
            const std::string& sym=kv.first; int tgt=kv.second;
            int cur=(int)std::llround(cur_pos_.count(sym)?cur_pos_[sym]:0.0);
            int delta=tgt-cur;
            double px=tgt_px_.count(sym)?tgt_px_[sym]:0.0;
            led<<sym<<","<<tgt<<","<<cur<<","<<delta<<","<<px<<","<<(live_?"LIVE":"SHADOW")<<"\n";
            std::printf("[IBKRCRYPTO][%s] %-4s net_target=%+d cur=%+d delta=%+d px=%.2f\n",
                        live_?"LIVE":"SHADOW",sym.c_str(),tgt,cur,delta,px);
            if(delta!=0 && live_ && con.count(sym)){
                Order o; o.action=delta>0?"BUY":"SELL"; o.orderType="MKT";
                o.totalQuantity=DecimalFunctions::doubleToDecimal((double)std::abs(delta));
                cli_->placeOrder(nextId_++, con[sym], o);
                std::printf("[IBKRCRYPTO][LIVE] ORDER %s %s %d @ MKT (conId=%ld)\n",
                            o.action.c_str(),sym.c_str(),std::abs(delta),(long)con[sym].conId);
            } else if(delta!=0 && live_){
                std::fprintf(stderr,"[IBKRCRYPTO] WANT %s delta=%+d but conId UNRESOLVED -> skipped\n",sym.c_str(),delta);
            }
        }
        write_state_();
        std::printf("[IBKRCRYPTO] route complete\n");
    }
```

**(C) Replace `write_state_()` body** (it still writes per-slot `slots_[i].pos`; dump the net targets instead):
```cpp
    void write_state_(){
        const std::string sp = crypto::env_or("IBKRCRYPTO_ENGINE_STATE", crypto::data_dir()+"/engine_state.json");
        std::ofstream st(sp);
        st<<"{\"engine\":\"IBKRCrypto-exec\",\"mode\":\""<<(live_?"LIVE":"SHADOW")<<"\",\"source\":\"shadow_state.json\",\"net\":[";
        bool first=true;
        for(auto& kv:tgt_net_){ if(!first)st<<","; first=false; st<<"{\"sym\":\""<<kv.first<<"\",\"target\":"<<kv.second<<"}"; }
        st<<"]}";
    }
```
(Keep the existing explanatory comment above write_state_ about not clobbering the shared daily-book state.json — the private engine_state.json path is correct.)

## THEN — verify + flip (in order)
1. Build: `cd /Users/jo/Crypto && cmake --build build --target ibkrcrypto_engine -j` (clangd "file not found" diagnostics in-editor are false — CMake supplies -Iinclude/-Ithird_party; trust the actual build).
2. **SHADOW dry-run** (zero orders): `RISK_USD=2920 ./build/ibkrcrypto_engine 4002`. EXPECT: `[TGT]` lines (eth_emax→QEF −1, btc_emax+btc_roc→QTF −2, ndx_tsmom→QNDX +1, sol_* skipped), position sync (0 held), then `[SHADOW] QEF net_target=-1 cur=+0 delta=-1`, `QTF -2`, `QNDX +1`, `route complete`. Confirm deltas match the shadow book table above.
3. **Paper-live flip** (Task #2): `RISK_USD=2920 ./build/ibkrcrypto_engine --live 4002`. Verify a **real paper fill** on the IBKR paper account (clientId 88). Watch for `[LIVE] ORDER SELL QEF 1` etc. Confirm via IB (reqPositions next run shows QEF −1 / QTF −2 / QNDX +1). **Runs alongside the Omega fleet on the shared clientId-88 gateway — flatten is roster-scoped so it never touches other fleet legs.**
4. **Prove --flatten** (Task #3, prereq 6 — still OPEN): with a real paper position open, run `./build/ibkrcrypto_engine --flatten --live 4002`; confirm it closes ONLY our SQF legs (QTF/QEF/QNDX), leaves other fleet positions.
5. **Idempotency check:** run `--live 4002` again with positions already at target → all deltas 0 → zero orders (proves the position-sync/delta logic).
6. **Wire cron + file vaults** (Task #4): add the `--live` engine to run AFTER the daily CSV refresh + shadow_refresh (state.json must be fresh first — same dependency the shadow cron has). Update `Memory-Chimera/wiki/entities/CryptoLiveCutover4002.md` (prereq 6 → done, mode → LIVE-paper), index.md, log.md (`## [DD-MM-YYYY HH.MM]` NZ time `TZ='Pacific/Auckland' date '+%d-%m-%Y %H.%M'`). Vault-update is MANDATORY on deploy.

## Open caveats to note in the vault
- SOL leg is shadow-only until QSOL lists on IB (live book = SQF-tradeable subset). If operator wants SOL live now, route it via the CME SOL dated future (crypto_ladder, mult 500) — separate contract resolution, NOT in scope of the state.json-mirror (state.json SOL uses mult 5.0, won't map to the dated future cleanly).
- SQF yearly roll: closing orders use the front conId resolved this session; if a held position is on an older expiry the close won't offset it. Fine for the first-flip-from-flat; add roll handling later.
- RiskManager is now retained only as a future catastrophe hook (not in the size path). Its equity-model caps are wrong for a futures book — do not re-enable in sizing.
- git: /Users/jo/Crypto HEAD `ee5c8ea`; working tree also has pre-existing noise (backtest/data/*.csv, xsec_allocator.py, multiyr/) — do NOT bundle. Commit ONLY src/ibkrcrypto_engine.cpp when the rewire is verified.

## Tasks (this session's TaskList)
#1 SHADOW dry-run — done (revealed the sizer/pivot). #2 paper-live flip — pending. #3 prove --flatten — pending (prereq 6). #4 vault+cron — pending.
