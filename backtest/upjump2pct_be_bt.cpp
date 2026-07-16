// ─────────────────────────────────────────────────────────────────────────────
// upjump2pct_be_bt — OPERATOR SPEC 2026-07-14: independent LONG SPOT engine.
//   "trade these when they jump 2% ... trade until it reverses then exit ...
//    make sure we cover BE ... use a bracket if you have to"
//
// Mechanism (per coin, one position max):
//   DETECT  j = c[i]/c[i-W] - 1 >= thr (thr = 2% uniform mandate, 52c0d31 lever)
//   ENTRY   next bar open (spot market buy). Re-entry only after j < thr once
//           since last exit (fresh jump required).
//   BRACKET pre-BE stop s% below entry (exchange stop order, intrabar fill,
//           gap-through fills at open). s=0 → reversal-only, no hard stop.
//   BE-FLOOR once close covers entry*(1+RT) (cost covered), stop rises to
//           entry*(1+RT) → trade can never close negative after arming.
//   TRAIL   engine updates stop each 1h close: max(floor, E*(1+mfe*(1-g))) —
//           peak-profit giveback g of MFE (close-based mfe). Intrabar fill.
//   REVERSAL j <= -thr at close → market sell next open. End flush last close.
//
// Costs: 20bp Binance RT debited per trade; 2x-cost = FULL re-sim at 40bp.
// Gate (long-only corrected, feedback-crypto-omit-2022-longonly):
//   PASS = net>0 AND PF>=1.3 AND WF-H1>0 AND WF-H2>0 AND 2xcost-net>0.
//   y2022 bleed SHOWN, not gated. Pre-BE stop losses quantified explicitly
//   (the "can trade into a loss" exposure — operator must see the number).
// Judged STANDALONE (independent engine — never vs WIDE / never vs mimic).
//
// Build: g++ -O2 -std=c++17 upjump2pct_be_bt.cpp -o upjump2pct_be_bt
// Run:   ./upjump2pct_be_bt sweep          (config ridge, portfolio-level)
//        ./upjump2pct_be_bt detail W thr_pct s_pct g   (per-coin table + gate)
// ─────────────────────────────────────────────────────────────────────────────
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <cmath>
#include <fstream>
#include <sstream>
#include <string>
#include <vector>
#include <map>
#include <algorithm>

static const char* COINS[] = {"AAVE","ADA","AVAX","BCH","BNB","BTC","DOGE","ETH","GRT",
                              "LDO","LINK","LTC","NEAR","OP","SOL","TRX","UNI","XLM","XRP",
                              "ONDO"};
static const int NC = 20;

struct Bars { std::vector<int64_t> ts; std::vector<double> o,h,l,c; int N=0; };

static Bars load(const std::string& coin){
    Bars b; std::ifstream f("data/"+coin+"USDT_1h.csv");
    if(!f){ std::fprintf(stderr,"[skip] no 1h data %s\n",coin.c_str()); return b; }
    std::string ln; std::getline(f,ln);
    while(std::getline(f,ln)){
        std::stringstream ss(ln); std::string t; std::vector<std::string> v;
        while(std::getline(ss,t,',')) v.push_back(t);
        if(v.size()<5) continue;
        b.ts.push_back((int64_t)std::stoll(v[0]));
        b.o.push_back(std::stod(v[1])); b.h.push_back(std::stod(v[2]));
        b.l.push_back(std::stod(v[3])); b.c.push_back(std::stod(v[4]));
    }
    b.N=(int)b.ts.size(); return b;
}

struct Trade { int64_t ets; double net_bp; bool prebe_stop; int ei; };

struct Res {
    std::vector<Trade> tr;
    double net=0, wins=0, losses=0, worst=0, maxdd=0;
    int n=0, n_prebe=0; double prebe_bp=0;
    double y2022=0, wf1=0, wf2=0;
};

static int year_of(int64_t ms){ time_t s=ms/1000; struct tm g; gmtime_r(&s,&g); return g.tm_year+1900; }

