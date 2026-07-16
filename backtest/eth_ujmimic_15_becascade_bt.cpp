// eth_ujmimic_15_becascade_bt.cpp — operator's EXACT spec, tested STANDALONE. S-2026-07-16.
//
// Operator (2026-07-16): "if the overall price goes up by 1.5% enter a trade, lock it in;
// as soon as we get to BE open ANOTHER mimic trade; the mimic exits when the price reverses;
// the mimic has NO effect on the overall trade." => a SELF-TRIGGERING, BE-floored, BE-cascade
// restacking, reversal-exit long book on ETH. Judged STANDALONE (its OWN book net after ITS
// OWN cost), NEVER vs a parent — feedback-companion-independent-engine + feedback-test-operator-
// spec-before-verdict. This does NOT touch any parent; it is an additive book.
//
// Expressed in the REAL live UpJumpLadderCompanion (parity by construction, net_bp_real):
//   det_w=W det_thr=0.015          -> +1.5% up-move trigger (internal detector, self-triggering)
//   mimic_floor=true               -> BE floor le*(1+RT): NEVER closes negative after arming ("lock in")
//   mimic_stagger + stagger_mode=1 -> BE_CASCADE: release the next leg the moment the current one
//                                     reaches BE (mfe>=stagger_be_bp) = "as soon as BE, open another"
//   reclip_pct=0                   -> OFF (required for the never-negative guarantee under cascade)
//   mimic_giveback=g               -> exit on reversal (g=1.0 = reversal-only)
// L legs total (2 base + L-2 extra_base tiers). Long-only spot. MEASURED cost 28/56bp.
//
// Build: clang++ -std=c++17 -O2 -I/Users/jo/ChimeraCrypto/include eth_ujmimic_15_becascade_bt.cpp -o eth_ujmimic_15_becascade_bt
// Data:  /Users/jo/Crypto/backtest/data/ETHUSDT_1h.csv
// Env:   UM_RT(28) UM_THR(0.015) UM_W(comma list) UM_G(comma list) UM_LEGS(comma list)

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

// drive the REAL engine self-detecting over ETH; return the STANDALONE mimic book
static double g_losscut=0.0;
static Agg run(const std::vector<Bar>& b, int W, double thr, double g, int legs, double rt){
    UpJumpLadderCompanion::Config c;
    c.parent_tag="SELF"; c.tag="ETH-UJ15"; c.symbol="ethusdt";
    c.det_w=W; c.det_thr=thr; c.tf_secs=3600; c.round_trip_bp=rt;
    c.mimic_floor=true; c.mimic_stagger=true; c.stagger_mode=1; c.stagger_be_bp=20.0;
    c.reclip_pct=0.0; c.loss_cut_bp=g_losscut; c.confirm_bp=0.0; c.be_floor=false;
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
        if(p<a.floormin)a.floormin=p; a.yr[yidx(year_of(rows[k].ts))]+=p/100.0;
        if(k<rows.size()/2)a.h1+=p/100.0; else a.h2+=p/100.0; }
    a.n=(int)rows.size(); a.pf=gl>0?gw/gl:(gw>0?999:0); if(rows.empty())a.floormin=0;
    return a;
}

int main(){
    double rt = getenv("UM_RT")?atof(getenv("UM_RT")):28.0;
    double thr= getenv("UM_THR")?atof(getenv("UM_THR")):0.015;
    g_losscut = getenv("UM_LOSSCUT")?atof(getenv("UM_LOSSCUT")):0.0;
    std::vector<int> Ws; { const char*e=getenv("UM_W"); std::string s=e?e:"1,4,12,24";
        std::stringstream ss(s); std::string t; while(std::getline(ss,t,','))if(!t.empty())Ws.push_back(atoi(t.c_str())); }
    std::vector<double> Gs; { const char*e=getenv("UM_G"); std::string s=e?e:"1.0,0.75,0.5";
        std::stringstream ss(s); std::string t; while(std::getline(ss,t,','))if(!t.empty())Gs.push_back(atof(t.c_str())); }
    std::vector<int> Ls; { const char*e=getenv("UM_LEGS"); std::string s=e?e:"2,4";
        std::stringstream ss(s); std::string t; while(std::getline(ss,t,','))if(!t.empty())Ls.push_back(atoi(t.c_str())); }

    std::vector<Bar> b=load(); if(b.empty())return 1;
    std::printf("ETH SELF-TRIGGER BE-CASCADE MIMIC — operator exact spec — bars=%zu thr=%.1f%% RT=%.0f/%.0fbp\n",
        b.size(), thr*100, rt, rt*2);
    std::printf("(enter on +%.1f%% up-move; floor at BE never-neg; open next leg AT BE; exit on reversal; STANDALONE book)\n", thr*100);
    std::printf("%-3s %-4s %-4s | %5s %8s %5s %9s | %5s %8s | %8s %8s | %8s %8s | %6s %6s %6s %6s %6s %6s | %s\n",
        "W","g","leg","n","net%","PF","worst_bp","nNeg","sumNeg%","H1%","H2%","floorMinBp","2xnet%","2021","2022","2023","2024","2025","2026","GATE(base;2x; omit22)");
    for(int L:Ls)for(int W:Ws)for(double g:Gs){
        Agg a  = run(b,W,thr,g,L,rt);
        Agg a2 = run(b,W,thr,g,L,rt*2.0);
        // standalone gate, OMIT 2022 (long-only spot can't short a bear — feedback-crypto-omit-2022-longonly)
        double net_x22  = a.net  - a.yr[yidx(2022)];
        double net2_x22 = a2.net - a2.yr[yidx(2022)];
        bool gate = net_x22>0 && a.pf>=1.3 && a.h1>0 && a.h2>0 && net2_x22>0 && a2.pf>=1.3;
        std::printf("%-3d %-4.2f %-4d | %5d %+8.0f %5.2f %+9.1f | %5d %+8.1f | %+8.0f %+8.0f | %+8.1f %+8.0f | %+6.0f %+6.0f %+6.0f %+6.0f %+6.0f %+6.0f | %s\n",
            W,g,L, a.n,a.net,a.pf,a.worst, a.neg,a.negsum, a.h1,a.h2, a.floormin,a2.net,
            a.yr[0],a.yr[1],a.yr[2],a.yr[3],a.yr[4],a.yr[5], gate?"PASS":"fail");
    }
    std::printf("\nfloorMinBp = worst single clip net (armed legs floored at BE => >=~0; pre-BE reversals = small neg = admission cost).\n");
    std::printf("GATE = own book standalone: net>0 & PF>=1.3 & both WF halves>0, at base AND 2x cost, OMITTING 2022 bear.\n");
    return 0;
}
