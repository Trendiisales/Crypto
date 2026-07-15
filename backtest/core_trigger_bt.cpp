// core_trigger_bt.cpp — PHASE 1 of the CORE/MIMIC campaign build (2026-07-15l).
// KILL-EARLY GATE: does the spec's §3 CORE trigger have a standalone edge on BTC/ETH
// AFTER a realistic dynamic-ish cost, before we build any campaign machinery?
//
// Spec (CORE_MIMIC_SPEC.md §3): 15m compression -> range breakout WITH real aggressive
// spot buying -> pullback HOLDS above old range-high / anchored VWAP -> higher-low forms
// -> price RECLAIMS the pullback high -> CORE BUY. Regime gate: 1h & 4h trend up, price
// above anchored VWAP, HH/HL structure, BTC regime positive (even for ETH), expected
// remaining move >= 3x safe cost.
//
// PHASE-1 SIMPLIFICATIONS (documented, deliberate; refine in later phases with the tick
// tape + perp bookDepth now downloading):
//  - Runs on 15m bars aggregated from 1m klines (data/klines_spot/<COIN>USDT_1m.csv).
//    Entry/exit at 15m close. Intra-bar precision deferred.
//  - Aggressive-buy share from aggregated taker_buy_base fraction per 15m bar (kline field),
//    NOT tick-tape OFI (that's Phase-2, needs the aggTrades pull).
//  - 1h/4h "trend regime up" proxied as close > SMA(W1~4h) and close > SMA(W4~16h), both
//    rising-implied by the close>SMA test. BTC gate = same test on BTCUSDT, applied to both.
//  - safe_cost = FLAT conservative 35bp (operator example: 20 base +10 stress +5 latency).
//    Real depth-adjusted safe_cost is Phase-2 (perp bookDepth). 2x-cost gate = 70bp.
//  - "expected remaining move >= 3x safe cost" proxied by breakout-impulse >= MOVE_MULT x
//    safe_cost (economic-room proxy; the move that just happened implies room). Noted.
//  - spread/depth entry limits: no 1m spread data -> assumed within limits (absorbed by the
//    conservative flat safe_cost). Noted.
//
// GATE (long-only spot; data 2025-05..2026-05 so 2022 auto-omitted):
//   base: net>0 & PF>=1.3 & WF-H1>0 & WF-H2>0   AND   2x-cost: net>0 & PF>=1.3
//   Must hold on BOTH BTCUSDT and ETHUSDT to pass Phase 1.
//
// Build: clang++ -std=c++17 -O2 core_trigger_bt.cpp -o core_trigger_bt
// Data : CT_DATA_DIR (default /Users/jo/ChimeraCrypto/data/klines_spot)
// Env (all optional, defaults are the mid of the spec's candidate ranges):
//   CT_COMP_W (80)  compression max width bps (spec 35-120)
//   CT_COMP_BARS(6) compression lookback in 15m bars (~90min)
//   CT_SHORT_THR(0.60) agg-buy share short window (spec 0.58-0.65)
//   CT_SHORT_W(3)   short agg window in 15m bars
//   CT_MED_THR(0.56) agg-buy share medium window (spec 0.54-0.60)
//   CT_MED_W(8)     medium agg window in 15m bars
//   CT_MOVE_MULT(3.0) min breakout impulse as x of safe_cost (spec 3.0-4.0)
//   CT_PB_MAXFRAC(0.50) max pullback depth as frac of impulse (spec 0.35-0.50)
//   CT_W1(16) CT_W4(64)  trend SMA windows in 15m bars (~4h / ~16h)
//   CT_SAFE(35) safe_cost bps ; CT_TRAILMIN(40) min trail bps ; CT_TRAILATR(0.5) x 15m-ATR-bps
//   CT_STOPBUF(5) stop buffer bps below HL ; CT_MAXWAIT(24) max 15m bars breakout->reclaim
//   CT_COOLDOWN(8) bars after exit ; CT_SWEEP(0) 1=run COMP_W x SHORT_THR grid
//   CT_BTC_GATE(1) require BTC regime positive

#include <cstdio>
#include <cstdlib>
#include <cmath>
#include <cstdint>
#include <fstream>
#include <sstream>
#include <string>
#include <vector>
#include <algorithm>

static double envd(const char* k,double d){ const char* v=getenv(k); return v?atof(v):d; }
static int    envi(const char* k,int d){ const char* v=getenv(k); return v?atoi(v):d; }

struct Bars15 { std::vector<int64_t> ts; std::vector<double> o,h,l,c,tbb_frac; int N=0; };