// run one coin, one config. i0/i1 bound the ENTRY-eligible bar range (WF slicing);
// management runs to data end (trades owned by entry slice).
static void run_coin(const Bars& b,int W,double thr,double s_pre,double gb,double rt_bp,
                     int i0,int i1,Res& R){
    if(b.N<W+10) return;
    double rt=rt_bp*1e-4;
    bool armed=true;                       // may enter on first fresh jump
    int i=std::max(W,i0);
    for(; i<i1-1; i++){
        double j=b.c[i]/b.c[i-W]-1.0;
        if(!armed){ if(j<thr) armed=true; continue; }
        if(j<thr) continue;
        // ENTRY next open
        int e=i+1; double E=b.o[e];
        double floor_px=E*(1.0+rt);        // BE incl. cost
        bool floored=false; double mfe=0;  // close-based mfe (fraction)
        double stop=(s_pre>0)? E*(1.0-s_pre) : -1.0;
        int x=-1; double xp=0; bool prebe=false;
        for(int k=e;k<b.N;k++){
            // 1) resting stop order (intrabar, gap-through at open)
            if(stop>0 && b.l[k]<=stop){ x=k; xp=std::min(stop,b.o[k]); prebe=!floored; break; }
            // 2) close-based state update
            double fav=b.c[k]/E-1.0;
            if(fav>mfe) mfe=fav;
            if(!floored && fav>=rt){ floored=true; }
            double ns=stop;
            if(floored){ ns=std::max(floor_px, E*(1.0+mfe*(1.0-gb))); }
            if(ns>stop) stop=ns;
            // 3) reversal at close → market out next open
            double jk=b.c[k]/b.c[k-W>=0?k-W:0]-1.0;
            if(k-W>=0 && jk<=-thr){ x=std::min(k+1,b.N-1); xp=b.o[x]; if(x==k) xp=b.c[k]; break; }
            if(k==b.N-1){ x=k; xp=b.c[k]; break; }   // end flush
        }
        if(x<0) break;
        double net=(xp/E-1.0)*1e4 - rt_bp;
        R.tr.push_back({b.ts[e],net,prebe,e});
        // resume scan after exit; require fresh jump
        i=x; armed=false;
    }
    // aggregate
    double cum=0, pk=0;
    for(auto& t:R.tr){
        R.net+=t.net_bp; R.n++;
        if(t.net_bp>=0) R.wins+=t.net_bp; else R.losses+=-t.net_bp;
        if(t.net_bp<R.worst) R.worst=t.net_bp;
        if(t.prebe_stop){ R.n_prebe++; R.prebe_bp+=t.net_bp; }
        if(year_of(t.ets)==2022) R.y2022+=t.net_bp;
        cum+=t.net_bp; if(cum>pk) pk=cum; if(pk-cum>R.maxdd) R.maxdd=pk-cum;
    }
}

static Res run_full(const Bars& b,int W,double thr,double s,double g,double rt){
    Res R; run_coin(b,W,thr,s,g,rt,W,b.N,R);
    // WF halves by entry index
    int mid=b.N/2;
    for(auto& t:R.tr){ if(t.ei<mid) R.wf1+=t.net_bp; else R.wf2+=t.net_bp; }
    return R;
}

