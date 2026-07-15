// parity_core_trigger.cpp — PARITY GATE for the live CoreTriggerEngine.
// ---------------------------------------------------------------------
// Feeds the SAME 15m bars (BTC regime + ETH + XRP) through:
//   (A) a verbatim copy of run_core (the backtest reference, core_trigger_p2_bt.cpp)
//   (B) the live streaming chimera::CoreTriggerEngine (ingest_bar, taker path)
// and asserts an IDENTICAL per-trade sequence (entry_ts, exit_ts, gross_bp). Deploy is
// gated on this printing "PARITY OK". Cost is deterministic downstream of the trade
// decisions, so parity is judged on gross + entry/exit timestamps (not the cost model).
//
// Build: g++ -O2 -std=c++17 -I/Users/jo/ChimeraCrypto/include -I/Users/jo/ChimeraCrypto/include/core \
//        -o /tmp/ct_parity backtest/parity_core_trigger.cpp
// Run:   CT_DATA_DIR=/Users/jo/ChimeraCrypto/data/klines_spot /tmp/ct_parity

#include "core/CoreTriggerEngine.hpp"
#include <cstdio>
#include <cstdlib>
#include <cmath>
#include <cstdint>
#include <fstream>
#include <sstream>
#include <string>
#include <vector>
#include <algorithm>

// ------------------------------ reference (verbatim run_core) ------------------------------
struct Bars15 { std::vector<int64_t> ts; std::vector<double> o,h,l,c,tbb_frac; int N=0; };
static std::vector<std::string> split(const std::string& s){
    std::vector<std::string> v; v.reserve(12); std::stringstream ss(s); std::string t;
    while(std::getline(ss,t,',')) v.push_back(t); return v; }
static Bars15 load15(const std::string& dir,const std::string& coin){
    Bars15 b; std::ifstream f(dir+"/"+coin+"USDT_1m.csv");
    if(!f){ std::fprintf(stderr,"no data for %s in %s\n",coin.c_str(),dir.c_str()); return b; }
    std::string ln; std::getline(f,ln);
    int cnt=0; double O=0,H=0,L=0,C=0,V=0,TBB=0; int64_t bts=0;
    auto flush=[&](){ if(cnt>0){ b.ts.push_back(bts); b.o.push_back(O); b.h.push_back(H);
        b.l.push_back(L); b.c.push_back(C); b.tbb_frac.push_back(V>0?TBB/V:0.5); } };
    while(std::getline(f,ln)){
        auto v=split(ln); if(v.size()<10) continue;
        int64_t ts=(int64_t)std::stoll(v[0]);
        double o=std::stod(v[1]),h=std::stod(v[2]),l=std::stod(v[3]),c=std::stod(v[4]);
        double vol=std::stod(v[5]),tbb=std::stod(v[9]);
        int64_t slot = ts - (ts % (15LL*60*1000));
        if(cnt==0 || slot!=bts){ flush(); bts=slot; O=o; H=h; L=l; C=c; V=vol; TBB=tbb; cnt=1; }
        else { H=std::max(H,h); L=std::min(L,l); C=c; V+=vol; TBB+=tbb; cnt++; }
    }
    flush(); b.N=(int)b.ts.size(); return b;
}
static std::vector<char> trend_up(const Bars15& b,int W){
    std::vector<char> up(b.N,0); double sum=0;
    for(int i=0;i<b.N;++i){ sum+=b.c[i]; if(i>=W) sum-=b.c[i-W];
        if(i>=W){ double sma=sum/W; up[i]=(b.c[i]>sma)?1:0; } } return up; }
static std::vector<double> atr_bps(const Bars15& b,int P=14){
    std::vector<double> a(b.N,0); double sum=0; std::vector<double> tr(b.N,0);
    for(int i=0;i<b.N;++i){ double pc=(i>0)?b.c[i-1]:b.c[i];
        double t=std::max(b.h[i]-b.l[i],std::max(std::fabs(b.h[i]-pc),std::fabs(b.l[i]-pc)));
        tr[i]=t; sum+=t; if(i>=P) sum-=tr[i-P];
        if(i>=P&&b.c[i]>0) a[i]=(sum/P)/b.c[i]*1e4; } return a; }

struct RefP { double comp_w=80,short_thr=0.64,med_thr=0.56,move_mult=3.0,pb_maxfrac=0.50,safe_ref=30,
              trailmin=240,trailatr=0.5,stopbuf=5; int comp_bars=6,short_w=3,med_w=8,w1=16,w4=64,
              maxwait=24,cooldown=8,btc_gate=1,room_h=8; };
struct RefTrade { int64_t entry_ts, exit_ts; double gross_bp; };

