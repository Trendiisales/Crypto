// core_trigger_maker_bt.cpp — MAKER-ONLY fill-realism variant of core_trigger_p2_bt.cpp
// ---------------------------------------------------------------------------------
// Purpose (handoff 2026-07-15p FORK, branch 2): the ETH+XRP CORE edge was validated
// under TAKER economics (marketable-limit entry + MARKET exit, real depth cost 28bp).
// Operator asked "surely we are maker only". Code says the live path is TAKER, but
// IF the desk goes maker-only the taker result does NOT transfer, because the CORE
// entry (line: b.c[i] > peak) is a momentum breakout re-acceleration — a resting
// post-only bid gets LEFT BEHIND on a clean breakout (miss the winner) or is only
// hit when the breakout fails (adverse selection). This harness quantifies that.
//
// MAKER MODEL (honest, conservative):
//   * Entry = resting post-only LIMIT bid placed AFTER the signal-bar close.
//       - fills ONLY if a later bar low <= bid within CT_MAKER_WAIT bars (price revisits)
//       - if never revisited within the window -> NO TRADE (counted as a MISS)
//       - entry slippage = 0 (you set the price); depth entry-slip zeroed in cost
//   * Exit  = TAKER (MARKET), same as the live execute() path even in a maker world
//       (maker exits won't fill in a fast reversal -> taker fallback). Full depth
//       exit-slip retained. This is the realistic asymmetry: passive in, aggressive out.
//   * Fee: CP_FEE_RT still applies (Binance spot maker==taker 10bp/side at base tier;
//       the fee win only appears with BNB/VIP — test CP_FEE_RT=15 as an optimistic case).
//
// CT_MAKER_MODE:  0 = TAKER baseline (identical to p2; self-check reproduces +2120/+2005)
//                 1 = maker CHASE   (bid at the rebreak close b.c[i]  — naive "same entry, passive")
//                 2 = maker RETEST  (bid at the breakout level rhi    — old-resistance-as-support)
// CT_MAKER_WAIT:  bars to wait for a fill before cancel (default 8 = 2h on 15m)
//
// Everything else (trigger, gates, exit, depth cost, null) mirrors core_trigger_p2_bt.cpp
// variable-for-variable so the only difference measured is the maker fill realism.
// GATE unchanged: net>0 & PF>=1.3 & WF-H1>0 & WF-H2>0  AND  2x-cost net>0 & PF>=1.3

#include "/Users/jo/ChimeraCrypto/include/core/DepthLiquidationModel.hpp"
#include <cstdio>
#include <cstdlib>
#include <cmath>
#include <cstdint>
#include <fstream>
#include <sstream>
#include <string>
#include <vector>
#include <algorithm>

using chimera::DepthBook;
using chimera::DepthSnapshot;

static double envd(const char* k,double d){ const char* v=getenv(k); return v?atof(v):d; }
static int    envi(const char* k,int d){ const char* v=getenv(k); return v?atoi(v):d; }

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
        if(i>=W){ double sma=sum/W; up[i]=(b.c[i]>sma)?1:0; } }
    return up;
}
static std::vector<double> atr_bps(const Bars15& b,int P=14){
    std::vector<double> a(b.N,0); double sum=0; std::vector<double> tr(b.N,0);
    for(int i=0;i<b.N;++i){ double pc=(i>0)?b.c[i-1]:b.c[i];
        double t=std::max(b.h[i]-b.l[i],std::max(std::fabs(b.h[i]-pc),std::fabs(b.l[i]-pc)));
        tr[i]=t; sum+=t; if(i>=P) sum-=tr[i-P];
        if(i>=P&&b.c[i]>0) a[i]=(sum/P)/b.c[i]*1e4; }
    return a;
}

struct Trade { int64_t ts; double net_bps; };