static std::vector<std::string> split(const std::string& s){
    std::vector<std::string> v; v.reserve(12); std::stringstream ss(s); std::string t;
    while(std::getline(ss,t,',')) v.push_back(t); return v; }

// Load 1m klines and aggregate to 15m. Kline cols:
// 0 open_time_ms,1 open,2 high,3 low,4 close,5 volume,...,9 taker_buy_base
static Bars15 load15(const std::string& dir,const std::string& coin){
    Bars15 b; std::ifstream f(dir+"/"+coin+"USDT_1m.csv");
    if(!f){ std::fprintf(stderr,"no data for %s in %s\n",coin.c_str(),dir.c_str()); return b; }
    std::string ln; std::getline(f,ln); // header
    int cnt=0; double O=0,H=0,L=0,C=0,V=0,TBB=0; int64_t bts=0;
    auto flush=[&](){ if(cnt>0){ b.ts.push_back(bts); b.o.push_back(O); b.h.push_back(H);
        b.l.push_back(L); b.c.push_back(C); b.tbb_frac.push_back(V>0?TBB/V:0.5); } };
    while(std::getline(f,ln)){
        auto v=split(ln); if(v.size()<10) continue;
        int64_t ts=(int64_t)std::stoll(v[0]);
        double o=std::stod(v[1]),h=std::stod(v[2]),l=std::stod(v[3]),c=std::stod(v[4]);
        double vol=std::stod(v[5]),tbb=std::stod(v[9]);
        int64_t slot = ts - (ts % (15LL*60*1000)); // 15m bucket start
        if(cnt==0 || slot!=bts){ flush(); bts=slot; O=o; H=h; L=l; C=c; V=vol; TBB=tbb; cnt=1; }
        else { H=std::max(H,h); L=std::min(L,l); C=c; V+=vol; TBB+=tbb; cnt++; }
    }
    flush(); b.N=(int)b.ts.size(); return b;
}

// close > SMA(W) trend proxy
static std::vector<char> trend_up(const Bars15& b,int W){
    std::vector<char> up(b.N,0); double sum=0;
    for(int i=0;i<b.N;++i){ sum+=b.c[i]; if(i>=W) sum-=b.c[i-W];
        if(i>=W){ double sma=sum/W; up[i]=(b.c[i]>sma)?1:0; } }
    return up;
}
// 15m ATR in bps (Wilder-ish simple mean of true range over 14)
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
    double comp_w, short_thr, med_thr, move_mult, pb_maxfrac, safe, trailmin, trailatr, stopbuf;
    int comp_bars, short_w, med_w, w1, w4, maxwait, cooldown, btc_gate, room_h;
};
// stage counters for debugging where setups die (printed when CT_DEBUG=1)
struct Dbg{ long comp=0,broke=0,pulled=0,reclaim=0,gate_block=0,entry=0; };
static Dbg g_dbg;

