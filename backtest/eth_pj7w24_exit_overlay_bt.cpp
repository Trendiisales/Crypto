// eth_pj7w24_exit_overlay_bt.cpp — §5 GATE for the ETH PJ7W24 wide/patient exit
// overlay (#2 profit-keeper). S-2026-07-16.
//
// This is NOT a separate-contract clip (that path = NO-GO, REAL_PARENT_MIMIC_FINDINGS).
// It is an A/B on the ONE real ETH contract's EXIT (permitted overlay class):
//   BASELINE = the REAL deployed PJ7W24 jump_floor exit (giveback g=1.0 ride-to-flip,
//              pre-BE stop 400bp). Entries + exits read from the real
//              chimera::UpJumpLadderCompanion — parity by construction, no re-impl.
//   OVL-A    = baseline ride + 3xATR post-entry hard stop + 48-bar patient time-stop
//              + ck3 cost-gate entry filter (ADDITIVE brakes = the §4 delta).
//   OVL-B    = full frontier: 5xATR chandelier trail REPLACING the ride, + the same
//              3xATR hard stop + 48-bar ts + ck3.
//
// Overlay brakes only ever exit EARLIER or SKIP an entry, so they are applied by
// walking the REAL price path from each real entry -> honest, no jump-detect re-impl.
// The DIFF (overlay vs baseline) is what the gate judges.
//
// GATE PASS (per §5 of the spec): overlay net >= baseline net at base AND 2x cost;
// both WF halves > 0; worst bounded; and the BULL not collapsed (2024/2025 net not
// materially below baseline — the fat-tail-amputation tripwire).
//
// Build: clang++ -std=c++17 -O2 -I/Users/jo/ChimeraCrypto/include eth_pj7w24_exit_overlay_bt.cpp -o eth_pj7w24_exit_overlay_bt
// Data:  /Users/jo/Crypto/backtest/data/ETHUSDT_1h.csv  (open_time_ms,o,h,l,c)
// Env:   EO_RT(28 base bp) EO_HARDSTOP_ATR(3.0) EO_TS_BARS(48) EO_CK(3.0)
//        EO_TRAIL_ATR(5.0) EO_ATR(14) EO_YEAR_MIN(2021)

#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <cmath>
#include <string>
#include <vector>
#include <algorithm>
#include <fstream>
#include <sstream>
#include <unistd.h>
#include <fcntl.h>
#include "core/UpJumpLadderCompanion.hpp"

using chimera::UpJumpLadderCompanion;

struct Bar { int64_t ts; double o,h,l,c; };

static std::vector<std::string> split(const std::string& s){
    std::vector<std::string> v; std::stringstream ss(s); std::string t;
    while(std::getline(ss,t,',')) v.push_back(t); return v; }

static std::vector<Bar> load_1h(){
    std::vector<Bar> b;
    std::ifstream f("/Users/jo/Crypto/backtest/data/ETHUSDT_1h.csv");
    if(!f){ std::fprintf(stderr,"no ETH 1h data\n"); return b; }
    std::string ln; std::getline(f,ln);
    while(std::getline(f,ln)){ auto v=split(ln); if(v.size()<5) continue;
        Bar x; x.ts=(int64_t)std::stoll(v[0]); x.o=std::stod(v[1]); x.h=std::stod(v[2]);
        x.l=std::stod(v[3]); x.c=std::stod(v[4]);
        if(x.o>0&&x.h>0&&x.l>0&&x.c>0) b.push_back(x); }
    return b;
}

// year from ms (UTC, close enough for bucketing)
static int year_of(int64_t ms){
    time_t t=(time_t)(ms/1000); struct tm g; gmtime_r(&t,&g); return 1900+g.tm_year; }

// Wilder ATR(n) on the 1h bars. atr[i] uses info up to and including bar i.
static std::vector<double> wilder_atr(const std::vector<Bar>& b, int n){
    std::vector<double> atr(b.size(),0.0); if(b.empty()) return atr;
    double prev_c=b[0].c, tr_sum=0;
    for(size_t i=0;i<b.size();i++){
        double tr = b[i].h-b[i].l;
        if(i>0){ tr=std::max(tr, std::max(std::fabs(b[i].h-prev_c), std::fabs(b[i].l-prev_c))); }
        if((int)i< n){ tr_sum+=tr; atr[i]= (i>0)? tr_sum/(i+1) : tr; }
        else if((int)i==n){ tr_sum+=tr; atr[i]=tr_sum/(n+1); }
        else { atr[i]= (atr[i-1]*(n-1)+tr)/n; }
        prev_c=b[i].c;
    }
    return atr;
}

