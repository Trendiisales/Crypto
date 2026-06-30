// CmeSqfContracts.hpp -- IBKRCrypto: CME Spot-Quoted Futures (SQF) contract table.
//
// SQF are CME Globex, CFTC-regulated, centrally-cleared, perp-LIKE contracts:
//   - quoted at SPOT (not futures price)
//   - daily financing adjustment (TFA) = funding-rate analogue, derived from the
//     transparent published CME futures basis (NOT reflexive order-flow funding)
//   - one expiry per year (no monthly/quarterly roll)
// Reference: cmegroup.com/markets/spot-quoted-futures.html
//
// This maps an internal symbol (lowercase, Chimera convention "btcusdt") to the
// IB Contract fields used by omega::IbkrClient / TWS API, plus the SQF size + tick.
#ifndef IBKRCRYPTO_CME_SQF_CONTRACTS_H
#define IBKRCRYPTO_CME_SQF_CONTRACTS_H

#include <string>
#include <vector>
#include <cstdint>

namespace ibkrcrypto {

struct SqfContract {
    std::string internal;     // Chimera engine symbol, e.g. "btcusdt"
    std::string ib_symbol;    // IB underlying symbol / SQF code, e.g. "QTF"
    std::string sec_type;     // IB secType: "FUT" (SQF is a futures product)
    std::string exchange;     // "CME"
    std::string currency;     // "USD"
    double      unit;         // contract unit (BTC SQF = 0.01, ETH = 0.20, QNDX = 0.1)
    double      tick;         // minimum price increment (USD), spot-quoted
    const char* display;      // ORIGINAL/friendly name for GUI (operator readability)
    const char* note;
};

// VERIFIED against the LIVE IB gateway 2026-06-24 (acct U23757894, reqContractDetails):
// SQF IB symbol = the PRODUCT CODE (QTF/QEF), NOT "BTC"/"ETH" (those return no CME def).
// front conId rolls -> resolve by (ib_symbol,secType,exchange,tradingClass) at runtime;
// conIds below are the 2026-06-24 front for reference.
inline const std::vector<SqfContract>& sqf_table() {
    static const std::vector<SqfContract> t = {
        // internal     ib_sym secType exch  ccy   unit   tick  display (original name)            note
        {"btcusdt",     "QTF", "FUT", "CME", "USD", 0.01, 1.0,  "BTC Spot-Quoted (QTF)",           "mult 0.01 conId 887699154"},
        {"ethusdt",     "QEF", "FUT", "CME", "USD", 0.20, 0.10, "ETH Spot-Quoted (QEF)",           "mult 0.20 conId 887699104"},
        {"ndx",         "QNDX","FUT", "CME", "USD", 0.10, 0.10, "Nasdaq-100 Spot-Quoted (QNDX)",   "mult 0.10"},
        {"spx",         "QSPX","FUT", "CME", "USD", 1.00, 0.10, "S&P 500 Spot-Quoted (QSPX)",      "mult 1.0"},
        {"rty",         "QRTY","FUT", "CME", "USD", 1.00, 0.10, "Russell 2000 Spot-Quoted (QRTY)", "mult 1.0"},
        {"dow",         "QDOW","FUT", "CBOT","USD", 0.10, 0.10, "Dow Spot-Quoted (QDOW)",          "mult 0.10"},
        // SOL/XRP SQF (QSOL/QXRP) NOT listed on IB account as of 24-06-26 (CME announced
        // Dec'25). SOL/XRP only as standard dated futures (mult 500 / 50000) -> crypto_ladder.
    };
    return t;
}

// CME crypto LADDER (verified live) -- larger/dated contracts = far lower bps, the
// home for high-turnover mean-reversion (SQF nano fee-wall kills it). Dated => roll.
struct LadderContract { const char* internal,*ib_symbol,*trading_class; double mult; const char* note; };
inline const std::vector<LadderContract>& crypto_ladder() {
    static const std::vector<LadderContract> t = {
        {"btc_micro", "MBT", "MBT", 0.1,    "Micro BTC ~$6.4k notional ~6bps (BTC-IBS home)"},
        {"eth_micro", "MET", "MET", 0.1,    "Micro ETH"},
        {"sol_fut",   "SOL", "SOL", 500.0,  "SOL future ~$80k ~1bps (SOL-IBS home; SQF not on IB yet)"},
        {"xrp_fut",   "XRP", "XRP", 50000.0,"XRP future ~$55k ~1.6bps"},
        // newer CME crypto futures found live 24-06-26 (dated, long+short capable) -- expansion candidates, re-BT before use:
        {"link_fut",  "LNK", "LNK", 0.0,    "Chainlink CME future (LNK) + micro MLN"},
        {"ada_fut",   "ADA", "ADA", 0.0,    "Cardano CME future (ADA) + micro MCA"},
        {"xlm_fut",   "XLM", "XLM", 0.0,    "Stellar CME future (XLM)"},
        // SPOT-ONLY (Paxos/ZeroHash, long-only, NO perp/short): DOGE, LTC, BCH, AVAX,
        // MATIC, SUI, DOT -- do NOT fit the long+short perp engine. DOGE = spot only.
    };
    return t;
}

inline const SqfContract* find_sqf(const std::string& internal) {
    for (const auto& c : sqf_table()) if (c.internal == internal) return &c;
    return nullptr;
}

// Convert a coin-notional target (how Chimera engines size) into a whole number
// of SQF contracts. Returns 0 if the symbol is not a live SQF (size==0).
inline int coin_to_contracts(const std::string& internal, double coin_qty) {
    const SqfContract* c = find_sqf(internal);
    if (!c || c->unit <= 0.0) return 0;
    return static_cast<int>(coin_qty / c->unit + (coin_qty >= 0 ? 0.5 : -0.5));
}

} // namespace ibkrcrypto
#endif
