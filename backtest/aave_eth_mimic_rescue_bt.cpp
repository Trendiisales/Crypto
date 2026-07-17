// aave_eth_mimic_rescue_bt.cpp — AAVE/ETH floored-mimic RESCUE sweep. S-2026-07-17.
//
// Context: AAVE-PJ4W1 / ETH-PJ7W24 parent-driven floored mimics = certified NO-GO
// (AAVE_GRT_PJ_MIMIC_FINDINGS_2026-07-17.md). Operator order: find a VIABLE floored
// companion form for AAVE and ETH in the UNSWEPT lever space — SELF-TRIGGERING
// mimic_floor cells (SWEET pattern, no parent), W x thr x g x confirm x legs grid,
// built on the 17c BE-floor-on-open FOUNDATION recipe (confirm>=2xRT, anchor le=epx,
// reclip=0). Judged STANDALONE (feedback-companion-independent-engine).
//
// Derived from eth_ujmimic_15_becascade_bt.cpp (REAL live UpJumpLadderCompanion,
// net_bp_real, honest worse-of fills — book_mimic_stop_ books the ACTUAL fill).
// Deltas: UM_START year filter (default 2023 = the standard long-only gate window,
// 2022 omitted by construction), UM_THR comma list, gate adds n>=30, legs default 1,4.
//
// Build: clang++ -std=c++17 -O2 -I/Users/jo/ChimeraCrypto/include aave_eth_mimic_rescue_bt.cpp -o aave_eth_mimic_rescue_bt
// Env:   UM_COIN(ETH) UM_RT(28) UM_START(2023) UM_THR("0.02,...") UM_W("1,4,...")
//        UM_G("0.2,...") UM_LEGS("1,4") UM_CONFIRM(60) UM_ANCHOR(1) UM_LOSSCUT(0)

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
#include "core/UpJumpLadderCompanion.hpp"
using chimera::UpJumpLadderCompanion;

struct Bar { int64_t ts; double o,h,l,c; };
static std::vector<std::string> split(const std::string&s){std::vector<std::string>v;std::stringstream ss(s);std::string t;while(std::getline(ss,t,','))v.push_back(t);return v;}
static std::string g_coin="ETH";
static std::vector<Bar> load(int start_year){
    std::vector<Bar> b; std::ifstream f("/Users/jo/Crypto/backtest/data/"+g_coin+"USDT_1h.csv");
    if(!f){std::fprintf(stderr,"no %s data\n",g_coin.c_str());return b;} std::string ln; std::getline(f,ln);
    // start_year filter: gate window >=2023 (2022 omitted — long-only crypto standard)
    struct tm t0{}; t0.tm_year=start_year-1900; t0.tm_mday=1; int64_t cut=(int64_t)timegm(&t0)*1000;
    while(std::getline(f,ln)){auto v=split(ln);if(v.size()<5)continue;Bar x;x.ts=(int64_t)std::stoll(v[0]);
        x.o=std::stod(v[1]);x.h=std::stod(v[2]);x.l=std::stod(v[3]);x.c=std::stod(v[4]);
        if(x.c>0 && x.ts>=cut)b.push_back(x);} return b;
}
static int year_of(int64_t ms){time_t t=(time_t)(ms/1000);struct tm g;gmtime_r(&t,&g);return 1900+g.tm_year;}

struct Clip { int64_t ts; double net_bp; };
struct Agg { int n; double net,pf,worst,h1,h2,floormin,top1; int neg; double negsum; double yr[8]; };
static int yidx(int y){int k=y-2021;return k<0?0:(k>7?7:k);}

static double g_losscut=0.0;
static double g_confirm=60.0;  // BE-ENTRY confirm_bp >= 2x measured RT (28bp) — 17c foundation
static int    g_anchor=1;      // confirm_anchor_epx: le=epx on open => floored ON OPEN
static Agg run(const std::vector<Bar>& b, int W, double thr, double g, int legs, double rt){
    UpJumpLadderCompanion::Config c;
    c.parent_tag="SELF"; c.tag=g_coin+"-RESCUE"; c.symbol=g_coin+"usdt";
    c.det_w=W; c.det_thr=thr; c.tf_secs=3600; c.round_trip_bp=rt;
    c.mimic_floor=true; c.mimic_stagger=(legs>1); c.stagger_mode=1; c.stagger_be_bp=20.0;
    c.reclip_pct=0.0; c.loss_cut_bp=g_losscut; c.confirm_bp=g_confirm; c.be_floor=false;
    c.confirm_anchor_epx=(g_anchor!=0);
    c.mimic_giveback=g;
    c.tight={0.2,0,0.0,0,0.0}; c.wide={0.2,0,0.0,0,0.0};
    for(int k=2;k<legs;k++) c.extra_base.push_back({0.2,0,0.0,0,0.0});
    c.cap=legs;

    std::vector<Clip> rows; int64_t cur=0;
    fflush(stdout); int saved=dup(1); {int dn=open("/dev/null",O_WRONLY);dup2(dn,1);close(dn);}
    UpJumpLadderCompanion eng(c);
    eng.set_on_clip([&](const UpJumpLadderCompanion::ClipRecord& r){ rows.push_back({cur, r.net_bp_real}); });
    for(const auto& k : b){ cur=k.ts; eng.stop_check_only(k.l,k.ts); eng.observe(true,0.0,k.c,k.ts); }
    if(!b.empty()){ cur=b.back().ts; eng.observe(false,0.0,b.back().c,b.back().ts+c.tf_secs*1000+2000); }
    fflush(stdout); dup2(saved,1); close(saved);

    std::sort(rows.begin(),rows.end(),[](const Clip&a,const Clip&b){return a.ts<b.ts;});
    Agg a{}; double gw=0,gl=0; a.floormin=1e18; for(int i=0;i<8;i++)a.yr[i]=0;
    for(size_t k=0;k<rows.size();k++){ double p=rows[k].net_bp; a.net+=p/100.0;
        if(p>0)gw+=p; else{gl-=p;a.neg++;a.negsum+=p/100.0;} if(p<a.worst)a.worst=p;
        if(p<a.floormin)a.floormin=p; if(p>a.top1)a.top1=p; a.yr[yidx(year_of(rows[k].ts))]+=p/100.0;
        if(k<rows.size()/2)a.h1+=p/100.0; else a.h2+=p/100.0; }
    a.n=(int)rows.size(); a.pf=gl>0?gw/gl:(gw>0?999:0); if(rows.empty())a.floormin=0;
    return a;
}