static std::vector<RefTrade> run_core_ref(const Bars15& b,const RefP& p,
        const std::vector<char>& btc_up1,const std::vector<char>& btc_up4){
    std::vector<RefTrade> tr;
    auto up1=trend_up(b,p.w1); auto up4=trend_up(b,p.w4); auto atr=atr_bps(b);
    int warm=std::max(p.w4,p.comp_bars)+2;
    enum St{SCAN,COMP,BROKE,INPOS}; St st=SCAN;
    double rhi=0,rlo=0,vwap_num=0,vwap_den=0;
    double impulse=0,peak=0,pb_low=0; int brk_i=0; bool pulled=false;
    double entry=0,stop=0,ppeak=0; int cool_until=-1; int entry_i=0;
    for(int i=warm;i<b.N;++i){
        if(st==COMP||st==BROKE){ double tp=(b.h[i]+b.l[i]+b.c[i])/3.0; vwap_num+=tp; vwap_den+=1; }
        double vwap = vwap_den>0? vwap_num/vwap_den : b.c[i];
        if(st==SCAN){
            if(i<=cool_until) continue;
            int a=i-p.comp_bars+1; double mh=-1e18,ml=1e18;
            for(int j=a;j<=i;++j){ mh=std::max(mh,b.h[j]); ml=std::min(ml,b.l[j]); }
            double mid=(mh+ml)/2.0; double w=mid>0?(mh-ml)/mid*1e4:1e9;
            if(w<=p.comp_w){ rhi=mh; rlo=ml; st=COMP; vwap_num=0; vwap_den=0; }
        } else if(st==COMP){
            double sh=0; { int a=std::max(0,i-p.short_w+1),n=0; for(int j=a;j<=i;++j){sh+=b.tbb_frac[j];n++;} sh/=(n>0?n:1);}
            double md=0; { int a=std::max(0,i-p.med_w+1),n=0; for(int j=a;j<=i;++j){md+=b.tbb_frac[j];n++;} md/=(n>0?n:1);}
            if(b.c[i]>rhi && sh>=p.short_thr && md>=p.med_thr){
                impulse=(b.c[i]-rlo)/rlo*1e4; st=BROKE; brk_i=i; peak=b.h[i]; pb_low=b.l[i]; pulled=false;
            } else if(b.l[i] < rlo){ st=SCAN; }
        } else if(st==BROKE){
            if(i-brk_i>p.maxwait){ st=SCAN; continue; }
            if(!pulled) peak=std::max(peak,b.h[i]);
            double hold = std::max(rhi,vwap);
            if(b.l[i] < hold){ st=SCAN; continue; }
            double giveback_bps = (peak-b.c[i])/peak*1e4;
            if(!pulled){ if(giveback_bps >= std::max(15.0,0.25*impulse)){ pulled=true; pb_low=b.l[i]; } }
            else { pb_low=std::min(pb_low,b.l[i]);
                double pbdepth=(peak-pb_low)/peak*1e4;
                if(pbdepth > p.pb_maxfrac*impulse + p.safe_ref){ st=SCAN; continue; }
                if(b.c[i] > peak){
                    bool g_up = up1[i] && up4[i];
                    bool g_vwap = b.c[i] > vwap;
                    bool g_btc = (!p.btc_gate) || (i<(int)btc_up1.size() && btc_up1[i] && btc_up4[i]);
                    bool g_hl = pb_low > rhi;
                    bool g_room = atr[i]*p.room_h >= p.move_mult*p.safe_ref;
                    if(g_up&&g_vwap&&g_btc&&g_hl&&g_room){
                        st=INPOS; entry=b.c[i]; ppeak=b.c[i]; entry_i=i; stop=pb_low*(1.0 - p.stopbuf/1e4);
                    } else { st=SCAN; }
                }
            }
        } else if(st==INPOS){
            ppeak=std::max(ppeak,b.h[i]);
            double trail=std::max(p.trailmin, p.trailatr*atr[i]);
            double exit_px=0; bool ex=false;
            if(b.l[i] <= stop){ exit_px=stop; ex=true; }
            else { double gb=(ppeak-b.c[i])/ppeak*1e4;
                   if(gb>=trail){ exit_px=b.c[i]; ex=true; }
                   else if(b.c[i] < vwap){ exit_px=b.c[i]; ex=true; } }
            if(ex){ double gross=(exit_px-entry)/entry*1e4;
                tr.push_back({b.ts[entry_i], b.ts[i], gross}); st=SCAN; cool_until=i+p.cooldown;
                vwap_num=0; vwap_den=0; }
        }
    }
    return tr;
}