struct Params {
    double comp_w, short_thr, med_thr, move_mult, pb_maxfrac, safe_ref, trailmin, trailatr, stopbuf;
    int comp_bars, short_w, med_w, w1, w4, maxwait, cooldown, btc_gate, room_h;
    // Phase-2 cost
    double qusd, fee_rt, reserve; DepthBook* depth=nullptr; double cost_mult=1.0;
    long* n_fb=nullptr; long* n_tot=nullptr; // fallback / total trade counters
    // MAKER extension
    int maker_mode=0;       // 0 taker, 1 chase, 2 retest
    int maker_wait=8;       // bars to wait for a resting-bid fill
    long* n_miss=nullptr;   // signals that never filled (winner-miss counter)
};

// Per-trade realized depth-adjusted cost (bps). In maker mode the ENTRY slip is 0
// (passive fill at your own price); the EXIT slip is full taker depth slip.
static double trade_cost(const Params& p,int64_t entry_ts,int64_t exit_ts){
    if(p.n_tot)(*p.n_tot)++;
    if(!p.depth){ if(p.n_fb)(*p.n_fb)++; return p.safe_ref*p.cost_mult; }
    const DepthSnapshot* se=p.depth->nearest(entry_ts);
    const DepthSnapshot* sx=p.depth->nearest(exit_ts);
    if(!se||!sx){ if(p.n_fb)(*p.n_fb)++; return p.safe_ref*p.cost_mult; }
    double eslip = (p.maker_mode>0) ? 0.0 : se->buy_slip_bps(p.qusd/se->mid); // maker: 0 entry slip
    double xslip = sx->sell_slip_bps(p.qusd/sx->mid);                        // exit always taker
    return (p.fee_rt + eslip + xslip + p.reserve)*p.cost_mult;
}

