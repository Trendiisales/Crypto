// IbkrCryptoEngine.cpp -- IBKRCrypto live engine (Omega/ibkr fleet member).
//
// DESTINED FOR: /Users/jo/Omega/ibkr/IbkrCryptoEngine.cpp
// Pattern: identical to BigCapMomoEngine.cpp / GapShortEngine.cpp --
//   * subclasses DefaultEWrapper, eConnect(host, port, clientId=88) to the ONE
//     IB gateway shared by the whole Omega ibkr fleet (4002 paper / 4001 live)
//   * own RiskManager (catastrophe caps) -- per-process, like the other fleet members
//   * trades CME Spot-Quoted Futures (secType=FUT exchange=CME) on the validated
//     roster (BTC-IBS, SOL-IBS, ETH-TSMom) from IbkrCryptoStrat.hpp
//   * forward-ledger -> data/ibkrcrypto/daily_ledger.csv  (like gapshort)
//   * state JSON     -> data/ibkrcrypto/state.json        (for the GUI)
//   * SHADOW by default (no orders) until edge re-confirmed on real SQF data
//
// Build (on the box with TWS API, same recipe as the fleet):
//   c++ -std=c++20 -O2 -DOMEGA_WITH_IBKR -I../include -I$TWSAPI/source/cppclient/client \
//       IbkrCryptoEngine.cpp $TWSAPI/source/cppclient/client/*.cpp -lpthread -o ibkrcrypto
// Not yet added to CMake -- a standalone fleet binary, wired on approval.
#include <cstdio>
#include <cstring>
#include <cstdlib>
#include <string>
#include <unordered_map>
#include <fstream>
#include "IbkrCryptoStrat.hpp"
#include "CmeSqfContracts.hpp"
#include "RiskManager.hpp"            // vendored from Omega/ibkr (catastrophe caps)
#include "crypto/Roster.hpp"         // crypto::data_dir() — env-overridable output dir

#ifdef OMEGA_WITH_IBKR
#include "EWrapper.h"
#include "DefaultEWrapper.h"
#include "EClientSocket.h"
#include "EReader.h"
#include "EReaderOSSignal.h"
#include "Contract.h"
#include "Order.h"

using namespace ibkrcrypto;

// Account capital (USD-notional, since SQF are USD-denominated).
// Operator basis 2026-06-30: NZ$5000 account. NZDUSD 0.584 (latest tick 2026-04-10,
// ~/Tick/NZDUSD; no live NZDUSD feed) -> USD ~2920. Override with RISK_USD env to
// correct the FX or resize. Was 50000 (pre-cutover paper).
static double account_usd(){
    if(const char* e = std::getenv("RISK_USD")) { double v = std::atof(e); if(v > 0) return v; }
    return 2920.0;  // NZ$5000 @ NZDUSD 0.584
}

// Build a CME Spot-Quoted Future contract for an IBKRCrypto symbol.
static Contract sqf_contract(const SqfContract& sc){
    Contract c; c.symbol=sc.ib_symbol; c.secType=sc.sec_type;
    c.exchange=sc.exchange; c.currency=sc.currency;
    // tradingClass/localSymbol for the SQF product resolved via reqContractDetails
    // at startup (see resolve_contracts_) -- CME SQF product code, not yet hardcoded.
    return c;
}

class IbkrCryptoEngine : public DefaultEWrapper {
public:
    IbkrCryptoEngine(bool live)
        : live_(live), risk_(account_usd()) {
        risk_.new_day();
        cli_ = std::make_unique<EClientSocket>(this,&sig_);
        // build the validated roster
        for(const auto& e : ROSTER){
            const SqfContract* sc = find_sqf(e.sym);
            if(!sc || sc->unit<=0) continue;
            slots_.push_back({e.sym, sc, IbkrCryptoStrat(e.mode), 0});
        }
    }

    bool connect(const char* host,int port,int id){
        if(!cli_->eConnect(host,port,id,false)) return false;
        rd_=std::make_unique<EReader>(cli_.get(),&sig_); rd_->start(); return true;
    }
    void pump(){ sig_.waitForSignal(); errno=0; rd_->processMsgs(); }
    EClientSocket* cli(){ return cli_.get(); }

    void nextValidId(OrderId id) override { nextId_=id;
        std::printf("[IBKRCRYPTO] connected, nextValidId=%ld clientId=88\n",(long)id);
        resolve_and_subscribe_();
    }

