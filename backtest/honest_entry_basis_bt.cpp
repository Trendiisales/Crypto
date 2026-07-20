// honest_entry_basis_bt.cpp — S-2026-07-20 HONEST ENTRY-BASIS re-cert (operator order:
// "run the honest-basis re-cert now", handoff 2026-07-20o).
//
// Finding being tested (MimicShadowEntryBasisError, 17f-class ENTRY edition): the shadow
// books every confirmed anchored leg from le=epx (window entry), but the REAL LiveMimicMirror
// fills at the confirm level = le*(1+confirm) ~= +60bp above the booking basis. No real
// order ever fills at epx. Live proof: 13 real BE-exits avg -36bp/leg, zero positive, while
// the shadow booked the same clips >=0.
//
// This harness re-runs the S-20j EARLY-CONFIRM cert matrix (early_confirm_bt.cpp, 840/840)
// with the ONE change ordered: every clip is re-based to ENTRY AT THE CONFIRM FILL.
// Exits are unchanged (the mirror exits when the shadow clips — same exit px), the cost
// debit is unchanged; only the entry basis shifts. Exact per-clip transform:
//     cost         = gross_bp_real - net_bp_real            (whatever the engine debited)
//     honest_gross = ((1 + gross_bp_real/1e4)/(1 + confirm/1e4) - 1)*1e4
//     honest_net   = honest_gross - cost
// (gross_bp_real = (exit/le - 1)*1e4; re-basing entry to le*(1+cf) gives
//  (exit/(le*(1+cf)) - 1)*1e4 = ((1+gross/1e4)/(1+cf) - 1)*1e4 — exact, no approximation.)
//
// Everything else byte-matches early_confirm_bt.cpp: drives the REAL live
// MimicLadderCompanion from /Users/jo/ChimeraCrypto/include, confirm60 BE-ENTRY anchored
// (le=epx floored-on-open), legs8, RT=30/60bp worse-of fills, omit-2022 gate
// (net>0, PF>=1.3, both WF halves, base AND 2x cost).
//
// Build: clang++ -std=c++17 -O2 -I/Users/jo/ChimeraCrypto/include honest_entry_basis_bt.cpp -o honest_entry_basis_bt
// Env:   UM_COIN UM_RT(30) UM_THR UM_W UM_G UM_LEGS UM_CONFIRM(60) UM_ANCHOR(1) UM_EARLY(0/1)

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

struct Clip { int64_t ts; double net_bp; };
struct Agg { int n; double net,pf,worst,h1,h2,floormin; int neg; double negsum; double yr[8]; };
static int yidx(int y){int k=y-2021;return k<0?0:(k>7?7:k);}

static double g_confirm=60.0;
static int    g_anchor=1;
static int    g_early=0;
static Agg run(const std::vector<Bar>& b, int W, double thr, double g, int legs, double rt){
    MimicLadderCompanion::Config c;
    c.parent_tag="SELF"; c.tag="HB-BT"; c.symbol="btusdt";
    c.det_w=W; c.det_thr=thr; c.tf_secs=3600; c.round_trip_bp=rt;
    c.mimic_floor=true; c.mimic_stagger=true; c.stagger_mode=1; c.stagger_be_bp=20.0;
    c.reclip_pct=0.0; c.loss_cut_bp=0.0; c.confirm_bp=g_confirm; c.be_floor=false;
    c.confirm_anchor_epx=(g_anchor!=0);
    c.mimic_giveback=g;
    c.tight={0.2,0,0.0,0,0.0}; c.wide={0.2,0,0.0,0,0.0};
    for(int k=2;k<legs;k++) c.extra_base.push_back({0.2,0,0.0,0,0.0});
    c.cap=legs;

    const double cf = g_confirm/1e4;   // honest entry basis: le*(1+cf)
    std::vector<Clip> rows; int64_t cur=0;
    fflush(stdout); int saved=dup(1); {int dn=open("/dev/null",O_WRONLY);dup2(dn,1);close(dn);}
    MimicLadderCompanion eng(c);
    eng.set_on_clip([&](const MimicLadderCompanion::ClipRecord& r){
        const double cost         = r.gross_bp_real - r.net_bp_real;
        const double honest_gross = ((1.0 + r.gross_bp_real/1e4)/(1.0 + cf) - 1.0)*1e4;
        rows.push_back({cur, honest_gross - cost});
    });
    for(const auto& k : b){
        cur=k.ts;
        if(g_early){
            eng.observe(true,0.0,k.h,k.ts);          // first tick of the bar at the HIGH:
                                                     // prior-bar close-walk + intra-bar confirm fill
            eng.stop_check_only(k.l,k.ts);           // worse-of: same bar's low vs the fresh leg
            eng.observe(true,0.0,k.c,k.ts);          // settle running close at the real close
        } else {
            eng.stop_check_only(k.l,k.ts);           // byte-original certified drive
            eng.observe(true,0.0,k.c,k.ts);
        }
    }
    if(!b.empty()){ cur=b.back().ts; eng.observe(false,0.0,b.back().c,b.back().ts+c.tf_secs*1000+2000); }
    fflush(stdout); dup2(saved,1); close(saved);

    std::sort(rows.begin(),rows.end(),[](const Clip&a,const Clip&b){return a.ts<b.ts;});
    Agg a{}; double gw=0,gl=0; a.floormin=1e18; for(int i=0;i<8;i++)a.yr[i]=0;
    for(size_t k=0;k<rows.size();k++){ double p=rows[k].net_bp; a.net+=p/100.0;
        if(p>0)gw+=p; else{gl-=p;a.neg++;a.negsum+=p/100.0;} if(p<a.worst)a.worst=p;
        if(p<a.floormin)a.floormin=p; a.yr[yidx(year_of(rows[k].ts))]+=p/100.0;
        if(k<rows.size()/2)a.h1+=p/100.0; else a.h2+=p/100.0; }
    a.n=(int)rows.size(); a.pf=gl>0?gw/gl:(gw>0?999:0); if(rows.empty())a.floormin=0;
    return a;
}