static std::vector<Trade> run_core(const Bars15& b,const Params& p,
                                   const std::vector<char>* btc_up1,const std::vector<char>* btc_up4){
    std::vector<Trade> tr;
    auto up1=trend_up(b,p.w1); auto up4=trend_up(b,p.w4); auto atr=atr_bps(b);
    int warm=std::max(p.w4,p.comp_bars)+2;
    enum St{SCAN,COMP,BROKE,PEND,INPOS}; St st=SCAN;
    double rhi=0,rlo=0,anchor_px=0; double vwap_num=0,vwap_den=0;
    double impulse=0,peak=0,pb_low=0; int brk_i=0; bool pulled=false;
    double entry=0,stop=0,ppeak=0; int cool_until=-1; int entry_i=0;
    double bid_px=0,pend_stop=0; int pend_start=0;   // maker pending-fill state
    for(int i=warm;i<b.N;++i){
        if(st==COMP||st==BROKE){ double tp=(b.h[i]+b.l[i]+b.c[i])/3.0; vwap_num+=tp; vwap_den+=1; }
        double vwap = vwap_den>0? vwap_num/vwap_den : b.c[i];
        if(st==SCAN){
            if(i<=cool_until) continue;
            int a=i-p.comp_bars+1; double mh=-1e18,ml=1e18;
            for(int j=a;j<=i;++j){ mh=std::max(mh,b.h[j]); ml=std::min(ml,b.l[j]); }
            double mid=(mh+ml)/2.0; double w=mid>0?(mh-ml)/mid*1e4:1e9;
            if(w<=p.comp_w){ rhi=mh; rlo=ml; anchor_px=mid; st=COMP; vwap_num=0; vwap_den=0; }
        }
        else if(st==COMP){
            double sh=0; { int a=std::max(0,i-p.short_w+1),n=0; for(int j=a;j<=i;++j){sh+=b.tbb_frac[j];n++;} sh/= (n>0?n:1);}
            double md=0; { int a=std::max(0,i-p.med_w+1),n=0; for(int j=a;j<=i;++j){md+=b.tbb_frac[j];n++;} md/=(n>0?n:1);}
            if(b.c[i]>rhi && sh>=p.short_thr && md>=p.med_thr){
                impulse=(b.c[i]-rlo)/rlo*1e4;
                st=BROKE; brk_i=i; peak=b.h[i]; pb_low=b.l[i]; pulled=false;
            } else if(b.l[i] < rlo){ st=SCAN; }
        }
        else if(st==BROKE){
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
                    bool g_btc = (!p.btc_gate) || (btc_up1&&btc_up4 ? (i<(int)btc_up1->size() && (*btc_up1)[i] && (*btc_up4)[i]) : true);
                    bool g_hl = pb_low > rhi;
                    bool g_room = atr[i]*p.room_h >= p.move_mult*p.safe_ref;
                    if(g_up&&g_vwap&&g_btc&&g_hl&&g_room){
                        if(p.maker_mode==0){                 // TAKER: fill at signal close
                            st=INPOS; entry=b.c[i]; ppeak=b.c[i]; entry_i=i;
                            stop=pb_low*(1.0 - p.stopbuf/1e4);
                        } else {                             // MAKER: place resting post-only bid
                            bid_px = (p.maker_mode==2) ? rhi : b.c[i]; // 2=retest level, 1=chase close
                            pend_stop = pb_low*(1.0 - p.stopbuf/1e4);
                            pend_start = i; st=PEND;
                        }
                    } else { st=SCAN; }
                }
            }
        }
        else if(st==PEND){
            // resting post-only bid: fill only if price revisits bid within the window
            if(i-pend_start > p.maker_wait){ if(p.n_miss)(*p.n_miss)++; st=SCAN; cool_until=i+p.cooldown; continue; }
            if(b.l[i] <= bid_px){
                st=INPOS; entry=bid_px; ppeak=std::max(bid_px,b.h[i]); entry_i=i;
                stop=pend_stop;
            }
        }
        else if(st==INPOS){
            ppeak=std::max(ppeak,b.h[i]);
            double vw = vwap;
            double trail=std::max(p.trailmin, p.trailatr*atr[i]);
            double exit_px=0; bool ex=false;
            if(b.l[i] <= stop){ exit_px=stop; ex=true; }
            else { double gb=(ppeak-b.c[i])/ppeak*1e4;
                   if(gb>=trail){ exit_px=b.c[i]; ex=true; }
                   else if(b.c[i] < vw){ exit_px=b.c[i]; ex=true; } }
            if(ex){
                int64_t ets=b.ts[entry_i]+900000, xts=b.ts[i]+900000; // bar-close times
                double cost=trade_cost(p,ets,xts);
                double net=(exit_px-entry)/entry*1e4 - cost;
                tr.push_back({b.ts[i],net}); st=SCAN; cool_until=i+p.cooldown;
                vwap_num=0; vwap_den=0; }
        }
    }
    return tr;
}

// NULL model: random entry in the same regime-up candidate set, identical exit +
// identical (maker) per-trade depth cost. Edge is structural iff real net >> null.
static double run_null(const Bars15& b,const Params& p,int n_target,unsigned seed,
                       const std::vector<char>* btc_up1,const std::vector<char>* btc_up4){
    auto up1=trend_up(b,p.w1); auto up4=trend_up(b,p.w4); auto atr=atr_bps(b);
    std::vector<int> cand; int warm=std::max(p.w4,p.comp_bars)+2;
    for(int i=warm;i<b.N;++i){ bool gb=(!p.btc_gate)||(btc_up1&&btc_up4? (i<(int)btc_up1->size()&&(*btc_up1)[i]&&(*btc_up4)[i]):true);
        if(up1[i]&&up4[i]&&gb) cand.push_back(i); }
    if((int)cand.size()<n_target||n_target<=0) return 0;
    srand(seed); double net=0; int taken=0, guard=0;
    std::vector<char> used(b.N,0);
    while(taken<n_target && guard++<n_target*50){
        int i=cand[rand()%cand.size()]; if(used[i]||i+1>=b.N) continue; used[i]=1;
        double entry=b.c[i], ppeak=b.c[i], stop=b.l[i]*(1.0-p.stopbuf/1e4); taken++; int ei=i;
        for(int j=i+1;j<b.N;++j){ ppeak=std::max(ppeak,b.h[j]);
            double trail=std::max(p.trailmin,p.trailatr*atr[j]); double ex=0; bool done=false;
            if(b.l[j]<=stop){ex=stop;done=true;} else{ double gb=(ppeak-b.c[j])/ppeak*1e4;
                if(gb>=trail){ex=b.c[j];done=true;} }
            if(done){ int64_t ets=b.ts[ei]+900000,xts=b.ts[j]+900000;
                Params pp=p; pp.n_tot=nullptr; pp.n_fb=nullptr;
                net+=(ex-entry)/entry*1e4-trade_cost(pp,ets,xts); break; } }
    }
    return net;
}

