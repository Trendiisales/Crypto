// IbkrCryptoFeed.hpp -- IBKRCrypto: drop-in replacement for chimera::BinanceWSFeed.
//
// Same public contract as the Chimera spot feed so the entire EdgeEngine roster
// (285 engines) runs UNCHANGED:
//     feed.add_symbol("btcusdt");
//     feed.set_callback([](const MarketTick& t){ ... engine.on_tick(mid, ts); });
//     feed.start();
//
// Source of ticks is IBKR (TWS / IB Gateway) market data for CME crypto / SQF,
// via omega::IbkrClient (Omega/third_party/twsapi). Behind IBKRCRYPTO_WITH_IBKR,
// matching Omega's own OMEGA_WITH_IBKR convention: real build pulls live IB data;
// default build is a SHADOW/replay source so this compiles + runs with no TWS.
#ifndef IBKRCRYPTO_FEED_H
#define IBKRCRYPTO_FEED_H

#include <string>
#include <vector>
#include <functional>
#include <cstdint>
#include "CmeSqfContracts.hpp"

// Reuse the real Chimera MarketTick when building inside the Chimera tree;
// otherwise define a layout-compatible local mirror so the adapter compiles
// standalone (syntax-check / unit harness).
#if defined(__has_include)
#  if __has_include("live/BinanceWSFeed.hpp")
#    include "live/BinanceWSFeed.hpp"   // chimera::MarketTick
#    define IBKRCRYPTO_HAVE_CHIMERA_TICK 1
#  endif
#endif

namespace ibkrcrypto {

#ifndef IBKRCRYPTO_HAVE_CHIMERA_TICK
// Layout-compatible mirror of chimera::MarketTick (bid/ask/sizes + depth5).
struct MarketTick {
    std::string symbol;
    double bid = 0.0, ask = 0.0, bid_size = 0.0, ask_size = 0.0;
    double bid_prices[5] = {}, ask_prices[5] = {};
    int64_t ts_ms = 0;
};
using Tick = MarketTick;
#else
using Tick = chimera::MarketTick;
#endif

using TickCallback = std::function<void(const Tick&)>;

class IbkrCryptoFeed {
public:
    struct Config {
        std::string host = "127.0.0.1";
        int port      = 4002;   // IB Gateway paper; 4001 live (Omega convention)
        int clientId  = 21;     // distinct from Omega's gapper/bigcap clients
        bool shadow   = true;   // default SHADOW until data sub + edge proven
    };
    IbkrCryptoFeed() = default;
    explicit IbkrCryptoFeed(Config cfg) : cfg_(cfg) {}

    void add_symbol(const std::string& internal) { symbols_.push_back(internal); }
    void set_callback(TickCallback cb)            { cb_ = std::move(cb); }

    // start(): in a real build, opens IB Gateway, builds a CME SQF Contract per
    // symbol (secType=FUT exchange=CME), reqMktData, and on each tickPrice emits
    // a Tick -> callback. The engine roster consumes mid=(bid+ask)/2 exactly as
    // it does from Binance today.
    bool start() {
#ifdef IBKRCRYPTO_WITH_IBKR
        return start_live_();      // implemented in IbkrCryptoFeed.cpp (TWS API)
#else
        // SHADOW build: no live data. The harness/backtester drives ticks via
        // push_tick() from CSV (IB historical pulled by omega::IbkrClient), so
        // engines see identical input to live.
        started_ = true;
        return true;
#endif
    }
    void stop() { started_ = false; }

    // Test / backtest entry point: inject a tick (e.g. from IB historical CSV).
    void push_tick(const Tick& t) { if (cb_) cb_(t); }

    const std::vector<std::string>& symbols() const { return symbols_; }
    bool shadow() const { return cfg_.shadow; }

private:
#ifdef IBKRCRYPTO_WITH_IBKR
    bool start_live_();            // TWS API wiring (separate .cpp, gated build)
#endif
    Config cfg_;
    std::vector<std::string> symbols_;
    TickCallback cb_;
    bool started_ = false;
};

} // namespace ibkrcrypto
#endif