// Run the CORE trigger on one coin. btc_up1/up4 aligned by index (same span/agg) if provided.
static std::vector<Trade> run_core(const Bars15& b,const Params& p,double safe_cost,
                                   const std::vector<char>* btc_up1,const std::vector<char>* btc_up4){
    std::vector<Trade> tr;
    auto up1=trend_up(b,p.w1); auto up4=trend_up(b,p.w4); auto atr=atr_bps(b);
    int warm=std::max(p.w4,p.comp_bars)+2;
    enum St{SCAN,COMP,BROKE,INPOS}; St st=SCAN;
    double rhi=0,rlo=0,anchor_px=0; double vwap_num=0,vwap_den=0; // anchored vwap from comp start
    double impulse=0,peak=0,pb_low=0; int brk_i=0; bool pulled=false;
    double entry=0,stop=0,ppeak=0; int cool_until=-1;
    for(int i=warm;i<b.N;++i){
        // maintain anchored VWAP once anchored (COMP/BROKE)
        if(st==COMP||st==BROKE){ double tp=(b.h[i]+b.l[i]+b.c[i])/3.0; vwap_num+=tp; vwap_den+=1; }
        double vwap = vwap_den>0? vwap_num/vwap_den : b.c[i];

        if(st==SCAN){
            if(i<=cool_until) continue;
            // compression over last comp_bars completed bars [i-cb+1..i]
            int a=i-p.comp_bars+1; double mh=-1e18,ml=1e18;
            for(int j=a;j<=i;++j){ mh=std::max(mh,b.h[j]); ml=std::min(ml,b.l[j]); }
            double mid=(mh+ml)/2.0; double w=mid>0?(mh-ml)/mid*1e4:1e9;
            if(w<=p.comp_w){ rhi=mh; rlo=ml; anchor_px=mid; st=COMP; g_dbg.comp++;
                vwap_num=0; vwap_den=0; // re-anchor at compression
            }
        }
        else if(st==COMP){
            // breakout: close above range hi WITH aggressive buying
            double sh=0; { int a=std::max(0,i-p.short_w+1),n=0; for(int j=a;j<=i;++j){sh+=b.tbb_frac[j];n++;} sh/= (n>0?n:1);}
            double md=0; { int a=std::max(0,i-p.med_w+1),n=0; for(int j=a;j<=i;++j){md+=b.tbb_frac[j];n++;} md/=(n>0?n:1);}
            if(b.c[i]>rhi && sh>=p.short_thr && md>=p.med_thr){
                // impulse = breakout leg from range LOW to breakout close (measured-move basis)
                impulse=(b.c[i]-rlo)/rlo*1e4;
                st=BROKE; brk_i=i; peak=b.h[i]; pb_low=b.l[i]; pulled=false; g_dbg.broke++;
            } else if(b.l[i] < rlo){ st=SCAN; } // broke down -> abandon
        }
        else if(st==BROKE){
            if(i-brk_i>p.maxwait){ st=SCAN; continue; }
            if(!pulled) peak=std::max(peak,b.h[i]); // freeze the pre-pullback peak once pullback starts
            // held-above test: a low that loses max(range_hi, vwap) => setup failed
            double hold = std::max(rhi,vwap);
            if(b.l[i] < hold){ st=SCAN; continue; }
            // pullback tracking: once close pulls back from peak by a bit
            double giveback_bps = (peak-b.c[i])/peak*1e4;
            if(!pulled){ if(giveback_bps >= std::max(15.0,0.25*impulse)){ pulled=true; pb_low=b.l[i]; g_dbg.pulled++; } }
            else { pb_low=std::min(pb_low,b.l[i]);
                double pbdepth=(peak-pb_low)/peak*1e4;
                if(pbdepth > p.pb_maxfrac*impulse + p.safe){ st=SCAN; continue; } // too deep
                // RECLAIM: close back above the pre-pullback peak
                if(b.c[i] > peak){ g_dbg.reclaim++;
                    // regime gate at entry
                    bool g_up = up1[i] && up4[i];
                    bool g_vwap = b.c[i] > vwap;
                    bool g_btc = (!p.btc_gate) || (btc_up1&&btc_up4 ? (i<(int)btc_up1->size() && (*btc_up1)[i] && (*btc_up4)[i]) : true);
                    bool g_hl = pb_low > rhi; // higher-low above breakout level
                    bool g_room = atr[i]*p.room_h >= p.move_mult*safe_cost; // ATR-projected room over hold horizon
                    if(g_up&&g_vwap&&g_btc&&g_hl&&g_room){
                        st=INPOS; entry=b.c[i]; ppeak=b.c[i]; g_dbg.entry++;
                        stop=pb_low*(1.0 - p.stopbuf/1e4);
                    } else { st=SCAN; g_dbg.gate_block++; } // eligible setup but regime blocked
                }
            }
        }
        else if(st==INPOS){
            ppeak=std::max(ppeak,b.h[i]);
            double vw = vwap; // vwap kept updating? we stopped anchoring at entry; use last
            double trail=std::max(p.trailmin, p.trailatr*atr[i]);
            double exit_px=0; bool ex=false;
            if(b.l[i] <= stop){ exit_px=stop; ex=true; }                       // structural stop (intrabar)
            else { double gb=(ppeak-b.c[i])/ppeak*1e4;
                   if(gb>=trail){ exit_px=b.c[i]; ex=true; }                    // adaptive trail (close)
                   else if(b.c[i] < vw){ exit_px=b.c[i]; ex=true; } }           // lost anchored VWAP (close)
            if(ex){ double net=(exit_px-entry)/entry*1e4 - safe_cost;
                tr.push_back({b.ts[i],net}); st=SCAN; cool_until=i+p.cooldown;
                vwap_num=0; vwap_den=0; }
        }
    }
    return tr;
}

