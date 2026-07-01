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
#include <cmath>
#include <string>
#include <set>
#include <unordered_map>
#include <fstream>
#include <sstream>
#include <cctype>
#include "IbkrCryptoStrat.hpp"
#include "CmeSqfContracts.hpp"
#include "RiskManager.hpp"            // vendored from Omega/ibkr (catastrophe caps)
#include "crypto/Roster.hpp"         // crypto::data_dir() — env-overridable output dir
#include "json.hpp"                  // nlohmann — read the validated shadow state.json

#ifdef OMEGA_WITH_IBKR
#include "EWrapper.h"
#include "DefaultEWrapper.h"
#include "EClientSocket.h"
#include "EReader.h"
#include "EReaderOSSignal.h"
#include "Contract.h"
#include "Order.h"
#include "OrderState.h"      // whatIf preview result (commission/margin) — --probe-crypto
#include "Decimal.h"          // DecimalFunctions::doubleToDecimal — Order.totalQuantity is bid64

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
    IbkrCryptoEngine(bool live, bool flatten=false, bool probe=false)
        : live_(live), flatten_(flatten), probe_(probe), risk_(account_usd()) {
        risk_.new_day();
        cli_ = std::make_unique<EClientSocket>(this,&sig_);
        // build the validated roster
        for(const auto& e : ROSTER){
            const SqfContract* sc = find_sqf(e.sym);
            if(!sc || sc->unit<=0) continue;
            slots_.push_back({e.sym, sc, IbkrCryptoStrat(e.mode), 0.0, 0});
            // roster scope for PANIC flatten: only close OUR SQF legs on the
            // shared account, never another fleet member's positions.
            crypto_ib_syms_.insert(sc->ib_symbol);
        }
        // Targets are the VALIDATED shadow book (shadow_refresh -> state.json), NOT an
        // independent recompute. The engine only resolves conIds + routes delta orders to
        // realize exactly those positions. Recomputing signals here diverges from the book
        // (different roster/n_open, no corr-downsize) -> 3x mis-sizing was observed.
        tgt_net_ = load_state_targets_();
    }

    bool connect(const char* host,int port,int id){
        if(!cli_->eConnect(host,port,id,false)) return false;
        rd_=std::make_unique<EReader>(cli_.get(),&sig_); rd_->start(); return true;
    }
    void pump(){ sig_.waitForSignal(); errno=0; rd_->processMsgs(); }
    EClientSocket* cli(){ return cli_.get(); }

    void nextValidId(OrderId id) override { nextId_=id;
        std::printf("[IBKRCRYPTO] connected, nextValidId=%ld clientId=88\n",(long)id);
        if(probe_){
            // Non-destructive eligibility probe: can this account resolve IBKR spot-crypto
            // (CSP/Paxos) contracts? reqContractDetails only -- NO orders. err 200 "no
            // security definition" / permission errors surface via error(). reqId>=3000.
            const char* venues[]={"PAXOS","ZEROHASH"};
            const char* coins[]={"BTC","ETH"};
            int rid=3000;
            for(const char* v:venues) for(const char* co:coins){
                Contract c; c.symbol=co; c.secType="CRYPTO"; c.exchange=v; c.currency="USD";
                std::printf("[IBKRCRYPTO][PROBE] reqContractDetails %s CRYPTO %s USD (reqId=%d)\n",co,v,rid);
                cli_->reqContractDetails(rid++, c);
            }
            return;
        }
        if(flatten_){
            std::printf("[IBKRCRYPTO] PANIC FLATTEN mode: requesting positions (roster-scoped, live=%d)\n",live_);
            cli_->reqPositions();   // -> position() sweep, close only our SQF legs
            return;
        }
        // Non-flatten: sync current IB positions FIRST (roster-scoped) so route_ can
        // order the DELTA to target, not the full target -- makes the daily cron run
        // idempotent (no doubling-up each day). resolve+warm+route fire from positionEnd.
        syncing_=true;
        std::printf("[IBKRCRYPTO] position sync (reqPositions) before routing\n");
        cli_->reqPositions();
    }

    // NOTE: warm is now CSV-only (SQF HMDS has no usable daily history), so no
    // reqHistoricalData is issued and these IB historical callbacks never fire. Kept as
    // no-ops in case a future venue supplies SQF daily bars. See warm_from_csv_ / route_.
    void historicalData(TickerId,const ::Bar&) override {}
    void historicalDataEnd(int,const std::string&,const std::string&) override {}

    void error(int id,int code,const std::string& msg,const std::string&) override {
        if(code!=2104&&code!=2106&&code!=2158) std::fprintf(stderr,"[IBKRCRYPTO] err %d: %s\n",code,msg.c_str());
    }

    // whatIf order preview result -- commission/margin if eligible; a permission error
    // instead arrives via error(). Only relevant to --probe-crypto.
    void openOrder(OrderId,const Contract& c,const Order&,const OrderState& st) override {
        if(!probe_) return;
        std::printf("[IBKRCRYPTO][PROBE] whatIf OK %s status=%s commission=%s %s initMargin=%s maintMargin=%s\n",
                    c.symbol.c_str(), st.status.c_str(),
                    st.commission==st.commission?std::to_string(st.commission).c_str():"n/a", st.commissionCurrency.c_str(),
                    st.initMarginChange.c_str(), st.maintMarginChange.c_str());
    }

    // PANIC FLATTEN — close ONLY our roster's SQF legs on the shared account.
    // Roster scope (crypto_ib_syms_) means we never touch another Omega ibkr
    // fleet member's positions on the same clientId-88 account.
    void position(const std::string&,const Contract& c,Decimal pos,double) override {
        if(!crypto_ib_syms_.count(c.symbol)) return;            // not our leg — leave it
        double q = DecimalFunctions::decimalToDouble(pos);
        if(syncing_){ cur_pos_[c.symbol]+=q; return; }          // record for order-diff
        if(!flatten_) return;
        if(q==0.0) return;
        Order o; o.action = q>0?"SELL":"BUY"; o.orderType="MKT";
        o.totalQuantity = DecimalFunctions::doubleToDecimal(std::fabs(q));
        std::printf("[IBKRCRYPTO][FLATTEN] %s pos=%.4f -> %s %.4f (live=%d)\n",
                    c.symbol.c_str(),q,o.action.c_str(),std::fabs(q),live_);
        if(live_) cli_->placeOrder(nextId_++,c,o);              // SHADOW prints only, no order
    }
    void positionEnd() override {
        if(syncing_){
            syncing_=false; cli_->cancelPositions();
            std::printf("[IBKRCRYPTO] position sync done: %zu roster leg(s) held\n",cur_pos_.size());
            for(auto& kv:cur_pos_) std::printf("[IBKRCRYPTO]   cur %s = %.4f\n",kv.first.c_str(),kv.second);
            resolve_and_subscribe_();
            return;
        }
        std::printf("[IBKRCRYPTO][FLATTEN] sweep complete (live=%d)\n",live_);
        cli_->cancelPositions();
    }

    // #5 conId resolution: a bare (symbol=QTF,secType=FUT,exchange=CME) is ambiguous
    // -> err 321 "enter a local symbol or an expiry". reqContractDetails returns the
    // fully-qualified front contract(s); keep the FRONT (earliest expiry) per slot,
    // then route orders on the resolved conId. WARM is from the local daily CSV
    // (see warm_from_csv_), NOT IB HMDS -- newly-listed 2025 SQF have no daily HMDS
    // history (err 162 / empty), and EMAx needs 200 bars which those contracts cannot
    // supply. The CSV is the exact validated series the backtest+shadow book use, so
    // warming from it is bar-for-bar faithful; IB is used only for execution.
    void contractDetails(int reqId,const ContractDetails& cd) override {
        if(reqId>=3000){   // PROBE: spot-crypto eligibility
            const Contract& c=cd.contract;
            std::printf("[IBKRCRYPTO][PROBE] RESOLVED %s %s %s conId=%ld localSym=%s\n",
                        c.symbol.c_str(),c.secType.c_str(),c.exchange.c_str(),(long)c.conId,c.localSymbol.c_str());
            // Decisive non-destructive permission test: whatIf order (preview only, never
            // executes). Returns commission/margin in orderState if eligible, or an error
            // if not permitted -- the exact check that would have caught the SQF err 201.
            // IBKR spot-crypto MKT orders require IOC. Probe PAXOS only (one venue enough).
            if(c.exchange=="PAXOS"){
                // LMT + share qty (not MKT+cashQty): the integer bid64 shim cannot encode
                // UNSET_DECIMAL that cashQty leaves in totalQuantity (err 320 '-inf'). A LMT
                // exercises the SAME permission gate. Far-from-market limit so it never fills.
                double lmt = (c.symbol=="BTC")?1000.0:100.0;
                Order o; o.action="BUY"; o.orderType="LMT"; o.lmtPrice=lmt;
                o.totalQuantity=DecimalFunctions::doubleToDecimal(1.0);   // whole unit: integer shim can't do fractional; whatIf never fills
                o.tif="DAY"; o.whatIf=true;
                std::printf("[IBKRCRYPTO][PROBE] whatIf BUY 1 %s @ LMT %.0f (conId=%ld)\n",c.symbol.c_str(),lmt,(long)c.conId);
                cli_->placeOrder(nextId_++, c, o);
            }
            return;
        }
        auto it=cd_rid_to_slot_.find(reqId); if(it==cd_rid_to_slot_.end()) return;
        Slot& sl=slots_[it->second];
        const std::string& exp=cd.contract.lastTradeDateOrContractMonth;
        if(!sl.has_con || exp<sl.front_expiry){ sl.resolved=cd.contract; sl.has_con=true; sl.front_expiry=exp; }
    }
    void contractDetailsEnd(int reqId) override {
        if(reqId>=3000){ std::printf("[IBKRCRYPTO][PROBE] end reqId=%d\n",reqId); return; }
        auto it=cd_rid_to_slot_.find(reqId); if(it==cd_rid_to_slot_.end()) return;
        int s=it->second; Slot& sl=slots_[s];
        if(!sl.has_con){ std::fprintf(stderr,"[IBKRCRYPTO] UNRESOLVED %s (%s) — no CME def, slot idle\n",
                                      sl.sym.c_str(),sl.sc->ib_symbol.c_str()); maybe_route_(); return; }
        std::printf("[IBKRCRYPTO] resolved %s -> %s conId=%ld exp=%s\n",
                    sl.sym.c_str(),sl.resolved.localSymbol.c_str(),(long)sl.resolved.conId,sl.front_expiry.c_str());
        maybe_route_();
    }
    // Route once every slot's contractDetails has returned (resolved or not), so con[] is
    // populated for all SQF symbols before we diff targets against current positions.
    void maybe_route_(){ if(++cd_ends_ >= (int)slots_.size()) route_(); }

    // Net signed SQF contracts per ib_symbol from the validated shadow state.json.
    // Each shadow slot: key ("btc_emax"/"sol_roc"/"ndx_tsmom"), pos (-1/0/+1), contracts.
    // Legs whose coin has no CME SQF on IB (SOL — QSOL not listed) are shadow-only -> skip
    // (the live book is the SQF-tradeable subset until QSOL lists). tgt_px_ keeps a mark for
    // the ledger only.
    std::unordered_map<std::string,int> load_state_targets_(){
        std::unordered_map<std::string,int> net;
        const std::string sp = crypto::env_or("IBKRCRYPTO_STATE", crypto::data_dir()+"/state.json");
        std::ifstream f(sp);
        if(!f){ std::fprintf(stderr,"[IBKRCRYPTO] state.json missing at %s -> no targets\n",sp.c_str()); return net; }
        nlohmann::json j; try { f>>j; } catch(...){ std::fprintf(stderr,"[IBKRCRYPTO] state.json parse fail %s\n",sp.c_str()); return net; }
        for(const auto& sl : j.value("slots", nlohmann::json::array())){
            int pos = sl.value("pos",0); int ct = sl.value("contracts",0);
            if(pos==0 || ct==0) continue;
            std::string key = sl.value("key", std::string());
            std::string coin = key.substr(0, key.find('_'));
            std::string internal = (coin=="ndx") ? "ndx" : coin+"usdt";
            const SqfContract* sc = find_sqf(internal);
            if(!sc){ std::fprintf(stderr,"[IBKRCRYPTO][TGT] %s (%s) not IB-SQF-tradeable -> shadow-only, skip\n",
                                  key.c_str(),coin.c_str()); continue; }
            net[sc->ib_symbol] += (pos>0? ct : -ct);
            if(sl.contains("entry_px")) tgt_px_[sc->ib_symbol] = sl.value("entry_px",0.0);
            std::printf("[IBKRCRYPTO][TGT] %-10s pos=%+d ct=%d -> %s net=%+d\n",
                        key.c_str(),pos,ct,sc->ib_symbol.c_str(),net[sc->ib_symbol]);
        }
        std::printf("[IBKRCRYPTO] loaded %zu SQF target(s) from %s\n",net.size(),sp.c_str());
        return net;
    }

