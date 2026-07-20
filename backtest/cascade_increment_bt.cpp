// cascade_increment_bt.cpp — S-2026-07-20 INCREMENT-GATED CASCADE (operator proposal:
// "if the cost per BE is 38bp then surely we can arm the next mimic once we have attained
// the 38bp level and so on and then keep incrementing until the trade reverses").
//
// Spec under test (exact, honest-by-construction — answers MimicShadowEntryBasisError):
//   - Leg k arms ONLY when fav (from window entry) >= confirm + k*inc  (escalating per-tier
//     confirm ladder: 60, 60+inc, 60+2*inc, ... — each increment is pre-funded by the run).
//   - confirm_anchor_epx = FALSE: le = the leg's OWN fill (the mark at confirm crossing;
//     e1 drive = bar HIGH, pessimistic). Floor, trail, cut and BOOKING all run from the
//     real fill — the shadow ledger equals the mirror's economics by construction.
//   - mimic_floor + per-leg BE floor at own fill + giveback g trail (engine L983-986: the
//     escalating-confirm stagger design — "each opens at a DIFFERENT price and floors at
//     its OWN fill").
//   - loss_cut_bp = 60 (2xRT): pre-arm reversal cut at fill*(1-60bp) books PREBE_CUT —
//     mirror o2-band parity ([fill-2RT, fill-RT] exit). The honest churn cost per failed
//     increment is bounded ~ -(lc+RT).
//   - reclip OFF (window re-triggers on the next detector event).
// Same drive/gate as the S-20j/honest-basis certs: REAL MimicLadderCompanion @ ChimeraCrypto
// HEAD, e1 live-faithful worse-of (open at bar high, same-bar low tests cut/floor), RT=30/60bp,
// omit-2022 gate (net>0, PF>=1.3, both WF halves, base AND 2x cost). NO booking transform —
// net_bp_real is already own-fill basis under this config.
//
// Build: clang++ -std=c++17 -O2 -I/Users/jo/ChimeraCrypto/include cascade_increment_bt.cpp -o cascade_increment_bt
// Env:   UM_COIN UM_RT(30) UM_THR UM_W UM_G UM_LEGS(8) UM_CONFIRM(60) UM_INC(38) UM_LC(60) UM_EARLY(1)
//        UM_SINGLE=1 (S-2026-07-20p order): mimic_stagger=false -> the engine's own plain
//        mimic_floor design = ONE managed T1 leg per window event (header L979-982: extra arm
//        tiers are meaningless under a single-g floor trail). anchor=0 own-fill + OHLC-path
//        drive unchanged, so booking stays honest by construction. inc is a no-op (forced {0}),
//        legs forced 1 — the sweep axes are lc x W x g only.
//        UM_E0=1 (S-2026-07-20p order item 2a, EXACT close-confirm cert): live BECASC design
//        (stagger_mode=1 BE-cascade, stagger_be_bp=20, uniform confirm60, legs 8) with the
//        confirm evaluated ONLY at bar close: drive = stop_check_only(low) [byte-original
//        certified stop drive] + observe(close). anchor=0 -> le = the ACTUAL close fill, so
//        booking is exact own-fill (NOT the le*(1+confirm) re-base upper bound of
//        honest_entry_basis_bt e0=747/840). inc forced {0} (uniform confirm = live design).
//        lc sweep {30,60} = mirror own-stop o2-band parity.

#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <cmath>
#include <string>
#include <vector>
#include <algorithm>
#include <fstream>
#include <sstream>
#include <ctime>
#include <unistd.h>
#include <fcntl.h>
#include "core/MimicLadderCompanion.hpp"
using chimera::MimicLadderCompanion;

struct Bar { int64_t ts; double o,h,l,c; };
static std::vector<std::string> split(const std::string&s){std::vector<std::string>v;std::stringstream ss(s);std::string t;while(std::getline(ss,t,','))v.push_back(t);return v;}
static std::vector<Bar> load(){
    const char* cn=getenv("UM_COIN"); std::string coin=cn?cn:"ETH";
    std::vector<Bar> b; std::ifstream f("/Users/jo/Crypto/backtest/data/"+coin+"USDT_1h.csv");
    if(!f){std::fprintf(stderr,"no %s data\n",coin.c_str());return b;} std::string ln; std::getline(f,ln);
    while(std::getline(f,ln)){auto v=split(ln);if(v.size()<5)continue;Bar x;x.ts=(int64_t)std::stoll(v[0]);
        x.o=std::stod(v[1]);x.h=std::stod(v[2]);x.l=std::stod(v[3]);x.c=std::stod(v[4]);
        if(x.c>0)b.push_back(x);} return b;
}
static int year_of(int64_t ms){time_t t=(time_t)(ms/1000);struct tm g;gmtime_r(&t,&g);return 1900+g.tm_year;}