struct Metrics{ int n; double net,pf,worst,h1,h2; int nwin; };
static Metrics metrics(const std::vector<Trade>& tr){
    Metrics m{0,0,0,0,0,0,0}; if(tr.empty()) return m;
    double gw=0,gl=0; std::vector<Trade> s=tr;
    std::sort(s.begin(),s.end(),[](const Trade&a,const Trade&b){return a.ts<b.ts;});
    for(size_t k=0;k<s.size();++k){ double x=s[k].net_bps; m.net+=x;
        if(x>0){gw+=x;m.nwin++;} else gl-=x; if(x<m.worst)m.worst=x;
        if(k<s.size()/2) m.h1+=x; else m.h2+=x; }
    m.n=(int)s.size(); m.pf = gl>0? gw/gl : (gw>0?999:0); return m;
}

int main(){
    std::string dir = getenv("CT_DATA_DIR")?getenv("CT_DATA_DIR"):"/Users/jo/ChimeraCrypto/data/klines_spot";
    std::string ddir = getenv("CP_DEPTH_DIR")?getenv("CP_DEPTH_DIR"):"/Users/jo/ChimeraCrypto/data/bookdepth_perp";
    Params p;
    p.comp_w=envd("CT_COMP_W",80); p.comp_bars=envi("CT_COMP_BARS",6);
    p.short_thr=envd("CT_SHORT_THR",0.64); p.short_w=envi("CT_SHORT_W",3);   // handoff passing cfg
    p.med_thr=envd("CT_MED_THR",0.56); p.med_w=envi("CT_MED_W",8);
    p.move_mult=envd("CT_MOVE_MULT",3.0); p.pb_maxfrac=envd("CT_PB_MAXFRAC",0.50);
    p.w1=envi("CT_W1",16); p.w4=envi("CT_W4",64);
    p.safe_ref=envd("CP_SAFEREF",30); p.trailmin=envd("CT_TRAILMIN",240); p.trailatr=envd("CT_TRAILATR",0.5); // trail 240
    p.stopbuf=envd("CT_STOPBUF",5); p.maxwait=envi("CT_MAXWAIT",24); p.cooldown=envi("CT_COOLDOWN",8);
    p.btc_gate=envi("CT_BTC_GATE",1); p.room_h=envi("CT_ROOM_H",8);
    p.qusd=envd("CP_QUSD",100000); p.fee_rt=envd("CP_FEE_RT",20); p.reserve=envd("CP_RESERVE",8);
    p.cost_mult=1.0;
    p.maker_mode=envi("CT_MAKER_MODE",1); p.maker_wait=envi("CT_MAKER_WAIT",8);

    Bars15 btc=load15(dir,"BTC"); if(!btc.N) return 1;
    auto btc_up1=trend_up(btc,p.w1); auto btc_up4=trend_up(btc,p.w4);

    std::vector<std::string> coins;
    { const char* cc=getenv("CP_COINS"); std::string cl=cc?cc:"ETH,XRP";
      std::stringstream ss(cl); std::string t; while(std::getline(ss,t,',')) if(!t.empty()) coins.push_back(t); }
    int randz=envi("CT_RANDZ",0);

    auto load_depth=[&](const std::string& coin)->DepthBook*{
        DepthBook* bk=new DepthBook(); size_t n=bk->load_dir(ddir,coin+"USDT");
        if(n==0){ std::fprintf(stderr,"[warn] no depth for %s in %s -> flat fallback\n",coin.c_str(),ddir.c_str()); delete bk; return nullptr; }
        return bk; };

    const char* mm = p.maker_mode==0?"TAKER-baseline":(p.maker_mode==2?"MAKER-retest(bid@rhi)":"MAKER-chase(bid@close)");

    if(randz>0){
        std::printf("RANDZ null-test (%d seeds) mode=%s wait=%d Q=$%.0f\n",randz,mm,p.maker_wait,p.qusd);
        for(auto cs:coins){ Bars15 b=(cs=="BTC")?btc:load15(dir,cs); if(!b.N) continue;
            Params pc=p; pc.depth=load_depth(cs); long miss=0; pc.n_miss=&miss;
            auto tr=run_core(b,pc,&btc_up1,&btc_up4); auto m=metrics(tr);
            std::vector<double> nulls; for(int s=0;s<randz;++s) nulls.push_back(run_null(b,pc,m.n,1000+s,&btc_up1,&btc_up4));
            std::sort(nulls.begin(),nulls.end());
            double nmean=0; for(double x:nulls)nmean+=x; nmean/= (nulls.empty()?1:nulls.size());
            double p95=nulls.empty()?0:nulls[(int)(0.95*nulls.size())]; double p05=nulls.empty()?0:nulls[(int)(0.05*nulls.size())];
            int beat=0; for(double x:nulls) if(m.net>x) beat++;
            std::printf("%-4s real net=%+.0f (n=%d miss=%ld) | null mean=%+.0f p05=%+.0f p95=%+.0f | %.0f%%ile %s\n",
                cs.c_str(),m.net,m.n,miss,nmean,p05,p95,100.0*beat/randz, (m.net>p95?"EDGE":"within-null"));
        }
        return 0;
    }

    std::printf("CORE-TRIGGER MAKER | mode=%s wait=%d | Q=$%.0f fee_rt=%.0f reserve=%.0f | short=%.2f trail=max(%.0f,%.1f*atr)\n",
        mm,p.maker_wait,p.qusd,p.fee_rt,p.reserve,p.short_thr,p.trailmin,p.trailatr);
    std::printf("%-5s | %5s %5s %6s %9s %6s %8s | %9s %9s | %s\n","coin","n","miss","win%","net_bp","PF","worst","WF-H1","WF-H2","GATE(base;2x)");
    bool all_pass=true;
    for(auto cs:coins){ Bars15 b=(cs=="BTC")?btc:load15(dir,cs); if(!b.N){all_pass=false;continue;}
        DepthBook* bk=load_depth(cs);
        long nfb=0,ntot=0,miss=0;
        Params pb=p; pb.depth=bk; pb.cost_mult=1.0; pb.n_fb=&nfb; pb.n_tot=&ntot; pb.n_miss=&miss;
        Params p2=p; p2.depth=bk; p2.cost_mult=2.0;
        auto trb=run_core(b,pb,&btc_up1,&btc_up4);
        auto tr2=run_core(b,p2,&btc_up1,&btc_up4);
        auto m=metrics(trb); auto m2=metrics(tr2);
        bool base = m.net>0&&m.pf>=1.3&&m.h1>0&&m.h2>0;
        bool x2   = m2.net>0&&m2.pf>=1.3;
        bool pass = base&&x2; all_pass&=pass;
        std::printf("%-5s | %5d %5ld %5.1f %+9.0f %6.2f %+8.0f | %+9.0f %+9.0f | %s [2x net%+.0f pf%.2f]\n",
            cs.c_str(),m.n,miss, m.n>0?100.0*m.nwin/m.n:0, m.net,m.pf,m.worst,m.h1,m.h2, pass?"PASS":"fail", m2.net,m2.pf);
    }
    std::printf("MAKER VERDICT (%s): %s\n", mm,
        all_pass?"maker edge SURVIVES -> maker-only is viable":"maker edge FAILS -> taker-RT (28bp) is the only validated basis");
    return 0;
}