int main(){
    if(const char* cn=getenv("UM_COIN")) g_coin=cn;
    double rt = getenv("UM_RT")?atof(getenv("UM_RT")):28.0;
    int start = getenv("UM_START")?atoi(getenv("UM_START")):2023;
    g_losscut = getenv("UM_LOSSCUT")?atof(getenv("UM_LOSSCUT")):0.0;
    g_confirm = getenv("UM_CONFIRM")?atof(getenv("UM_CONFIRM")):60.0;
    g_anchor  = getenv("UM_ANCHOR")?atoi(getenv("UM_ANCHOR")):1;
    std::vector<double> Ts; { const char*e=getenv("UM_THR"); std::string s=e?e:"0.02,0.03,0.04,0.05,0.07";
        std::stringstream ss(s); std::string t; while(std::getline(ss,t,','))if(!t.empty())Ts.push_back(atof(t.c_str())); }
    std::vector<int> Ws; { const char*e=getenv("UM_W"); std::string s=e?e:"1,4,8,12,24,48";
        std::stringstream ss(s); std::string t; while(std::getline(ss,t,','))if(!t.empty())Ws.push_back(atoi(t.c_str())); }
    std::vector<double> Gs; { const char*e=getenv("UM_G"); std::string s=e?e:"0.2,0.3,0.4,0.5,0.6,0.75,0.85,1.0";
        std::stringstream ss(s); std::string t; while(std::getline(ss,t,','))if(!t.empty())Gs.push_back(atof(t.c_str())); }
    std::vector<int> Ls; { const char*e=getenv("UM_LEGS"); std::string s=e?e:"1,4";
        std::stringstream ss(s); std::string t; while(std::getline(ss,t,','))if(!t.empty())Ls.push_back(atoi(t.c_str())); }

    std::vector<Bar> b=load(start); if(b.empty())return 1;
    std::printf("%s SELF-TRIGGER FLOORED MIMIC RESCUE — bars=%zu (>=%d) RT=%.0f/%.0fbp confirm=%.0f anchor=%d lc=%.0f\n",
        g_coin.c_str(), b.size(), start, rt, rt*2, g_confirm, g_anchor, g_losscut);
    std::printf("%-3s %-5s %-4s %-4s | %5s %8s %5s %9s | %5s %8s | %8s %8s | %10s %8s %5s | %6s %6s %6s %6s %6s %6s | %s\n",
        "W","thr%","g","leg","n","net%","PF","worst_bp","nNeg","sumNeg%","H1%","H2%","floorMinBp","2xnet%","top1%","2021","2022","2023","2024","2025","2026","GATE(base;2x;n>=30)");
    for(int L:Ls)for(double thr:Ts)for(int W:Ws)for(double g:Gs){
        Agg a  = run(b,W,thr,g,L,rt);
        Agg a2 = run(b,W,thr,g,L,rt*2.0);
        bool gate = a.n>=30 && a.net>0 && a.pf>=1.3 && a.h1>0 && a.h2>0 && a2.net>0 && a2.pf>=1.3;
        double top1s = a.net>0 ? (a.top1/100.0)/a.net*100.0 : 0.0;
        std::printf("%-3d %-5.1f %-4.2f %-4d | %5d %+8.0f %5.2f %+9.1f | %5d %+8.1f | %+8.0f %+8.0f | %+10.1f %+8.0f %5.0f | %+6.0f %+6.0f %+6.0f %+6.0f %+6.0f %+6.0f | %s\n",
            W,thr*100,g,L, a.n,a.net,a.pf,a.worst, a.neg,a.negsum, a.h1,a.h2, a.floormin,a2.net,top1s,
            a.yr[yidx(2021)],a.yr[yidx(2022)],a.yr[yidx(2023)],a.yr[yidx(2024)],a.yr[yidx(2025)],a.yr[yidx(2026)], gate?"PASS":"fail");
    }
    std::printf("\nGATE = standalone own-book: n>=30 & net>0 & PF>=1.3 & WF-H1>0 & WF-H2>0 at base AND 2x cost. Window >=%d.\n", start);
    return 0;
}