struct Clip { int64_t ts; double net_bp; bool prebe; };
struct Agg { int n; double net,pf,worst,h1,h2; int neg,nprebe; double negsum,prebesum; double yr[8]; };
static int yidx(int y){int k=y-2021;return k<0?0:(k>7?7:k);}

static double g_confirm=60.0, g_inc=38.0, g_lc=60.0, g_step=15.0;
static int    g_early=1, g_single=0, g_e0=0;
static Agg run(const std::vector<Bar>& b, int W, double thr, double g, int legs, double rt){
    MimicLadderCompanion::Config c;
    c.parent_tag="SELF"; c.tag="CI-BT"; c.symbol="btusdt";
    c.det_w=W; c.det_thr=thr; c.tf_secs=3600; c.round_trip_bp=rt;
    c.mimic_floor=true; c.mimic_stagger=!g_single; c.stagger_mode=0;   // all legs eligible; each self-gates on its OWN confirm level. UM_SINGLE: stagger off -> engine builds T1 only
    if(g_e0){ c.stagger_mode=1; c.stagger_be_bp=20.0; }                // UM_E0: live BECASC release rule (BE-cascade), uniform confirm60 via inc=0
    c.reclip_pct=0.0; c.loss_cut_bp=g_lc; c.be_floor=false;
    c.confirm_bp=g_confirm;
    c.confirm_anchor_epx=false;          // le = OWN FILL -> floor/cut/booking honest by construction
    c.mimic_giveback=g;
    // escalating per-tier confirm ladder: 60, 60+inc, 60+2*inc, ...
    c.tight={0.2,0,0.0,0,g_confirm};
    c.wide ={0.2,0,0.0,0,g_confirm+g_inc};
    for(int k=2;k<legs;k++) c.extra_base.push_back({0.2,0,0.0,0,g_confirm+g_inc*k});
    c.cap=legs;

    std::vector<Clip> rows; int64_t cur=0;
    fflush(stdout); int saved=dup(1); {int dn=open("/dev/null",O_WRONLY);dup2(dn,1);close(dn);}
    MimicLadderCompanion eng(c);
    eng.set_on_clip([&](const MimicLadderCompanion::ClipRecord& r){
        rows.push_back({cur, r.net_bp_real, r.reason=="PREBE_CUT"});
    });
    // Live-faithful tick walk on the canonical OHLC path (up bar O→L→H→C, down bar
    // O→H→L→C), <=g_step bp per tick. Own-fill basis makes the fill PRICE load-bearing
    // (unlike the anchored certs where entry@high was harmless pessimism): legs fill
    // within one step of their confirm level, exactly as live ticks fill them, and
    // cuts/floors are tested by every down-tick along the path (observe() runs
    // intrabar_confirm_opens_ + intrabar_reversal_cut_ + intrabar_mimic_floor_ per tick).
    // The always-low-AFTER-fill ordering of the anchored e1 drive is a mechanism-killing
    // bias here: it charges every fresh fill the bar's full pre-fill low.
    auto walk=[&](double a,double bpx,int64_t ts){
        const double step=g_step/1e4;
        if(bpx>a){ for(double px=a*(1.0+step); px<bpx; px*=(1.0+step)) eng.observe(true,0.0,px,ts); }
        else if(bpx<a){ for(double px=a*(1.0-step); px>bpx; px*=(1.0-step)) eng.observe(true,0.0,px,ts); }
        eng.observe(true,0.0,bpx,ts);
    };
    for(const auto& k : b){
        cur=k.ts;
        if(g_e0){                                   // EXACT close-confirm: stops tested at the low
            eng.stop_check_only(k.l,k.ts);          // (byte-original certified stop drive); confirm
            eng.observe(true,0.0,k.c,k.ts);         // only sees the CLOSE -> le = real close fill
            continue;
        }
        eng.observe(true,0.0,k.o,k.ts);
        if(k.c>=k.o){ walk(k.o,k.l,k.ts); walk(k.l,k.h,k.ts); walk(k.h,k.c,k.ts); }
        else        { walk(k.o,k.h,k.ts); walk(k.h,k.l,k.ts); walk(k.l,k.c,k.ts); }
    }
    if(!b.empty()){ cur=b.back().ts; eng.observe(false,0.0,b.back().c,b.back().ts+c.tf_secs*1000+2000); }
    fflush(stdout); dup2(saved,1); close(saved);

    std::sort(rows.begin(),rows.end(),[](const Clip&a,const Clip&b){return a.ts<b.ts;});
    Agg a{}; double gw=0,gl=0; for(int i=0;i<8;i++)a.yr[i]=0;
    for(size_t k=0;k<rows.size();k++){ double p=rows[k].net_bp; a.net+=p/100.0;
        if(p>0)gw+=p; else{gl-=p;a.neg++;a.negsum+=p/100.0;} if(p<a.worst)a.worst=p;
        if(rows[k].prebe){a.nprebe++;a.prebesum+=p/100.0;}
        a.yr[yidx(year_of(rows[k].ts))]+=p/100.0;
        if(k<rows.size()/2)a.h1+=p/100.0; else a.h2+=p/100.0; }
    a.n=(int)rows.size(); a.pf=gl>0?gw/gl:(gw>0?999:0);
    return a;
}