int main(){
    double rt = getenv("UM_RT")?atof(getenv("UM_RT")):30.0;
    double thr= getenv("UM_THR")?atof(getenv("UM_THR")):0.005;
    g_confirm = getenv("UM_CONFIRM")?atof(getenv("UM_CONFIRM")):60.0;
    g_anchor  = getenv("UM_ANCHOR")?atoi(getenv("UM_ANCHOR")):1;
    g_early   = getenv("UM_EARLY")?atoi(getenv("UM_EARLY")):0;
    std::vector<int> Ws; { const char*e=getenv("UM_W"); std::string s=e?e:"1,2,4,12";
        std::stringstream ss(s); std::string t; while(std::getline(ss,t,','))if(!t.empty())Ws.push_back(atoi(t.c_str())); }
    std::vector<double> Gs; { const char*e=getenv("UM_G"); std::string s=e?e:"0.2,0.5,0.75";
        std::stringstream ss(s); std::string t; while(std::getline(ss,t,','))if(!t.empty())Gs.push_back(atof(t.c_str())); }
    std::vector<int> Ls; { const char*e=getenv("UM_LEGS"); std::string s=e?e:"8";
        std::stringstream ss(s); std::string t; while(std::getline(ss,t,','))if(!t.empty())Ls.push_back(atoi(t.c_str())); }

    std::vector<Bar> b=load(); if(b.empty())return 1;
    std::printf("HONEST-ENTRY-BASIS CERT — coin=%s early=%d bars=%zu thr=%.2f%% confirm=%.0fbp anchor=%d RT=%.0f/%.0fbp (entry re-based to le*(1+confirm))\n",
        getenv("UM_COIN")?getenv("UM_COIN"):"ETH", g_early, b.size(), thr*100, g_confirm, g_anchor, rt, rt*2);
    std::printf("%-3s %-4s %-4s | %5s %8s %5s %9s | %5s %8s | %8s %8s | %8s %8s | %s\n",
        "W","g","leg","n","net%","PF","worst_bp","nNeg","sumNeg%","H1%","H2%","floorMinBp","2xnet%","GATE(base;2x; omit22)");
    for(int L:Ls)for(int W:Ws)for(double g:Gs){
        Agg a  = run(b,W,thr,g,L,rt);
        Agg a2 = run(b,W,thr,g,L,rt*2.0);
        double net_x22  = a.net  - a.yr[yidx(2022)];
        double net2_x22 = a2.net - a2.yr[yidx(2022)];
        bool gate = net_x22>0 && a.pf>=1.3 && a.h1>0 && a.h2>0 && net2_x22>0 && a2.pf>=1.3;
        std::printf("%-3d %-4.2f %-4d | %5d %+8.0f %5.2f %+9.1f | %5d %+8.1f | %+8.0f %+8.0f | %+8.1f %+8.0f | %s\n",
            W,g,L, a.n,a.net,a.pf,a.worst, a.neg,a.negsum, a.h1,a.h2, a.floormin,a2.net, gate?"PASS":"fail");
    }
    return 0;
}