// NULL model: enter at RANDOM regime-up bars (same count as the real trigger) and ride the
// IDENTICAL exit. If the real trigger's net sits well above this null distribution, the edge
// is in the trigger (structure), not in "buy any uptrend + wide trail". Addresses thin-n doubt.
static double run_null(const Bars15& b,const Params& p,double safe_cost,int n_target,unsigned seed,
                       const std::vector<char>* btc_up1,const std::vector<char>* btc_up4){
    auto up1=trend_up(b,p.w1); auto up4=trend_up(b,p.w4); auto atr=atr_bps(b);
    // candidate bars = regime-up (same gate the real entry requires, sans structure)
    std::vector<int> cand; int warm=std::max(p.w4,p.comp_bars)+2;
    for(int i=warm;i<b.N;++i){ bool gb=(!p.btc_gate)||(btc_up1&&btc_up4? (i<(int)btc_up1->size()&&(*btc_up1)[i]&&(*btc_up4)[i]):true);
        if(up1[i]&&up4[i]&&gb) cand.push_back(i); }
    if((int)cand.size()<n_target||n_target<=0) return 0;
    srand(seed); double net=0; int taken=0, guard=0;
    std::vector<char> used(b.N,0);
    while(taken<n_target && guard++<n_target*50){
        int i=cand[rand()%cand.size()]; if(used[i]||i+1>=b.N) continue; used[i]=1;
        double entry=b.c[i], ppeak=b.c[i], stop=b.l[i]*(1.0-p.stopbuf/1e4); taken++;
        for(int j=i+1;j<b.N;++j){ ppeak=std::max(ppeak,b.h[j]);
            double trail=std::max(p.trailmin,p.trailatr*atr[j]); double ex=0; bool done=false;
            if(b.l[j]<=stop){ex=stop;done=true;} else{ double gb=(ppeak-b.c[j])/ppeak*1e4;
                if(gb>=trail){ex=b.c[j];done=true;} }
            if(done){ net+=(ex-entry)/entry*1e4-safe_cost; break; } }
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
    Params p;
    p.comp_w=envd("CT_COMP_W",80); p.comp_bars=envi("CT_COMP_BARS",6);
    p.short_thr=envd("CT_SHORT_THR",0.60); p.short_w=envi("CT_SHORT_W",3);
    p.med_thr=envd("CT_MED_THR",0.56); p.med_w=envi("CT_MED_W",8);
    p.move_mult=envd("CT_MOVE_MULT",3.0); p.pb_maxfrac=envd("CT_PB_MAXFRAC",0.50);
    p.w1=envi("CT_W1",16); p.w4=envi("CT_W4",64);
    p.safe=envd("CT_SAFE",35); p.trailmin=envd("CT_TRAILMIN",40); p.trailatr=envd("CT_TRAILATR",0.5);
    p.stopbuf=envd("CT_STOPBUF",5); p.maxwait=envi("CT_MAXWAIT",24); p.cooldown=envi("CT_COOLDOWN",8);
    p.btc_gate=envi("CT_BTC_GATE",1); p.room_h=envi("CT_ROOM_H",8);
    bool dbg=envi("CT_DEBUG",0);

    Bars15 btc=load15(dir,"BTC"); if(!btc.N) return 1;
    auto btc_up1=trend_up(btc,p.w1); auto btc_up4=trend_up(btc,p.w4);

    std::vector<std::string> coins;
    { const char* cc=getenv("CT_COINS"); std::string cl=cc?cc:"BTC,ETH";
      std::stringstream ss(cl); std::string t; while(std::getline(ss,t,',')) if(!t.empty()) coins.push_back(t); }
    bool sweep=envi("CT_SWEEP",0);
    int randz=envi("CT_RANDZ",0);
    if(randz>0){
        std::printf("RANDZ null-test (%d seeds): real trigger net vs random-entry-in-uptrend + same exit\n",randz);
        for(auto cs:coins){ Bars15 b=(std::string(cs)=="BTC")?btc:load15(dir,cs); if(!b.N) continue;
            auto tr=run_core(b,p,p.safe,&btc_up1,&btc_up4); auto m=metrics(tr);
            std::vector<double> nulls; for(int s=0;s<randz;++s) nulls.push_back(run_null(b,p,p.safe,m.n,1000+s,&btc_up1,&btc_up4));
            std::sort(nulls.begin(),nulls.end());
            double nmean=0; for(double x:nulls)nmean+=x; nmean/= (nulls.empty()?1:nulls.size());
            double p95=nulls.empty()?0:nulls[(int)(0.95*nulls.size())]; double p05=nulls.empty()?0:nulls[(int)(0.05*nulls.size())];
            int beat=0; for(double x:nulls) if(m.net>x) beat++;
            std::printf("%-4s real net=%+.0f (n=%d) | null mean=%+.0f p05=%+.0f p95=%+.0f | real beats %d/%d nulls (%.0f%%ile) %s\n",
                cs.c_str(),m.net,m.n,nmean,p05,p95,beat,randz,100.0*beat/randz, (m.net>p95?"EDGE-in-structure":"within-null"));
        }
        return 0;
    }

    if(!sweep){
        std::printf("CORE-TRIGGER phase1 | safe=%.0fbp comp_w=%.0f short=%.2f med=%.2f move=%.1fx pb=%.2f W1=%d W4=%d trail=max(%.0f,%.1f*atr) btc_gate=%d\n",
            p.safe,p.comp_w,p.short_thr,p.med_thr,p.move_mult,p.pb_maxfrac,p.w1,p.w4,p.trailmin,p.trailatr,p.btc_gate);
        std::printf("%-5s | %5s %5s %9s %6s %8s | %9s %9s | %s\n","coin","n","win%","net_bp","PF","worst","WF-H1","WF-H2","GATE(base;2x)");
        bool all_pass=true;
        for(auto cs:coins){ Bars15 b= (std::string(cs)=="BTC")?btc:load15(dir,cs); if(!b.N){all_pass=false;continue;}
            auto trb=run_core(b,p,p.safe,&btc_up1,&btc_up4);
            auto tr2=run_core(b,p,p.safe*2.0,&btc_up1,&btc_up4);
            auto m=metrics(trb); auto m2=metrics(tr2);
            bool base = m.net>0&&m.pf>=1.3&&m.h1>0&&m.h2>0;
            bool x2   = m2.net>0&&m2.pf>=1.3;
            bool pass = base&&x2; all_pass&=pass;
            std::printf("%-5s | %5d %5.1f %+9.0f %6.2f %+8.0f | %+9.0f %+9.0f | %s [2x net%+.0f pf%.2f]\n",
                cs.c_str(),m.n, m.n>0?100.0*m.nwin/m.n:0, m.net,m.pf,m.worst,m.h1,m.h2, pass?"PASS":"fail", m2.net,m2.pf);
            if(dbg) std::printf("   [dbg %s] comp=%ld broke=%ld pulled=%ld reclaim=%ld gate_block=%ld entry=%ld\n",
                cs.c_str(),g_dbg.comp,g_dbg.broke,g_dbg.pulled,g_dbg.reclaim,g_dbg.gate_block,g_dbg.entry);
            g_dbg=Dbg{};
        }
        std::printf("PHASE-1 VERDICT: %s\n", all_pass?"CORE HAS EDGE on BTC+ETH -> proceed to Phase 2":"CORE FAILS gate -> STOP / rethink trigger");
    } else {
        // 2D plateau sweep: entry selectivity (short_thr) x trend-ride trail width (trailmin).
        // For each cell print per-coin: n/net/PF/WF-both + PASS flag (base&2x&WF). Look for a
        // PLATEAU (>=3 adjacent PASS on BOTH coins), not isolated lucky cells (overfit trap).
        Bars15 eth=load15(dir,"ETH");
        double sts[]={0.58,0.60,0.62,0.64,0.66}; double tms[]={80,120,160,200,240};
        std::printf("PLATEAU SWEEP  short_thr(rows) x trailmin(cols)  safe=%.0fbp  cell='BTC|ETH' P=base&2x&WF pass\n",p.safe);
        std::printf("       "); for(double t:tms) std::printf("  tm=%-15.0f",t); std::printf("\n");
        for(double s:sts){ std::printf("st=%.2f",s);
            for(double t:tms){ Params q=p; q.short_thr=s; q.trailmin=t;
                auto ev=[&](Bars15& b)->int{ // 0=fail 1=pass
                    auto a=run_core(b,q,q.safe,&btc_up1,&btc_up4); auto a2=run_core(b,q,q.safe*2,&btc_up1,&btc_up4);
                    auto m=metrics(a); auto m2=metrics(a2);
                    return (m.net>0&&m.pf>=1.3&&m.h1>0&&m.h2>0&&m2.net>0&&m2.pf>=1.3)?1:0; };
                auto sm=[&](Bars15& b){ auto a=run_core(b,q,q.safe,&btc_up1,&btc_up4); return metrics(a); };
                int pb=ev(btc), pe=ev(eth); auto mb=sm(btc), me=sm(eth);
                char flag = (pb&&pe)?'P':(pe?'e':(pb?'b':'.'));
                std::printf("  %c %+5.0f/%-5.0f(%d/%d)",flag,mb.net,me.net,mb.n,me.n); }
            std::printf("\n"); }
        std::printf("  flag: P=both PASS  e=ETH-only  b=BTC-only  .=neither ; nums = net_bp BTC/ETH (n_btc/n_eth)\n");
    }
    return 0;
}