// ------------------------------ live-engine driver ------------------------------
int main(){
    using namespace chimera;
    std::string dir = getenv("CT_DATA_DIR")?getenv("CT_DATA_DIR"):"/Users/jo/ChimeraCrypto/data/klines_spot";
    Bars15 btc=load15(dir,"BTC"), eth=load15(dir,"ETH"), xrp=load15(dir,"XRP");
    if(!btc.N||!eth.N||!xrp.N){ std::fprintf(stderr,"missing data\n"); return 1; }

    RefP rp; auto btc_up1=trend_up(btc,rp.w1); auto btc_up4=trend_up(btc,rp.w4);
    auto ref_eth=run_core_ref(eth,rp,btc_up1,btc_up4);
    auto ref_xrp=run_core_ref(xrp,rp,btc_up1,btc_up4);

    // live engine: feed the identical bars merged in slot order (BTC first within a slot)
    CoreTriggerEngine::Config cfg; cfg.btc_symbol="btcusdt"; cfg.tf_secs=900;
    CoreTriggerEngine::Cell ce; ce.symbol="ethusdt"; ce.tag="CORE-ETH";
    CoreTriggerEngine::Cell cx; cx.symbol="xrpusdt"; cx.tag="CORE-XRP";
    cfg.cells={ce,cx};
    CoreTriggerEngine eng(cfg);
    std::vector<RefTrade> live_eth, live_xrp;
    eng.set_on_clip([&](const CoreTriggerEngine::ClipRecord& r){
        if(r.symbol=="ethusdt") live_eth.push_back({r.entry_ts_ms,r.exit_ts_ms,r.gross_bp});
        else if(r.symbol=="xrpusdt") live_xrp.push_back({r.entry_ts_ms,r.exit_ts_ms,r.gross_bp});
    });
    SymbolId BTC=symbol_to_id("btcusdt"), ETH=symbol_to_id("ethusdt"), XRP=symbol_to_id("xrpusdt");

    // merge by slot; feed each series in its own ascending order so engine index == csv index
    size_t bi=0,ei=0,xi=0;
    auto nextslot=[&](){ int64_t m=INT64_MAX; bool any=false;
        if(bi<btc.ts.size()){m=std::min(m,btc.ts[bi]);any=true;}
        if(ei<eth.ts.size()){m=std::min(m,eth.ts[ei]);any=true;}
        if(xi<xrp.ts.size()){m=std::min(m,xrp.ts[xi]);any=true;}
        return any?m:(int64_t)-1; };
    for(;;){ int64_t s=nextslot(); if(s<0) break;
        if(bi<btc.ts.size()&&btc.ts[bi]==s){ eng.ingest_bar(BTC,s,btc.o[bi],btc.h[bi],btc.l[bi],btc.c[bi],btc.tbb_frac[bi]); ++bi; }
        if(ei<eth.ts.size()&&eth.ts[ei]==s){ eng.ingest_bar(ETH,s,eth.o[ei],eth.h[ei],eth.l[ei],eth.c[ei],eth.tbb_frac[ei]); ++ei; }
        if(xi<xrp.ts.size()&&xrp.ts[xi]==s){ eng.ingest_bar(XRP,s,xrp.o[xi],xrp.h[xi],xrp.l[xi],xrp.c[xi],xrp.tbb_frac[xi]); ++xi; }
    }

    auto cmp=[&](const char* nm,std::vector<RefTrade>& R,std::vector<RefTrade>& L)->bool{
        bool ok=true; double rn=0,ln=0;
        std::printf("== %s == ref n=%zu live n=%zu\n",nm,R.size(),L.size());
        if(R.size()!=L.size()) ok=false;
        size_t n=std::min(R.size(),L.size());
        for(size_t k=0;k<n;++k){ rn+=R[k].gross_bp; ln+=L[k].gross_bp;
            bool m=(R[k].entry_ts==L[k].entry_ts && R[k].exit_ts==L[k].exit_ts && std::fabs(R[k].gross_bp-L[k].gross_bp)<1e-6);
            if(!m){ ok=false; std::printf("  DIFF[%zu] ref(e=%lld x=%lld g=%.4f) live(e=%lld x=%lld g=%.4f)\n",
                k,(long long)R[k].entry_ts,(long long)R[k].exit_ts,R[k].gross_bp,
                (long long)L[k].entry_ts,(long long)L[k].exit_ts,L[k].gross_bp); }
        }
        std::printf("  ref gross Σ=%.1f  live gross Σ=%.1f  net@28 ref=%.1f live=%.1f\n",
            rn,ln,rn-28.0*R.size(),ln-28.0*L.size());
        return ok; };
    bool ok = cmp("ETH",ref_eth,live_eth); ok &= cmp("XRP",ref_xrp,live_xrp);
    std::printf("%s\n", ok?"PARITY OK — live engine == backtest (deploy gate GREEN)":"PARITY FAIL — do NOT deploy");
    return ok?0:2;
}