int main(int argc,char**argv){
    std::string mode=argc>1?argv[1]:"sweep";
    std::map<std::string,Bars> B;
    for(int c=0;c<NC;c++) B[COINS[c]]=load(COINS[c]);

    if(mode=="sweep"){
        int Ws[]={1,2,3,4,6,8,12,24};
        double thrs[]={0.02,0.03,0.04};
        double ss[]={0.0,0.01,0.02};
        double gs[]={0.3,0.5};
        std::printf("W thr%% s%% g | port_net_bp port_net_2x nPASS(19) medPF n_tr preBE%% worst_bp\n");
        for(int wi=0;wi<8;wi++) for(auto thr:thrs) for(auto s:ss) for(auto g:gs){
            double pnet=0,pnet2=0; int npass=0; long ntr=0; long npre=0; double worst=0;
            std::vector<double> pfs;
            for(int c=0;c<NC;c++){
                auto& b=B[COINS[c]]; if(!b.N) continue;
                Res r=run_full(b,Ws[wi],thr,s,g,20.0);
                Res r2=run_full(b,Ws[wi],thr,s,g,40.0);
                pnet+=r.net; pnet2+=r2.net; ntr+=r.n; npre+=r.n_prebe;
                if(r.worst<worst) worst=r.worst;
                double pf=r.losses>0? r.wins/r.losses : (r.wins>0?99:0);
                pfs.push_back(pf);
                bool pass = r.net>0 && pf>=1.3 && r.wf1>0 && r.wf2>0 && r2.net>0;
                if(pass) npass++;
            }
            std::sort(pfs.begin(),pfs.end());
            double medpf=pfs.empty()?0:pfs[pfs.size()/2];
            std::printf("%2d %4.1f %4.1f %.1f | %+9.0f %+9.0f %2d %5.2f %6ld %5.1f%% %+7.0f\n",
                Ws[wi],thr*100,s*100,g,pnet,pnet2,npass,medpf,ntr,ntr? 100.0*npre/ntr:0,worst);
        }
        return 0;
    }
    if(mode=="detail"){
        int W=atoi(argv[2]); double thr=atof(argv[3])/100.0, s=atof(argv[4])/100.0, g=atof(argv[5]);
        std::printf("CONFIG: W=%dh thr=%.1f%% preBE-stop=%.1f%% giveback=%.0f%% RT=20bp (2x=40)\n",
            W,thr*100,s*100,g*100);
        std::printf("%-6s %5s %10s %6s %8s %8s %9s %9s %9s %5s %8s %6s | GATE\n",
            "coin","n","net_bp","PF","worst","maxDD","y2022","WF-H1","WF-H2","preBE","preBEbp","2x_net");
        double tot=0,tot2=0; int npass=0;
        for(int c=0;c<NC;c++){
            auto& b=B[COINS[c]]; if(!b.N) continue;
            Res r=run_full(b,W,thr,s,g,20.0);
            Res r2=run_full(b,W,thr,s,g,40.0);
            double pf=r.losses>0? r.wins/r.losses : (r.wins>0?99:0);
            bool pass = r.net>0 && pf>=1.3 && r.wf1>0 && r.wf2>0 && r2.net>0;
            if(pass) npass++;
            tot+=r.net; tot2+=r2.net;
            std::printf("%-6s %5d %+10.0f %6.2f %+8.0f %+8.0f %+9.0f %+9.0f %+9.0f %4d %+8.0f %+7.0f | %s\n",
                COINS[c],r.n,r.net,pf,r.worst,-r.maxdd,r.y2022,r.wf1,r.wf2,
                r.n_prebe,r.prebe_bp,r2.net, pass?"PASS":"fail");
        }
        std::printf("PORTFOLIO net=%+.0fbp 2x=%+.0fbp PASS=%d/19\n",tot,tot2,npass);
        return 0;
    }
    if(mode=="percoin"){
        // Full per-coin lever map. Cell PASS = corrected long-only gate (n>=30).
        // PLATEAU = cell PASS and >=3 of 4 thr/W neighbors have net>0 at 1x cost
        // (kills isolated-ridge cells like ETH W1/2%).
        double thrs[]={0.02,0.025,0.03,0.035,0.04,0.045,0.05,0.055,0.06,0.07,0.08,0.10,0.12};
        int Ws[]={1,2,3,4,6,8,12,24};
        double ss[]={0.0,0.01,0.02}; double gs[]={0.3,0.5,1.0};
        const int NT=13,NW=8;
        for(int c=0;c<NC;c++){
            auto& b=B[COINS[c]]; if(!b.N){ std::printf("%-6s NO-DATA\n",COINS[c]); continue; }
            struct Cell{ double net2=-1e18,net=0,pf=0,worst=0,y22=0; int n=0; bool pass=false,plat=false; int wi=0,ti=0; double s=0,g=0; };
            // net at 1x for every (wi,ti) at each (s,g) — for neighbor test
            static double net1[3][3][NW][NT];
            Cell best;
            for(int si=0;si<3;si++) for(int gi=0;gi<3;gi++)
                for(int wi=0;wi<NW;wi++) for(int ti=0;ti<NT;ti++){
                    Res r=run_full(b,Ws[wi],thrs[ti],ss[si],gs[gi],20.0);
                    net1[si][gi][wi][ti]=r.net;
                }
            for(int si=0;si<3;si++) for(int gi=0;gi<3;gi++)
                for(int wi=0;wi<NW;wi++) for(int ti=0;ti<NT;ti++){
                    Res r=run_full(b,Ws[wi],thrs[ti],ss[si],gs[gi],20.0);
                    if(r.n<30) continue;
                    double pf=r.losses>0? r.wins/r.losses:(r.wins>0?99:0);
                    if(!(r.net>0&&pf>=1.3&&r.wf1>0&&r.wf2>0)) continue;
                    Res r2=run_full(b,Ws[wi],thrs[ti],ss[si],gs[gi],40.0);
                    if(r2.net<=0) continue;
                    int ok=0,tot=0;
                    int dw[]={-1,1,0,0}, dt[]={0,0,-1,1};
                    for(int k=0;k<4;k++){ int w2=wi+dw[k],t2=ti+dt[k];
                        if(w2<0||w2>=NW||t2<0||t2>=NT) continue;
                        tot++; if(net1[si][gi][w2][t2]>0) ok++; }
                    bool plat = tot>0 && ok*4>=tot*3;   // >=75% of existing neighbors positive
                    if(!plat) continue;
                    if(r2.net>best.net2){ best={r2.net,r.net,pf,r.worst,r.y2022,r.n,true,true,wi,ti,ss[si],gs[gi]}; }
                }
            if(best.pass)
                std::printf("%-6s VIABLE  W=%-2d thr=%.1f%% s=%.0f%% g=%.1f | n=%d net=%+.0fbp PF=%.2f 2x=%+.0f worst=%+.0f y22=%+.0f\n",
                    COINS[c],Ws[best.wi],thrs[best.ti]*100,best.s*100,best.g,best.n,best.net,best.pf,best.net2,best.worst,best.y22);
            else std::printf("%-6s NO-CELL (no plateau-validated PASS across 936 lever combos)\n",COINS[c]);
            std::fflush(stdout);
        }
        return 0;
    }
    if(mode=="stopsweep"){
        // stopsweep COIN W thr_pct g — hold the wired cell (W,thr,g), sweep the
        // pre-BE stop s over a fine grid. Full history, full stats at 1x + 2x cost.
        // Ask 2026-07-14: per-trade hard cap re-sweep on the 16 s=0 PJ cells.
        std::string coin=argv[2]; int W=atoi(argv[3]);
        double thr=atof(argv[4])/100.0, g=atof(argv[5]);
        auto& b=B[coin]; if(!b.N){ std::fprintf(stderr,"no data %s\n",coin.c_str()); return 1; }
        double sgrid[]={0.0,0.0025,0.005,0.0075,0.01,0.015,0.02,0.03,0.04,0.05};
        std::printf("%s W=%d thr=%.1f%% g=%.1f\n",coin.c_str(),W,thr*100,g);
        std::printf("  s%%    n   net_bp    PF   worst   maxDD  preBE%% preBE_bp   WF-H1   WF-H2  2x_net | GATE\n");
        for(double s:sgrid){
            Res r=run_full(b,W,thr,s,g,20.0);
            Res r2=run_full(b,W,thr,s,g,40.0);
            double pf=r.losses>0? r.wins/r.losses:(r.wins>0?99:0);
            bool pass = r.n>=30 && r.net>0 && pf>=1.3 && r.wf1>0 && r.wf2>0 && r2.net>0;
            std::printf("%5.2f %4d %+8.0f %5.2f %+7.0f %+7.0f  %5.1f%% %+8.0f %+7.0f %+7.0f %+7.0f | %s\n",
                s*100,r.n,r.net,pf,r.worst,-r.maxdd,r.n?100.0*r.n_prebe/r.n:0,r.prebe_bp,
                r.wf1,r.wf2,r2.net,pass?"PASS":"fail");
        }
        return 0;
    }
    if(mode=="trades"){
        // trades COIN W thr_pct s_pct g — per-trade dump for eyeball verification
        std::string coin=argv[2]; int W=atoi(argv[3]);
        double thr=atof(argv[4])/100.0, s=atof(argv[5])/100.0, g=atof(argv[6]);
        auto& b=B[coin]; if(!b.N){ std::fprintf(stderr,"no data %s\n",coin.c_str()); return 1; }
        Res r=run_full(b,W,thr,s,g,20.0);
        std::printf("%s W=%d thr=%.1f%% s=%.0f%% g=%.1f — %d trades net=%+.0fbp\n",
            coin.c_str(),W,thr*100,s*100,g,r.n,r.net);
        for(auto& t:r.tr){
            time_t sec=t.ets/1000; struct tm gm; gmtime_r(&sec,&gm); char buf[32];
            strftime(buf,sizeof buf,"%Y-%m-%d %H:%M",&gm);
            std::printf("  %s  %+8.1fbp%s\n",buf,t.net_bp,t.prebe_stop?"  [preBE-stop]":"");
        }
        return 0;
    }
    if(mode=="lowthr"){
        // 2026-07-14 LOWTHR STOP-RESCUE (S-2026-07-14av): the 4 stop-compatible
        // survivors of the stopsweep cull (ETH/AAVE/GRT/DOGE) — do LOWER jump
        // thresholds (1.0-3.0%) become viable when rescued by a per-coin fine
        // pre-BE stop + BE-floor? Uniform 2% was 0/19 (pre-BE bleed); the fine
        // stop grid was never run below thr=3. Gate = full corrected long-only
        // + plateau + 2023-26-only recheck + top1<=45% of net. thr<=2.5 answers
        // the ask; thr=3.0 rows are boundary context only.
        const char* CS[]={"ETH","AAVE","GRT","DOGE"};
        double thrs[]={0.010,0.015,0.020,0.025,0.030};
        int Ws[]={1,2,3,4,6,8,12,24};
        double ss[]={0.0025,0.005,0.0075,0.01,0.015,0.02,0.03,0.04,0.05};
        double gs[]={0.3,0.5,1.0};
        const int NT=5,NW=8,NS=9,NG=3;
        struct St{ double net=0,pf=0,wf1=0,wf2=0,worst=0,maxdd=0,y22=0,n2326=0,top1=0; int n=0; };
        static St S[NS][NG][NW][NT];
        for(int ci=0;ci<4;ci++){
            auto& b=B[CS[ci]]; if(!b.N){ std::printf("%s NO-DATA\n",CS[ci]); continue; }
            for(int si=0;si<NS;si++) for(int gi=0;gi<NG;gi++)
                for(int wi=0;wi<NW;wi++) for(int ti=0;ti<NT;ti++){
                    Res r=run_full(b,Ws[wi],thrs[ti],ss[si],gs[gi],20.0);
                    St& s=S[si][gi][wi][ti]; s=St{};
                    s.net=r.net; s.wf1=r.wf1; s.wf2=r.wf2; s.worst=r.worst;
                    s.maxdd=r.maxdd; s.y22=r.y2022; s.n=r.n;
                    s.pf=r.losses>0? r.wins/r.losses:(r.wins>0?99:0);
                    for(auto& t:r.tr){ if(year_of(t.ets)>=2023) s.n2326+=t.net_bp;
                                       if(t.net_bp>s.top1) s.top1=t.net_bp; }
                }
            std::printf("── %s ── PASS cells (thr W s g | n net PF WF1 WF2 2x net23-26 top1%% y22 worst maxDD)\n",CS[ci]);
            int npass_low=0,npass_bnd=0; double best2x=-1e18; char bestln[256]="";
            // gate funnel (diagnostic, printed after the PASS rows)
            int f_tot=0,f_net=0,f_basic=0,f_2326=0,f_top1=0,f_plat=0;
            double bestnet=-1e18; char bestnetln[160]="";
            for(int si=0;si<NS;si++) for(int gi=0;gi<NG;gi++)
                for(int wi=0;wi<NW;wi++) for(int ti=0;ti<NT;ti++){
                    St& s=S[si][gi][wi][ti];
                    f_tot++;
                    if(s.net>bestnet){ bestnet=s.net; std::snprintf(bestnetln,sizeof bestnetln,
                        "%.1f%% W=%d s=%.2f%% g=%.1f n=%d net=%+.0f PF=%.2f WF=%+.0f/%+.0f 23-26=%+.0f top1=%.0f%%",
                        thrs[ti]*100,Ws[wi],ss[si]*100,gs[gi],s.n,s.net,s.pf,s.wf1,s.wf2,
                        s.n2326,s.net>0?100.0*s.top1/s.net:0); }
                    if(s.net>0) f_net++;
                    if(!(s.n>=30 && s.net>0 && s.pf>=1.3 && s.wf1>0 && s.wf2>0)) continue;
                    f_basic++;
                    if(s.n2326<=0) continue;                       // 2022-regime artifact filter
                    f_2326++;
                    if(s.top1>0.45*s.net) continue;                // payoff-concentration cap
                    f_top1++;
                    int ok=0,tot=0; int dw[]={-1,1,0,0}, dt[]={0,0,-1,1};
                    for(int k=0;k<4;k++){ int w2=wi+dw[k],t2=ti+dt[k];
                        if(w2<0||w2>=NW||t2<0||t2>=NT) continue;
                        tot++; if(S[si][gi][w2][t2].net>0) ok++; }
                    if(!(tot>0 && ok*4>=tot*3)) continue;          // plateau >=75%
                    f_plat++;
                    Res r2=run_full(b,Ws[wi],thrs[ti],ss[si],gs[gi],40.0);
                    if(r2.net<=0) continue;                        // 2x-cost re-sim
                    char ln[256];
                    std::snprintf(ln,sizeof ln,
                        "  %.1f%% W=%-2d s=%.2f%% g=%.1f | n=%-4d %+8.0f PF=%.2f %+7.0f %+7.0f %+7.0f %+8.0f %4.0f%% %+8.0f %+6.0f %+7.0f",
                        thrs[ti]*100,Ws[wi],ss[si]*100,gs[gi],s.n,s.net,s.pf,s.wf1,s.wf2,
                        r2.net,s.n2326,100.0*s.top1/s.net,s.y22,s.worst,-s.maxdd);
                    std::printf("%s%s\n",ln,thrs[ti]>0.0251?"  [thr=3 boundary]":"");
                    if(thrs[ti]<=0.0251){ npass_low++;
                        if(r2.net>best2x){ best2x=r2.net; std::snprintf(bestln,sizeof bestln,"%s",ln); } }
                    else npass_bnd++;
                }
            if(npass_low) std::printf("%s BEST thr<=2.5:%s\n",CS[ci],bestln);
            else std::printf("%s: NO PASSING CELL at thr<=2.5%% (boundary thr=3.0 passes: %d)\n",CS[ci],npass_bnd);
            std::printf("  funnel: %d cells | net>0: %d | +basic(n,PF,WF): %d | +2023-26: %d | +top1<=45%%: %d | +plateau: %d\n",
                f_tot,f_net,f_basic,f_2326,f_top1,f_plat);
            std::printf("  best raw net cell: %s\n",bestnetln);
            std::fflush(stdout);
        }
        return 0;
    }
    std::fprintf(stderr,"mode? sweep|detail|percoin|trades|stopsweep|lowthr\n"); return 1;
}