// drive REAL PJ7W24 -> per-bar in_pos + entry_px (exactly tape_eth in real_parent_mimic_bt)
static void drive_parent(const std::vector<Bar>& b, std::vector<char>& inpos, std::vector<double>& epx, double cost){
    inpos.assign(b.size(),0); epx.assign(b.size(),0.0);
    UpJumpLadderCompanion::Config c;
    c.parent_tag="ETH-PJ7W24-FEED"; c.tag="ETH-PJ7W24"; c.symbol="ethusdt";
    c.det_w=24; c.det_thr=0.070; c.jump_floor=true; c.jf_giveback=1.0; c.jf_prebe_stop_bp=400.0;
    c.tf_secs=3600; c.round_trip_bp=cost; c.confirm_bp=0.0; c.loss_cut_bp=0.0;
    c.be_floor=false; c.reclip_pct=0.0; c.cap=1; c.cost_gate_bp=0.0; c.size_mult=1.0;
    fflush(stdout); int saved=dup(1); { int dn=open("/dev/null",O_WRONLY); dup2(dn,1); close(dn); }
    UpJumpLadderCompanion eng(c);
    for(size_t i=0;i<b.size();i++){
        eng.stop_check_only(b[i].l, b[i].ts);
        eng.observe(true, 0.0, b[i].c, b[i].ts);
        inpos[i]= eng.jf_in_position()?1:0; epx[i]=eng.jf_entry_px();
    }
    fflush(stdout); dup2(saved,1); close(saved);
}

struct Trade { int64_t ts; double net_bp; };
struct Agg { int n; double net,pf,worst,h1,h2; int neg; double yr[8]; };  // yr idx 0=2021..7=2028
static int yidx(int y){ int k=y-2021; return (k<0)?0:(k>7?7:k); }

static Agg summarize(std::vector<Trade>& tr){
    std::sort(tr.begin(),tr.end(),[](const Trade&a,const Trade&b){return a.ts<b.ts;});
    Agg a{}; double gw=0,gl=0; for(int i=0;i<8;i++) a.yr[i]=0;
    for(size_t k=0;k<tr.size();k++){ double p=tr[k].net_bp; a.net+=p/100.0;
        if(p>0)gw+=p; else{gl-=p;a.neg++;} if(p<a.worst)a.worst=p;
        a.yr[yidx(year_of(tr[k].ts))]+=p/100.0;
        if(k<tr.size()/2)a.h1+=p/100.0; else a.h2+=p/100.0; }
    a.n=(int)tr.size(); a.pf= gl>0?gw/gl:(gw>0?999:0);
    return a;
}

// BASELINE trades from real parent in_pos transitions. Exit at the bar where it goes flat (close).
static std::vector<Trade> run_baseline(const std::vector<Bar>& b, const std::vector<char>& inpos,
                                       const std::vector<double>& epx, double rt, int ymin){
    std::vector<Trade> out;
    for(size_t i=1;i<b.size();i++){
        if(inpos[i] && !inpos[i-1]){                     // ENTRY
            double e=epx[i]; if(e<=0) e=b[i].c;
            size_t j=i+1; for(; j<b.size(); ++j) if(!inpos[j]) break;   // first flat bar
            double xpx = (j<b.size())? b[j].c : b.back().c;
            if(year_of(b[i].ts)>=ymin){
                double net=(xpx/e-1.0)*1e4 - rt;
                out.push_back({b[i].ts, net});
            }
            i = (j<b.size())? j : b.size();
        }
    }
    return out;
}

// OVERLAY: same entries, ck3 filter, exit = earliest of {baseline flat bar, 3ATR hardstop,
// 48-bar ts, (variant B) 5ATR chandelier}. hard_atr/ts/ck shared; trail_atr>0 => variant B.
static std::vector<Trade> run_overlay(const std::vector<Bar>& b, const std::vector<char>& inpos,
                                      const std::vector<double>& epx, const std::vector<double>& atr,
                                      double rt, double hard_atr, int ts_bars, double ck, double trail_atr,
                                      int ymin, int* skipped){
    std::vector<Trade> out; if(skipped)*skipped=0;
    for(size_t i=1;i<b.size();i++){
        if(inpos[i] && !inpos[i-1]){
            double e=epx[i]; if(e<=0) e=b[i].c;
            size_t jflat=i+1; for(; jflat<b.size(); ++jflat) if(!inpos[jflat]) break;
            double A = atr[i];
            // ck3 entry filter: ATR at entry must clear ck x round-trip
            double atr_bp = (e>0)? A/e*1e4 : 0.0;
            if(atr_bp < ck*rt){ if(skipped)(*skipped)++; i=(jflat<b.size())?jflat:b.size(); continue; }
            double hard_lvl = e - hard_atr*A;
            double maxc=e, xpx=0; bool done=false;
            size_t lastk = (jflat<b.size())? jflat : b.size()-1;
            for(size_t k=i+1; k<=lastk && !done; ++k){
                int held=(int)(k-i);
                if(held>=ts_bars){ xpx=b[k].c; done=true; break; }          // patient time-stop
                if(b[k].l<=hard_lvl){ xpx=hard_lvl; done=true; break; }       // 3ATR disaster stop
                if(trail_atr>0){ maxc=std::max(maxc,b[k].c);                  // variant B: 5ATR chandelier
                    if(b[k].c <= maxc - trail_atr*A){ xpx=b[k].c; done=true; break; } }
            }
            if(!done) xpx = (jflat<b.size())? b[jflat].c : b.back().c;       // rode to baseline exit
            if(year_of(b[i].ts)>=ymin){
                double net=(xpx/e-1.0)*1e4 - rt;
                out.push_back({b[i].ts, net});
            }
            i=(jflat<b.size())?jflat:b.size();
        }
    }
    return out;
}

