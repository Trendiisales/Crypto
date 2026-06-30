// CmeSqfExecutor.hpp -- IBKRCrypto: drop-in replacement for chimera::SpotExecutor.
//
// Mirrors SpotExecutor::execute(symbol, is_buy, qty, price) -> OrderResult so the
// Chimera live loop / MultiVenueExecutor route is unchanged. Difference: routes a
// CME Spot-Quoted Future order through IBKR (omega::IbkrClient + TWS API) instead
// of a Binance spot REST order. Coin-notional from the engines is converted to a
// whole number of SQF contracts via CmeSqfContracts.
//
// SHADOW by default (signs/logs, never sends) -- same posture as SpotExecutor and
// the whole-book SHADOW rule. Live send is behind IBKRCRYPTO_WITH_IBKR.
#ifndef IBKRCRYPTO_CME_SQF_EXECUTOR_H
#define IBKRCRYPTO_CME_SQF_EXECUTOR_H

#include <string>
#include <cstdio>
#include <atomic>
#include "CmeSqfContracts.hpp"

namespace ibkrcrypto {

// Layout-compatible with chimera::OrderResult (ok/shadow/order_id/status/qty/px).
struct SqfOrderResult {
    bool        ok           = false;
    bool        shadow       = true;
    long        order_id     = 0;
    std::string status;
    int         contracts    = 0;     // SQF contracts sent (signed)
    double      avg_price    = 0.0;
};

class CmeSqfExecutor {
public:
    struct Config {
        std::string host = "127.0.0.1";
        int  port    = 4002;          // IB Gateway paper (4001 live)
        int  clientId = 22;
        bool shadow  = true;          // never POST until edge proven + go-live
    };
    CmeSqfExecutor() = default;
    explicit CmeSqfExecutor(Config cfg) : cfg_(cfg) {}

    bool init() { ready_ = true; return true; }   // live build: eConnect here

    // execute -- market order on the SQF for `internal` symbol.
    //   is_buy : true=long SQF, false=short SQF
    //   qty    : coin-notional target from the engine (e.g. 0.05 BTC)
    //   price  : signal price (logging; SQF fills at exchange spot-quote)
    SqfOrderResult execute(const std::string& internal, bool is_buy,
                           double qty, double price) {
        SqfOrderResult r; r.shadow = cfg_.shadow;
        const SqfContract* c = find_sqf(internal);
        if (!c || c->unit <= 0.0) {
            std::fprintf(stderr, "[IBKRCRYPTO] no live SQF for %s -- dropped\n", internal.c_str());
            return r;
        }
        int contracts = coin_to_contracts(internal, qty);
        if (contracts == 0) {                       // qty below 1 contract
            std::fprintf(stderr, "[IBKRCRYPTO] %s qty %.6f < 1 SQF unit (%.2f) -- skip\n",
                         internal.c_str(), qty, c->unit);
            return r;
        }
        r.contracts = is_buy ? contracts : -contracts;

        if (cfg_.shadow) {
            std::printf("[IBKRCRYPTO][SHADOW] %s %s %d SQF (%s) ref_px=%.2f\n",
                        c->ib_symbol.c_str(), is_buy ? "BUY" : "SELL",
                        contracts, c->exchange.c_str(), price);
            r.ok = true; r.status = "SHADOW"; r.avg_price = price;
            shadow_fills_.fetch_add(1, std::memory_order_relaxed);
            return r;
        }
#ifdef IBKRCRYPTO_WITH_IBKR
        r = place_live_(*c, is_buy, contracts, price);   // TWS placeOrder (.cpp)
#else
        std::fprintf(stderr, "[IBKRCRYPTO] live build flag off -- order dropped\n");
#endif
        return r;
    }

    bool ready() const { return ready_; }

private:
#ifdef IBKRCRYPTO_WITH_IBKR
    SqfOrderResult place_live_(const SqfContract&, bool, int, double);
#endif
    Config cfg_;
    bool ready_ = false;
    std::atomic<long> shadow_fills_{0};
};

} // namespace ibkrcrypto
#endif