int main(){
    double rt = getenv("UM_RT")?atof(getenv("UM_RT")):30.0;
    double thr= getenv("UM_THR")?atof(getenv("UM_THR")):0.015;
    g_confirm = getenv("UM_CONFIRM")?atof(getenv("UM_CONFIRM")):60.0;
    std::vector<double> Lcs; { const char*e=getenv("UM_LC"); std::string s2=e?e:"30,60";
        std::stringstream ss(s2); std::string t; while(std::getline(ss,t,','))if(!t.empty())Lcs.push_back(atof(t.c_str())); }
    g_step    = getenv("UM_STEP")?atof(getenv("UM_STEP")):15.0;
    g_early   = getenv("UM_EARLY")?atoi(getenv("UM_EARLY")):1;
    std::vector<double> Incs; { const char*e=getenv("UM_INC"); std::string s=e?e:"38,60,100";
        std::stringstream ss(s); std::string t; while(std::getline(ss,t,','))if(!t.empty())Incs.push_back(atof(t.c_str())); }
    std::vector<int> Ws; { const char*e=getenv("UM_W"); std::string s=e?e:"2,4,8,12";
        std::stringstream ss(s); std::string t; while(std::getline(ss,t,','))if(!t.empty())Ws.push_back(atoi(t.c_str())); }
    std::vector<double> Gs; { const char*e=getenv("UM_G"); std::string s=e?e:"0.2,0.5,0.75";
        std::stringstream ss(s); std::string t; while(std::getline(ss,t,','))if(!t.empty())Gs.push_back(atof(t.c_str())); }
    int legs = getenv("UM_LEGS")?atoi(getenv("UM_LEGS")):8;
    g_single = getenv("UM_SINGLE")?atoi(getenv("UM_SINGLE")):0;
    if(g_single){ Incs={0.0}; legs=1; }   // single T1 leg: inc/legs axes are no-ops
    g_e0 = getenv("UM_E0")?atoi(getenv("UM_E0")):0;
    if(g_e0){ Incs={0.0}; }               // uniform confirm60 across tiers = live BECASC design

    std::vector<Bar> b=load(); if(b.empty())return 1;
    std::printf("%s CERT — coin=%s early=%d bars=%zu thr=%.2f%% confirm=%.0fbp RT=%.0f/%.0fbp legs=%d (le=OWN FILL, honest by construction)\n",
        g_e0?"E0-EXACT CLOSE-CONFIRM BECASC":(g_single?"SINGLE-LEG MIMIC":"INCREMENT-CASCADE"),
        getenv("UM_COIN")?getenv("UM_COIN"):"ETH", g_early, b.size(), thr*100, g_confirm, rt, rt*2, legs);
    std::printf("%-3s %-4s %-3s %-4s | %5s %8s %5s %9s | %5s %8s | %6s %8s | %8s %8s | %8s | %s\n",
        "lc","inc","W","g","n","net%","PF","worst_bp","nNeg","sumNeg%","nPREBE","prebe%","H1%","H2%","2xnet%","GATE(base;2x; omit22)");
    for(double lc:Lcs){ g_lc=lc;
    for(double inc:Incs){ g_inc=inc;
        for(int W:Ws)for(double g:Gs){
            Agg a  = run(b,W,thr,g,legs,rt);
            Agg a2 = run(b,W,thr,g,legs,rt*2.0);
            double net_x22  = a.net  - a.yr[yidx(2022)];
            double net2_x22 = a2.net - a2.yr[yidx(2022)];
            bool gate = net_x22>0 && a.pf>=1.3 && a.h1>0 && a.h2>0 && net2_x22>0 && a2.pf>=1.3;
            std::printf("%-3.0f %-4.0f %-3d %-4.2f | %5d %+8.0f %5.2f %+9.1f | %5d %+8.1f | %6d %+8.1f | %+8.0f %+8.0f | %+8.0f | %s\n",
                g_lc,inc,W,g, a.n,a.net,a.pf,a.worst, a.neg,a.negsum, a.nprebe,a.prebesum, a.h1,a.h2, a2.net, gate?"PASS":"fail");
        }
    }}
    return 0;
}