private:
    struct Slot { std::string sym; const SqfContract* sc; IbkrCryptoStrat strat; double last_close; int pos;
                  Contract resolved; bool has_con=false; std::string front_expiry; int target=0; };

    // Map a roster symbol ("btcusdt"/"ethusdt"/"solusdt"/"ndx") to its validated
    // daily-OHLC CSV (the same series the backtest + shadow book use).
    static std::string sym_daily_csv_(const std::string& sym){
        if(sym=="ndx") return crypto::ndx_csv();
        std::string u; for(char ch:sym) u+=(char)std::toupper((unsigned char)ch);
        return crypto::csv_dir()+"/"+u+"_1d.csv";   // e.g. BTCUSDT_1d.csv
    }

    // Warm a slot's strat from its daily CSV. Format is time,open,high,low,close
    // (crypto = header + ms ts; NDX = headerless + sec ts) -- both carry OHLC in
    // cols 1-4, so a header line simply fails the numeric parse and is skipped.
    bool warm_from_csv_(Slot& sl){
        const std::string path=sym_daily_csv_(sl.sym);
        std::ifstream f(path);
        if(!f){ std::fprintf(stderr,"[IBKRCRYPTO] WARM-CSV missing %s (%s)\n",path.c_str(),sl.sym.c_str()); return false; }
        std::string line; int rows=0;
        while(std::getline(f,line)){
            if(line.empty()) continue;
            std::stringstream ss(line); std::string tok; double v[5]; int col=0; bool ok=true;
            while(std::getline(ss,tok,',')){
                if(col<5){ char* e=nullptr; v[col]=std::strtod(tok.c_str(),&e); if(e==tok.c_str()){ ok=false; break; } }
                ++col;
            }
            if(!ok||col<5) continue;
            sl.strat.on_daily_bar(ibkrcrypto::Bar{ v[1], v[2], v[3], v[4] });
            sl.last_close=v[4]; ++rows;
        }
        std::printf("[IBKRCRYPTO] WARM-CSV %s <- %s rows=%d warm=%d last_close=%.2f\n",
                    sl.sym.c_str(),path.c_str(),rows,(int)sl.strat.warm(),sl.last_close);
        return sl.strat.warm();
    }

    void resolve_and_subscribe_(){
        // Phase 1: resolve each SQF's front conId via reqContractDetails (see contractDetails*).
        // Warm (from local daily CSV) + placeOrder are issued from contractDetailsEnd on the
        // qualified contract. SQF HMDS has no usable daily history -> CSV is the warm source.
        for(size_t i=0;i<slots_.size();++i){
            int cdr=2000+(int)i; cd_rid_to_slot_[cdr]=(int)i;
            cli_->reqContractDetails(cdr, sqf_contract(*slots_[i].sc));
        }
        std::printf("[IBKRCRYPTO] resolving %zu SQF contracts (reqContractDetails) SHADOW=%d account=$%.0f USD (NZ$5000 @ 0.584)\n",
                    slots_.size(),!live_,account_usd());
    }

    // Route ALL targets in one pass (called once every contractDetails has returned).
    // Targets MIRROR the validated shadow book EXACTLY: tgt_net_ is the net signed SQF
    // contract count per conId taken straight from state.json's `contracts` field
    // (load_state_targets_). NO independent re-sizing -- the engine is a pure executor of
    // the shadow book, so the live position is bar-for-bar the shadow book's. An
    // independent per-leg pool sizer was tried and diverged (ETH -> 3 vs shadow's 1)
    // because this engine's roster differs from shadow_refresh (missing btc_roc/sol_roc +
    // SOL legs, extra btc_ibs; cannot reproduce n_open / corr-downsize). The vendored
    // equity RiskManager is retained only as a future catastrophe hook -- never sizes here.
    void route_(){
        std::unordered_map<std::string,int> net = tgt_net_;   // authoritative: shadow state.json
        std::unordered_map<std::string,Contract> con;         // resolved front contract per symbol
        for(auto& s:slots_){ if(s.has_con) con[s.sc->ib_symbol]=s.resolved; }
        // include currently-held roster symbols with no target so they get CLOSED to flat
        for(auto& kv:cur_pos_) net.emplace(kv.first,0);
        std::printf("[IBKRCRYPTO] routing: %zu target sym(s) live=%d\n", tgt_net_.size(), live_);
        // one netted MKT order per SQF conId: delta = net_target - current_position (idempotent)
        std::ofstream led(crypto::data_dir()+"/daily_ledger.csv",std::ios::app);
        for(auto& kv:net){
            const std::string& sym=kv.first; int tgt=kv.second;
            int cur=(int)std::llround(cur_pos_.count(sym)?cur_pos_[sym]:0.0);
            int delta=tgt-cur;
            double mark = tgt_px_.count(sym)?tgt_px_[sym]:0.0;   // shadow entry mark (ledger only; MKT order needs no px)
            led<<sym<<","<<tgt<<","<<cur<<","<<delta<<","<<mark<<","<<(live_?"LIVE":"SHADOW")<<"\n";
            std::printf("[IBKRCRYPTO][%s] %-4s net_target=%+d cur=%+d delta=%+d px=%.2f\n",
                        live_?"LIVE":"SHADOW", sym.c_str(), tgt, cur, delta, mark);
            if(delta==0) continue;
            if(!con.count(sym)){
                std::fprintf(stderr,"[IBKRCRYPTO][%s] WARN %s target %+d (delta %+d) but no resolved contract -> cannot route\n",
                             live_?"LIVE":"SHADOW", sym.c_str(), tgt, delta);
                continue;
            }
            if(live_){
                Order o; o.action=delta>0?"BUY":"SELL"; o.orderType="MKT";
                o.totalQuantity=DecimalFunctions::doubleToDecimal((double)std::abs(delta));
                cli_->placeOrder(nextId_++, con[sym], o);
                std::printf("[IBKRCRYPTO][LIVE] ORDER %s %s %d @ MKT (conId=%ld)\n",
                            o.action.c_str(), sym.c_str(), std::abs(delta), (long)con[sym].conId);
            }
        }
        write_state_();
        std::printf("[IBKRCRYPTO] route complete\n");
    }
    void write_state_(){
        // MUST NOT be data_dir()/state.json -- that file is the daily-book GUI state
        // owned by shadow_refresh + live_mark (rich schema w/ live_mark_ts + per-slot
        // px). This engine writes a compact {sym,pos} schema with no timestamp, so
        // clobbering the shared file makes the GUI show "no timestamps -> STALE" and
        // undefined px. Write to a private file instead (env-overridable at cutover
        // if/when the engine becomes the GUI's designated daily producer).
        // History: a 2026-07-01 shadow verification run wrote the shared state.json
        // and tripped the daily-book staleness alarm.
        const std::string sp = crypto::env_or("IBKRCRYPTO_ENGINE_STATE",
                                               crypto::data_dir()+"/engine_state.json");
        std::ofstream st(sp);
        st<<"{\"engine\":\"IBKRCrypto\",\"mode\":\""<<(live_?"LIVE":"SHADOW")<<"\",\"targets\":[";
        bool first=true;
        for(auto& kv:tgt_net_){ if(!first)st<<","; first=false;
            st<<"{\"sym\":\""<<kv.first<<"\",\"net\":"<<kv.second<<"}"; }
        st<<"]}";
    }

    bool live_; bool flatten_; bool probe_=false; OrderId nextId_=0;
    EReaderOSSignal sig_{2000};
    std::unique_ptr<EClientSocket> cli_; std::unique_ptr<EReader> rd_;
    std::vector<Slot> slots_; std::unordered_map<int,int> rid_to_slot_;
    std::unordered_map<int,int> cd_rid_to_slot_;   // reqContractDetails reqId -> slot
    std::set<std::string> crypto_ib_syms_;   // roster scope for panic flatten
    RiskManager risk_;                        // retained as a future catastrophe hook (sizing now mirrors the shadow book)
    bool syncing_=false;                      // reqPositions sync in flight (pre-route)
    int  cd_ends_=0;                          // contractDetailsEnd count -> route when == slots_
    std::unordered_map<std::string,double> cur_pos_;  // current IB net position per SQF ib_symbol
    std::unordered_map<std::string,int> tgt_net_;     // net signed SQF contracts from validated state.json (authoritative target)
    std::unordered_map<std::string,double> tgt_px_;   // per-symbol entry mark from state.json (ledger only)
};

int main(int argc,char**argv){
    setvbuf(stdout,nullptr,_IONBF,0);
    int port=4002; bool live=false; bool flatten=false; bool probe=false;
    for(int i=1;i<argc;++i){
        if(!strcmp(argv[i],"--live")) live=true;
        else if(!strcmp(argv[i],"--flatten")) flatten=true;
        else if(!strcmp(argv[i],"--probe-crypto")) probe=true;
        else port=atoi(argv[i]);
    }
    IbkrCryptoEngine e(live,flatten,probe);
    if(!e.connect("127.0.0.1",port,88)){ std::printf("connect failed\n"); return 1; }
    for(int i=0;i<2000;++i) e.pump();
    e.cli()->eDisconnect(); std::printf("[IBKRCRYPTO] session end\n"); return 0;
}

#else  // !OMEGA_WITH_IBKR  -- CSV/dev build: signal core is still testable
int main(){ std::printf("IbkrCryptoEngine: build with -DOMEGA_WITH_IBKR + TWS API to run live.\n"); return 0; }
#endif