    // historicalData / historicalDataUpdate feed daily bars into each strat.
    void historicalData(TickerId rid,const Bar& b) override {
        int s=rid_to_slot_[rid]; if(s<0) return;
        ibkrcrypto::Bar bar{ std::stod(b.open),std::stod(b.high),std::stod(b.low),std::stod(b.close) };
        slots_[s].strat.on_daily_bar(bar);
    }
    void historicalDataEnd(int rid,const std::string&,const std::string&) override {
        int s=rid_to_slot_[rid]; if(s<0||!slots_[s].strat.warm()) return;
        decide_(s, slots_[s].last_close);
    }

    void error(int id,int code,const std::string& msg,const std::string&) override {
        if(code!=2104&&code!=2106&&code!=2158) std::fprintf(stderr,"[IBKRCRYPTO] err %d: %s\n",code,msg.c_str());
    }

private:
    struct Slot { std::string sym; const SqfContract* sc; IbkrCryptoStrat strat; double last_close; };

    void resolve_and_subscribe_(){
        // For each slot: reqContractDetails (resolve SQF conId/tradingClass) then
        // reqHistoricalData("1 Y","1 day",MIDPOINT) to warm + reqMktData for live.
        for(size_t i=0;i<slots_.size();++i){
            int rid=1000+(int)i; rid_to_slot_[rid]=(int)i;
            Contract c=sqf_contract(*slots_[i].sc);
            cli_->reqHistoricalData(rid,c,"","1 Y","1 day","MIDPOINT",1,1,false,TagValueListSPtr());
        }
        std::printf("[IBKRCRYPTO] %zu SQF slots subscribed (SHADOW=%d) account=$%.0f USD (NZ$5000 @ 0.584)\n",
                    slots_.size(),!live_,account_usd());
    }

    void decide_(int s, double px){
        Slot& sl=slots_[s];
        int want = sl.strat.target();
        double mult = sl.strat.size_mult();
        if(want==sl.pos) return;
        // size via RiskManager catastrophe caps * vol-target mult, then -> SQF contracts
        double notional = risk_.allow_entry(sl.sym, px, px) * mult;
        int contracts = coin_to_contracts(sl.sym, notional/px);
        log_(sl.sym, want, contracts, px, mult);
        if(live_ && want!=0 && contracts!=0){
            Order o; o.action = want>0?"BUY":"SELL"; o.orderType="MKT"; o.totalQuantity=std::abs(contracts);
            cli_->placeOrder(nextId_++, sqf_contract(*sl.sc), o);
        }
        sl.pos=want;
    }

    void log_(const std::string& sym,int want,int contracts,double px,double mult){
        std::ofstream led(crypto::data_dir()+"/daily_ledger.csv",std::ios::app);
        led<<sym<<","<<want<<","<<contracts<<","<<px<<","<<mult<<","<<(live_?"LIVE":"SHADOW")<<"\n";
        std::printf("[IBKRCRYPTO][%s] %s target=%d contracts=%d px=%.2f vt=%.2f\n",
            live_?"LIVE":"SHADOW", sym.c_str(), want, contracts, px, mult);
        write_state_();   // refresh GUI state.json
    }
    void write_state_(){
        std::ofstream st(crypto::data_dir()+"/state.json");
        st<<"{\"engine\":\"IBKRCrypto\",\"mode\":\""<<(live_?"LIVE":"SHADOW")<<"\",\"slots\":[";
        for(size_t i=0;i<slots_.size();++i){ if(i)st<<","; st<<"{\"sym\":\""<<slots_[i].sym<<"\",\"pos\":"<<slots_[i].pos<<"}"; }
        st<<"]}";
    }

    bool live_; OrderId nextId_=0;
    EReaderOSSignal sig_{2000};
    std::unique_ptr<EClientSocket> cli_; std::unique_ptr<EReader> rd_;
    std::vector<Slot> slots_; std::unordered_map<int,int> rid_to_slot_;
    RiskManager risk_;
};

int main(int argc,char**argv){
    setvbuf(stdout,nullptr,_IONBF,0);
    int port=4002; bool live=false;
    for(int i=1;i<argc;++i){ if(!strcmp(argv[i],"--live")) live=true; else port=atoi(argv[i]); }
    IbkrCryptoEngine e(live);
    if(!e.connect("127.0.0.1",port,88)){ std::printf("connect failed\n"); return 1; }
    for(int i=0;i<2000;++i) e.pump();
    e.cli()->eDisconnect(); std::printf("[IBKRCRYPTO] session end\n"); return 0;
}

#else  // !OMEGA_WITH_IBKR  -- CSV/dev build: signal core is still testable
int main(){ std::printf("IbkrCryptoEngine: build with -DOMEGA_WITH_IBKR + TWS API to run live.\n"); return 0; }
#endif
