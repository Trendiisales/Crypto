// core_trigger_p2_bt.cpp — PHASE 2 re-validation of the CORE trigger (2026-07-15m).
// Same §3 CORE trigger as core_trigger_bt.cpp (Phase 1), but the FLAT 35bp
// safe_cost is replaced by a PER-TRADE depth-adjusted cost walked from the perp
// bookDepth ladder (DepthLiquidationModel), for a configurable campaign notional
// Q. This isolates the COST effect: orderflow is still the Phase-1 1m
// taker_buy_base proxy (tick OFI/CVD from aggTrades is the next piece).
//
// Per-trade cost (bps) = buy_fee + sell_fee                 (20 RT)
//                      + depth buy-slip  at ENTRY ts, Q     (ask ladder)
//                      + depth sell-slip at EXIT  ts, Q     (bid ladder)
//                      + reserve (spread+latency+dust)      (default 8)
// Setup-logic thresholds (room / pullback tolerance) use a scalar safe_ref
// (expected cost known at entry); realized PnL uses the realized per-trade cost.
// 2x-cost gate multiplies the whole per-trade cost by 2.
//
// Depth is PERP (spot has no free depth) — a tight-basis proxy for BTC/ETH/XRP.
// Snapshots ~30s; queried at bar-close (bar_start+15min), 5-min gap tolerance.
// Trades with no depth snapshot in range fall back to a flat cost and are counted.
//
// GATE (unchanged): net>0 & PF>=1.3 & WF-H1>0 & WF-H2>0  AND  2x-cost net>0 & PF>=1.3
//   on BOTH passing-universe coins (ETH + XRP). Long-only spot; 2022 auto-omitted.
//
// Build: clang++ -std=c++17 -O2 core_trigger_p2_bt.cpp -o core_trigger_p2_bt
//   (depth model included by absolute path; klines dir CT_DATA_DIR)
// Env (Phase-1 params carry over; new ones):
//   CP_DEPTH_DIR (/Users/jo/ChimeraCrypto/data/bookdepth_perp)
//   CP_QUSD (100000)  campaign notional walked through the book
//   CP_FEE_RT (20)  round-trip fee bps ; CP_RESERVE (8) spread+latency+dust
//   CP_SAFEREF (30) scalar safe_cost for setup-logic thresholds only
//   CP_COINS (ETH,XRP) ; CT_* Phase-1 trigger params all still honored
//   CT_RANDZ (0)  >0 = null-test mode

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
    // Operator BE-ladder + no-chase + bounded-stop (BTC_COST_AWARE_LONG_CONTINUATION spec)
    double chase_max, stop_min, stop_max;
    double lad1_mfe, lad1_floor, lad2_mfe, lad2_floor, lad3_mfe;
    // Phase-2 cost
    double qusd, fee_rt, reserve; DepthBook* depth=nullptr; double cost_mult=1.0;
    long* n_fb=nullptr; long* n_tot=nullptr; // fallback / total trade counters
};

// Per-trade realized depth-adjusted cost (bps). cost_mult scales the whole cost
// (2.0 = the 2x-cost gate). Falls back to a flat cost if no depth snapshot in range.
static double trade_cost(const Params& p,int64_t entry_ts,int64_t exit_ts){
    if(p.n_tot)(*p.n_tot)++;
    // fallback = the Phase-1 flat safe_ref constant when depth is unavailable.
    if(!p.depth){ if(p.n_fb)(*p.n_fb)++; return p.safe_ref*p.cost_mult; }
    const DepthSnapshot* se=p.depth->nearest(entry_ts);
    const DepthSnapshot* sx=p.depth->nearest(exit_ts);
    if(!se||!sx){ if(p.n_fb)(*p.n_fb)++; return p.safe_ref*p.cost_mult; }
    double eslip=se->buy_slip_bps(p.qusd/se->mid);
    double xslip=sx->sell_slip_bps(p.qusd/sx->mid);
    if(getenv("CP_DBGCOST")) std::fprintf(stderr,"[cost] ets=%lld gap=%lld emid=%.2f eslip=%.2f | xts=%lld gap=%lld xmid=%.2f xslip=%.2f -> %.1f\n",
        (long long)entry_ts,(long long)(se->ts_ms-entry_ts),se->mid,eslip,(long long)exit_ts,(long long)(sx->ts_ms-exit_ts),sx->mid,xslip,(p.fee_rt+eslip+xslip+p.reserve));
    return (p.fee_rt + eslip + xslip + p.reserve)*p.cost_mult;
}