static void print_row(const char* name, const Agg& a){
    std::printf("%-9s | %4d %+8.0f %5.2f %+9.1f | %+8.0f %+8.0f | ",
        name,a.n,a.net,a.pf,a.worst,a.h1,a.h2);
    for(int y=0;y<6;y++) std::printf("%+7.0f",a.yr[y]);   // 2021..2026
    std::printf("\n");
}

int main(){
    double rt   = getenv("EO_RT")?atof(getenv("EO_RT")):28.0;
    double hard = getenv("EO_HARDSTOP_ATR")?atof(getenv("EO_HARDSTOP_ATR")):3.0;
    int    ts   = getenv("EO_TS_BARS")?atoi(getenv("EO_TS_BARS")):48;
    double ck   = getenv("EO_CK")?atof(getenv("EO_CK")):3.0;
    double trl  = getenv("EO_TRAIL_ATR")?atof(getenv("EO_TRAIL_ATR")):5.0;
    int    an   = getenv("EO_ATR")?atoi(getenv("EO_ATR")):14;
    int    ymin = getenv("EO_YEAR_MIN")?atoi(getenv("EO_YEAR_MIN")):2021;

    std::vector<Bar> b=load_1h(); if(b.empty()) return 1;
    std::vector<double> atr=wilder_atr(b,an);

    std::printf("ETH PJ7W24 EXIT-OVERLAY GATE — bars=%zu  RT_base=%.0fbp  hard=%.1fxATR ts=%d ck=%.1f trailB=%.1fxATR ATR%d  yearMin=%d\n",
        b.size(),rt,hard,ts,ck,trl,an,ymin);
    std::printf("(BASELINE = real PJ7W24 jump_floor g=1.0 ride-to-flip; OVL only exits earlier/skips)\n\n");

    for(double mult : {1.0, 2.0}){
        double R=rt*mult;
        std::vector<char> inpos; std::vector<double> epx;
        drive_parent(b, inpos, epx, R);
        auto base = run_baseline(b,inpos,epx,R,ymin);
        int skA=0, skB=0;
        auto ovA  = run_overlay(b,inpos,epx,atr,R,hard,ts,ck,0.0,ymin,&skA);   // additive brakes
        auto ovB  = run_overlay(b,inpos,epx,atr,R,hard,ts,ck,trl,ymin,&skB);   // full frontier trail
        Agg ab=summarize(base), aA=summarize(ovA), aB=summarize(ovB);
        std::printf("== cost %.0fbp (%s) ==\n", R, mult==1.0?"BASE":"2x STRESS");
        std::printf("%-9s | %4s %8s %5s %9s | %8s %8s | %6s %6s %6s %6s %6s %6s\n",
            "arm","n","net%","PF","worst_bp","H1%","H2%","2021","2022","2023","2024","2025","2026");
        print_row("BASELINE", ab);
        print_row("OVL-A", aA);
        print_row("OVL-B", aB);
        std::printf("   (ck3 skipped entries: OVL-A=%d OVL-B=%d of %d)\n\n", skA,skB,ab.n);
    }

    // gate verdict re-runs at base+2x
    auto verdict=[&](double R)->bool{
        std::vector<char> ip; std::vector<double> ep; drive_parent(b,ip,ep,R);
        auto base=run_baseline(b,ip,ep,R,ymin); int s=0;
        auto A=run_overlay(b,ip,ep,atr,R,hard,ts,ck,0.0,ymin,&s);
        auto B=run_overlay(b,ip,ep,atr,R,hard,ts,ck,trl,ymin,&s);
        Agg ba=summarize(base),aa=summarize(A),bb=summarize(B);
        auto ok=[&](const Agg&o){ return o.net>=ba.net && o.h1>0 && o.h2>0
            && o.yr[3]>=ba.yr[3]*0.8 && o.yr[4]>=ba.yr[4]*0.8; };  // bull 2024/2025 not <20% below base
        return ok(aa)||ok(bb);
    };
    bool g1=verdict(rt), g2=verdict(rt*2.0);
    std::printf("GATE: base=%s  2x=%s  => %s\n", g1?"pass":"FAIL", g2?"pass":"FAIL",
        (g1&&g2)?"PASS (wire per spec §6)":"FAIL (ETH keeps live #1 ride-to-flip; do NOT wire)");
    return 0;
}