static std::vector<Trade> run_core(const Bars15& b,const Params& p,
                                   const std::vector<char>* btc_up1,const std::vector<char>* btc_up4){
    std::vector<Trade> tr;
    auto up1=trend_up(b,p.w1); auto up4=trend_up(b,p.w4); auto atr=atr_bps(b);
    int warm=std::max(p.w4,p.comp_bars)+2;
    enum St{SCAN,COMP,BROKE,INPOS}; St st=SCAN;
    double rhi=0,rlo=0,anchor_px=0; double vwap_num=0,vwap_den=0;
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
                        double stop_px=pb_low*(1.0 - p.stopbuf/1e4);
                        double chase_bps=(b.c[i]-peak)/peak*1e4;           // entry above trigger(base high)
                        double stop_bps=(b.c[i]-stop_px)/b.c[i]*1e4;       // structural stop width
                        if(chase_bps>p.chase_max){ st=SCAN; }              // NO-CHASE: too far above trigger
                        else if(stop_bps>p.stop_max){ st=SCAN; }           // SKIP: structural stop wider than max
                        else { if(stop_bps<p.stop_min) stop_px=b.c[i]*(1.0-p.stop_min/1e4); // floor stop width
                            st=INPOS; entry=b.c[i]; ppeak=b.c[i]; entry_i=i; stop=stop_px; }
                    } else { st=SCAN; }
                }
            }
        }
        else if(st==INPOS){
            ppeak=std::max(ppeak,b.h[i]);
            double vw = vwap;
            // Net-cost BE-floor profit-lock ladder (ratchets the protective stop up)
            double mfe=(ppeak-entry)/entry*1e4;
            double floor_bps=-1e9;
            if(mfe>=p.lad2_mfe) floor_bps=p.lad2_floor;       // MFE>=+115bp -> floor +80bp gross
            else if(mfe>=p.lad1_mfe) floor_bps=p.lad1_floor;  // MFE>=+75bp  -> floor +45bp gross
            if(floor_bps>-1e8){ double fpx=entry*(1.0+floor_bps/1e4); if(fpx>stop) stop=fpx; }
            // Tighter peak-trail once deep in profit (MFE>=+150bp): max(45bp,0.5*ATR)
            double trail = (mfe>=p.lad3_mfe) ? std::max(45.0, p.trailatr*atr[i])
                                             : std::max(p.trailmin, p.trailatr*atr[i]);
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
// identical per-trade depth cost. Edge is structural iff real net >> null.
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
    p.short_thr=envd("CT_SHORT_THR",0.60); p.short_w=envi("CT_SHORT_W",3);
    p.med_thr=envd("CT_MED_THR",0.56); p.med_w=envi("CT_MED_W",8);
    p.move_mult=envd("CT_MOVE_MULT",3.0); p.pb_maxfrac=envd("CT_PB_MAXFRAC",0.50);
    p.w1=envi("CT_W1",16); p.w4=envi("CT_W4",64);
    p.safe_ref=envd("CP_SAFEREF",30); p.trailmin=envd("CT_TRAILMIN",40); p.trailatr=envd("CT_TRAILATR",0.5);
    p.stopbuf=envd("CT_STOPBUF",5); p.maxwait=envi("CT_MAXWAIT",24); p.cooldown=envi("CT_COOLDOWN",8);
    p.btc_gate=envi("CT_BTC_GATE",1); p.room_h=envi("CT_ROOM_H",8);
    p.qusd=envd("CP_QUSD",100000); p.fee_rt=envd("CP_FEE_RT",20); p.reserve=envd("CP_RESERVE",8);
    p.cost_mult=1.0;
    // Operator spec: no-chase + bounded structural stop + net-cost BE ladder
    p.chase_max=envd("CT_CHASE_MAX",12); p.stop_min=envd("CT_STOP_MIN",50); p.stop_max=envd("CT_STOP_MAX",90);
    p.lad1_mfe=envd("CT_LAD1_MFE",75); p.lad1_floor=envd("CT_LAD1_FLOOR",45);
    p.lad2_mfe=envd("CT_LAD2_MFE",115); p.lad2_floor=envd("CT_LAD2_FLOOR",80);
    p.lad3_mfe=envd("CT_LAD3_MFE",150);

    Bars15 btc=load15(dir,"BTC"); if(!btc.N) return 1;
    auto btc_up1=trend_up(btc,p.w1); auto btc_up4=trend_up(btc,p.w4);

    std::vector<std::string> coins;
    { const char* cc=getenv("CP_COINS"); std::string cl=cc?cc:"ETH,XRP";
      std::stringstream ss(cl); std::string t; while(std::getline(ss,t,',')) if(!t.empty()) coins.push_back(t); }
    int randz=envi("CT_RANDZ",0);

    // load depth books once per coin
    auto load_depth=[&](const std::string& coin)->DepthBook*{
        DepthBook* bk=new DepthBook(); size_t n=bk->load_dir(ddir,coin+"USDT");
        if(n==0){ std::fprintf(stderr,"[warn] no depth for %s in %s -> flat fallback\n",coin.c_str(),ddir.c_str()); delete bk; return nullptr; }
        return bk; };

    if(randz>0){
        std::printf("RANDZ null-test (%d seeds) Q=$%.0f | real trigger net vs random-entry-in-uptrend + same exit + same depth cost\n",randz,p.qusd);
        for(auto cs:coins){ Bars15 b=(cs=="BTC")?btc:load15(dir,cs); if(!b.N) continue;
            Params pc=p; pc.depth=load_depth(cs);
            auto tr=run_core(b,pc,&btc_up1,&btc_up4); auto m=metrics(tr);
            std::vector<double> nulls; for(int s=0;s<randz;++s) nulls.push_back(run_null(b,pc,m.n,1000+s,&btc_up1,&btc_up4));
            std::sort(nulls.begin(),nulls.end());
            double nmean=0; for(double x:nulls)nmean+=x; nmean/= (nulls.empty()?1:nulls.size());
            double p95=nulls.empty()?0:nulls[(int)(0.95*nulls.size())]; double p05=nulls.empty()?0:nulls[(int)(0.05*nulls.size())];
            int beat=0; for(double x:nulls) if(m.net>x) beat++;
            std::printf("%-4s real net=%+.0f (n=%d) | null mean=%+.0f p05=%+.0f p95=%+.0f | real beats %d/%d nulls (%.0f%%ile) %s\n",
                cs.c_str(),m.net,m.n,nmean,p05,p95,beat,randz,100.0*beat/randz, (m.net>p95?"EDGE-in-structure":"within-null"));
        }
        return 0;
    }

    std::printf("CORE-TRIGGER phase2 | Q=$%.0f fee_rt=%.0f reserve=%.0f safe_ref=%.0f | comp_w=%.0f short=%.2f med=%.2f move=%.1fx pb=%.2f trail=max(%.0f,%.1f*atr) btc_gate=%d\n",
        p.qusd,p.fee_rt,p.reserve,p.safe_ref,p.comp_w,p.short_thr,p.med_thr,p.move_mult,p.pb_maxfrac,p.trailmin,p.trailatr,p.btc_gate);
    std::printf("%-5s | %5s %5s %9s %6s %8s | %9s %9s | %-14s | %s\n","coin","n","win%","net_bp","PF","worst","WF-H1","WF-H2","depth-cov","GATE(base;2x)");
    bool all_pass=true;
    for(auto cs:coins){ Bars15 b=(cs=="BTC")?btc:load15(dir,cs); if(!b.N){all_pass=false;continue;}
        DepthBook* bk=load_depth(cs);
        long nfb=0,ntot=0;
        Params pb=p; pb.depth=bk; pb.cost_mult=1.0; pb.n_fb=&nfb; pb.n_tot=&ntot;
        Params p2=p; p2.depth=bk; p2.cost_mult=2.0;
        auto trb=run_core(b,pb,&btc_up1,&btc_up4);
        auto tr2=run_core(b,p2,&btc_up1,&btc_up4);
        auto m=metrics(trb); auto m2=metrics(tr2);
        bool base = m.net>0&&m.pf>=1.3&&m.h1>0&&m.h2>0;
        bool x2   = m2.net>0&&m2.pf>=1.3;
        bool pass = base&&x2; all_pass&=pass;
        char cov[16]; std::snprintf(cov,sizeof(cov),"%ld/%ld fb",ntot-nfb,ntot);
        std::printf("%-5s | %5d %5.1f %+9.0f %6.2f %+8.0f | %+9.0f %+9.0f | %-14s | %s [2x net%+.0f pf%.2f]\n",
            cs.c_str(),m.n, m.n>0?100.0*m.nwin/m.n:0, m.net,m.pf,m.worst,m.h1,m.h2, cov, pass?"PASS":"fail", m2.net,m2.pf);
    }
    std::printf("PHASE-2 VERDICT (real depth cost, proxy flow): %s\n",
        all_pass?"CORE edge SURVIVES real cost -> proceed (add tick flow, then Phase 3)":"CORE FAILS under real cost -> report / rethink");
    return 0;
}
