// ibkrcrypto_bt.cpp -- IBKRCrypto FAITHFUL backtest harness (fountain of truth).
//
// Design goals (operator spec):
//   * CRTP strategies  -> zero virtual dispatch, fully inlined signal()
//   * struct-of-arrays  -> cache-friendly single pass, no per-bar allocation
//   * FAITHFUL fills     -> decide at bar close, ENTER at NEXT bar open, cross the
//                           spread on entry AND exit, pay round-trip taker cost
//   * PERP UNLOCK        -> LONG *and* SHORT both modelled; daily financing (SQF
//                           TFA / perp funding) applied every day a position is
//                           held, with the correct sign (long pays carry in
//                           contango, short receives). This is the spot->perp diff.
//
// Models CME Spot-Quoted Futures economics on the SQF underlying spot series
// (SQF trade AT spot by construction; the daily TFA financing is the carry leg).
// Real Binance funding used where history exists; else a parameterised annual
// carry stands in for the published CME basis. Both shown.
//
// Build:  c++ -std=c++20 -O3 -march=native ibkrcrypto_bt.cpp -o ibkrcrypto_bt
// Run:    ./ibkrcrypto_bt <SYMBOL> <spot_1h_csv> [funding_csv]
#include <cstdio>
#include <cstdint>
#include <cstring>
#include <cmath>
#include <string>
#include <vector>
#include <fstream>
#include <algorithm>
#include <cstdlib>

// Bar timeframe (ms). Default = daily so the live --signal/--equity path used by
// refresh_shadow.py is byte-identical. Set BT_TF_MS=3600000 (1h) / 14400000 (4h)
// for the intraday stack. G_BARS_PER_DAY scales the per-bar financing accrual so
// annual_carry/365/day stays correct at any cadence.
static int64_t G_TF_MS       = 86400000LL;
static double  G_BARS_PER_DAY = 1.0;

// ----------------------------- data (SoA) -----------------------------------
struct Series {
    std::string sym;
    std::vector<int64_t> ts;     // day bucket (ms, UTC midnight)
    std::vector<double>  o,h,l,c;
    std::vector<double>  fund;   // daily financing rate (fraction/day), 0 if none
    int n() const { return (int)c.size(); }
};

// Load 1h OHLC CSV (open_time_ms,open,high,low,close,...) and resample -> DAILY.
// Daily is the right cadence: SQF TFA is a once-per-24h financing adjustment.
static Series load_daily(const std::string& sym, const char* path){
    Series s; s.sym = sym;
    std::ifstream f(path);
    if(!f.is_open()){ std::fprintf(stderr,"cannot open %s\n",path); return s; }
    std::string line; bool first=true;
    int64_t cur_day=-1; double dop=0,dh=0,dl=0,dc=0;
    while(std::getline(f,line)){
        if(line.empty()) continue;
        if(first){ first=false; if(line[0]<'0'||line[0]>'9') continue; }
        int64_t t; double o,h,l,c;
        if(std::sscanf(line.c_str(),"%lld,%lf,%lf,%lf,%lf",(long long*)&t,&o,&h,&l,&c)!=5) continue;
        if(t < 1000000000000LL) t *= 1000;   // autodetect sec->ms (index files use sec)
        int64_t day = (t/G_TF_MS)*G_TF_MS;    // bar bucket (daily default; BT_TF_MS for intraday)
        if(day!=cur_day){
            if(cur_day>=0){ s.ts.push_back(cur_day); s.o.push_back(dop); s.h.push_back(dh); s.l.push_back(dl); s.c.push_back(dc); }
            cur_day=day; dop=o; dh=h; dl=l; dc=c;
        } else { dh=std::max(dh,h); dl=std::min(dl,l); dc=c; }
    }
    if(cur_day>=0){ s.ts.push_back(cur_day); s.o.push_back(dop); s.h.push_back(dh); s.l.push_back(dl); s.c.push_back(dc); }
    s.fund.assign(s.c.size(), 0.0);
    return s;
}

// Overlay real funding (symbol,funding_time_ms,funding_rate,mark) -> sum per day
// (Binance pays 3x/day @8h; SQF is 1x/day -> we sum the day's funding = daily rate).
static void overlay_funding(Series& s, const char* path){
    if(!path) return;
    std::ifstream f(path); if(!f.is_open()) return;
    std::string line; bool first=true;
    std::vector<double> day_sum(s.ts.size(),0.0); std::vector<int> hit(s.ts.size(),0);
    auto idx_of=[&](int64_t day)->int{
        auto it=std::lower_bound(s.ts.begin(),s.ts.end(),day);
        if(it!=s.ts.end() && *it==day) return (int)(it-s.ts.begin());
        return -1;
    };
    while(std::getline(f,line)){
        if(line.empty()) continue;
        if(first){ first=false; if(line.find("funding")!=std::string::npos) continue; }
        char symbuf[32]; long long t; double rate,mark;
        if(std::sscanf(line.c_str(),"%31[^,],%lld,%lf,%lf",symbuf,&t,&rate,&mark)!=4) continue;
        int64_t day=((int64_t)t/86400000LL)*86400000LL;
        int i=idx_of(day); if(i>=0){ day_sum[i]+=rate; hit[i]=1; }
    }
    int covered=0;
    for(size_t i=0;i<s.ts.size();++i) if(hit[i]){ s.fund[i]=day_sum[i]; covered++; }
    std::fprintf(stderr,"[funding] %s: %d/%zu days covered\n",s.sym.c_str(),covered,s.ts.size());
}

// ----------------------------- cost model -----------------------------------
struct Cfg {
    double cost_bps      = 6.0;     // round-trip taker+commission, bps of notional
    double half_spread_bps = 1.0;   // crossed each side
    double annual_carry  = 0.10;    // fallback daily TFA = this/365 (long pays in contango)
    bool   use_real_fund = false;   // use overlaid funding instead of annual_carry
    bool   allow_short   = false;   // LONG-ONLY: spot venue, WE CANNOT TRADE PERP (operator 2026-07-04). Down-cross => flat, never short. Was true ("PERP UNLOCK").
    // vol-target sizing (0 = off): size = clamp(vt_target / realized_daily_vol, vt_min, vt_max)
    double vt_target     = 0.0;     // e.g. 0.02 = target ~2%/day notional vol
    int    vt_lb         = 20;      // realized-vol lookback (days)
    double vt_min        = 0.10, vt_max = 1.50;
    double slip_bps      = 0.0;      // adverse-fill slippage beyond spread (per round trip)
    double trail_gb      = 0.0;      // profit-protect: exit if running profit retraces this frac of peak (0=off)
    double be_arm        = 0.0;      // S-2026-06-25 BE-RATCHET: arm once peak >= be_arm (frac); 0=off
    double be_floor      = 0.0;      // once armed, exit when running profit falls to be_floor (0=breakeven)
    int    regime_ma     = 0;        // S-2026-06-25 REGIME GATE: only trade WITH the SMA(regime_ma)-trend (0=off)
};

// ----------------------------- result ---------------------------------------
struct Res {
    int n=0, wins=0; double net=0,gw=0,gl=0, peak=0,eq=0,mdd=0; long bars_in=0;
    void trade(double pnl){ ++n; if(pnl>0){++wins;gw+=pnl;} else gl+=std::fabs(pnl); }
    void mark(double e){ eq=e; if(eq>peak)peak=eq; if(peak-eq>mdd)mdd=peak-eq; }
    double pf()const{ return gl>0?gw/gl:(gw>0?999:0); }
    double wr()const{ return n?100.0*wins/n:0; }
};

// ---- core array backtest: target-position series -> faithful P&L ------------
// pos[i] = desired position at CLOSE of bar i (-1/0/+1). Entered at open[i+1].
template<class Strat>
static Res run_bt(const Series& s, const Cfg& cfg, const Strat& strat,
                  int64_t t0, int64_t t1){
    Res r; const int N=s.n();
    const double cost = (cfg.cost_bps + 2*cfg.half_spread_bps + cfg.slip_bps)*1e-4; // RT: fees+spread+slippage
    // realized daily vol at bar i (over prior vt_lb closes) for vol-target sizing
    auto realized_vol=[&](int i)->double{
        int lb=cfg.vt_lb; if(i<lb+1) return 0.0; double m=0,s2=0; int k=0;
        for(int j=i-lb+1;j<=i;++j){ double rr=(s.c[j]-s.c[j-1])/s.c[j-1]; m+=rr; ++k; }
        m/=k; for(int j=i-lb+1;j<=i;++j){ double rr=(s.c[j]-s.c[j-1])/s.c[j-1]; s2+=(rr-m)*(rr-m); }
        return std::sqrt(s2/k);
    };
    auto sizer=[&](int i)->double{
        if(cfg.vt_target<=0) return 1.0; double rv=realized_vol(i);
        if(rv<=0) return cfg.vt_min; double z=cfg.vt_target/rv;
        return std::max(cfg.vt_min,std::min(cfg.vt_max,z));
    };
    int curpos=0; double entry=0, carry=0, size=1.0, equity=0, peak=0;
    for(int i=1;i<N;++i){
        if(s.ts[i] < t0 || s.ts[i] > t1){ curpos=0; peak=0; continue; } // outside window: flat
        int want = strat.signal(s, i-1);             // decided @close i-1
        if(!cfg.allow_short && want<0) want=0;
        if(cfg.regime_ma>0 && want!=0 && i-1>=cfg.regime_ma){   // REGIME GATE: trade only WITH the SMA-trend
            double sma=0; for(int j=i-cfg.regime_ma;j<i;++j) sma+=s.c[j]; sma/=cfg.regime_ma;
            if(want>0 && s.c[i-1]<sma) want=0;       // no longs below the MA
            if(want<0 && s.c[i-1]>sma) want=0;       // no shorts above the MA
        }
        if(curpos!=0){                               // financing carried into day i
            double tfa = cfg.use_real_fund ? s.fund[i] : (cfg.annual_carry/(365.0*G_BARS_PER_DAY));
            carry += -curpos * tfa; r.bars_in++;     // long pays, short receives (contango)
            // PROFIT-PROTECT: trailing-giveback exit (mark at this close)
            if(cfg.trail_gb>0){
                double cur = curpos*(s.c[i]-entry)/entry; if(cur>peak) peak=cur;
                if(peak>0 && cur < peak*(1.0-cfg.trail_gb)){
                    double pnl=size*(carry+cur-cost); r.trade(pnl); equity+=pnl; r.mark(equity);
                    curpos=0; peak=0; continue;       // banked; re-eval signal next bar
                }
            }
            // S-2026-06-25 BE-RATCHET: once peak >= be_arm, a winner can't fall below be_floor
            // (cuts green->red round-trips WITHOUT the giveback-lock that kills the trend edge).
            if(cfg.be_arm>0){
                double cur = curpos*(s.c[i]-entry)/entry; if(cur>peak) peak=cur;
                if(peak>=cfg.be_arm && cur<=cfg.be_floor){
                    double pnl=size*(carry+cur-cost); r.trade(pnl); equity+=pnl; r.mark(equity);
                    curpos=0; peak=0; continue;
                }
            }
        }
        if(want!=curpos){
            if(curpos!=0){                           // close @open[i]
                double ret = curpos*(s.o[i]-entry)/entry;
                double pnl = size*(carry + ret - cost);
                r.trade(pnl); equity+=pnl; r.mark(equity);
            }
            if(want!=0){ entry=s.o[i]; carry=0; size=sizer(i-1); peak=0; }
            curpos=want;
        }
    }
    if(curpos!=0){                                    // flatten runner @last close
        double ret = curpos*(s.c[N-1]-entry)/entry;
        double pnl = size*(carry + ret - cost); r.trade(pnl); equity+=pnl; r.mark(equity);
    }
    return r;
}

// ----------------------------- CRTP strategies ------------------------------
template<class D> struct Strat { int signal(const Series& s,int i) const {
    return static_cast<const D*>(this)->sig(s,i); } };

// Time-series momentum: sign of L-day return. long+short.
struct TSMom : Strat<TSMom>{ int L; explicit TSMom(int l):L(l){}
    int sig(const Series& s,int i)const{ if(i<L)return 0;
        double r=s.c[i]-s.c[i-L]; return r>0?1:(r<0?-1:0);} };

// Donchian breakout trend. long+short.
struct Donch : Strat<Donch>{ int N; explicit Donch(int n):N(n){}
    int sig(const Series& s,int i)const{ if(i<N)return 0;
        double hh=s.h[i],ll=s.l[i]; for(int k=1;k<=N;++k){hh=std::max(hh,s.h[i-k]);ll=std::min(ll,s.l[i-k]);}
        if(s.c[i]>=hh)return 1; if(s.c[i]<=ll)return -1; return 0/*hold via persistence below*/;} };

// IBS mean reversion: (c-l)/(h-l). low->long, high->short. long+short.
struct IBS : Strat<IBS>{ double lo,hi; IBS(double a,double b):lo(a),hi(b){}
    int sig(const Series& s,int i)const{ double rng=s.h[i]-s.l[i]; if(rng<=0)return 0;
        double v=(s.c[i]-s.l[i])/rng; if(v<lo)return 1; if(v>hi)return -1; return 0;} };

// Funding/TFA carry: when daily financing is richly positive, SHORT to collect it
// (and vice versa). The perp-native edge. Needs real funding overlay.
struct Carry : Strat<Carry>{ double thr; explicit Carry(double t):thr(t){}
    int sig(const Series& s,int i)const{ double fr=s.fund[i];
        if(fr> thr) return -1;   // longs overpay -> short collects funding
        if(fr<-thr) return  1;   // shorts overpay -> long collects
        return 0;} };

// Bollinger mean-reversion: close vs SMA(N) +/- K*sd. long+short. (Chimera BOLL family)
struct BollMR : Strat<BollMR>{ int N; double K; BollMR(int n,double k):N(n),K(k){}
    int sig(const Series& s,int i)const{ if(i<N)return 0; double m=0;
        for(int j=i-N+1;j<=i;++j)m+=s.c[j]; m/=N; double v=0;
        for(int j=i-N+1;j<=i;++j)v+=(s.c[j]-m)*(s.c[j]-m); double sd=std::sqrt(v/N);
        if(sd<=0)return 0; if(s.c[i]<m-K*sd)return 1; if(s.c[i]>m+K*sd)return -1; return 0;} };
// RSI reversal: Wilder-ish RSI(N). <lo long, >hi short. long+short. (Chimera RSIR family)
struct RSIrev : Strat<RSIrev>{ int N; double lo,hi; RSIrev(int n,double l,double h):N(n),lo(l),hi(h){}
    int sig(const Series& s,int i)const{ if(i<N+1)return 0; double g=0,l=0;
        for(int j=i-N+1;j<=i;++j){double d=s.c[j]-s.c[j-1]; if(d>0)g+=d; else l-=d;}
        g/=N; l/=N; double rs=l>0?g/l:999.0, rsi=100.0-100.0/(1.0+rs);
        if(rsi<lo)return 1; if(rsi>hi)return -1; return 0;} };
// EMA-cross trend: EMA(F) vs EMA(S). long+short. (Chimera EMA/ICHI-trend proxy)
struct EMAx : Strat<EMAx>{ int F,S; EMAx(int f,int s):F(f),S(s){}
    double ema(const Series& se,int i,int p)const{ int st=i-4*p; if(st<0)st=0;
        double a=2.0/(p+1), e=se.c[st]; for(int j=st+1;j<=i;++j)e=a*se.c[j]+(1-a)*e; return e;}
    int sig(const Series& se,int i)const{ if(i<4*S)return 0; double ef=ema(se,i,F),es=ema(se,i,S);
        if(ef>es)return 1; if(ef<es)return -1; return 0;} };

// Ichimoku tenkan/kijun trend (Chimera ICHI family — their top Sharpe). long+short.
struct Ichi : Strat<Ichi>{ int T,K; Ichi(int t,int k):T(t),K(k){}
    int sig(const Series& s,int i)const{ if(i<K)return 0;
        auto mid=[&](int n){double hh=s.h[i],ll=s.l[i];for(int j=1;j<n;++j){hh=std::max(hh,s.h[i-j]);ll=std::min(ll,s.l[i-j]);}return (hh+ll)/2;};
        double ten=mid(T),kij=mid(K);
        if(ten>kij&&s.c[i]>kij)return 1; if(ten<kij&&s.c[i]<kij)return -1; return 0;} };
// Keltner channel breakout: EMA(N) +/- M*ATR(N). long+short.
struct Kelt : Strat<Kelt>{ int N; double M; Kelt(int n,double m):N(n),M(m){}
    int sig(const Series& s,int i)const{ if(i<N+1)return 0;
        double a=2.0/(N+1),e=s.c[i-N]; for(int j=i-N+1;j<=i;++j)e=a*s.c[j]+(1-a)*e;
        double atr=0; for(int j=i-N+1;j<=i;++j){double tr=std::max(s.h[j]-s.l[j],std::max(std::fabs(s.h[j]-s.c[j-1]),std::fabs(s.l[j]-s.c[j-1])));atr+=tr;} atr/=N;
        if(atr<=0)return 0; if(s.c[i]>e+M*atr)return 1; if(s.c[i]<e-M*atr)return -1; return 0;} };
// MACD trend (F/S EMAs, momentum of the diff). long+short.
struct Macd : Strat<Macd>{ int F,S; Macd(int f,int s):F(f),S(s){}
    double ema(const Series& se,int i,int p)const{int st=i-4*p;if(st<0)st=0;double a=2.0/(p+1),e=se.c[st];for(int j=st+1;j<=i;++j)e=a*se.c[j]+(1-a)*e;return e;}
    int sig(const Series& se,int i)const{ if(i<4*S+1)return 0; double m=ema(se,i,F)-ema(se,i,S), mp=ema(se,i-1,F)-ema(se,i-1,S);
        if(m>0&&m>=mp)return 1; if(m<0&&m<=mp)return -1; return 0;} };
// ROC momentum: N-day % change vs threshold. long+short.
struct Roc : Strat<Roc>{ int N; double thr; Roc(int n,double t):N(n),thr(t){}
    int sig(const Series& s,int i)const{ if(i<N)return 0; double r=(s.c[i]-s.c[i-N])/s.c[i-N];
        if(r>thr)return 1; if(r<-thr)return -1; return 0;} };

// Wide up-jump momentum (crypto): LONG on a W-bar up-jump >= thr, ride until the
// symmetric W-bar DOWN-jump (<= -thr). Long-only. Stateless persistence: the most
// recent bar whose |W-bar jump| >= thr decides direction (mirrors the parent
// ride-to-symmetric-flip; DonchHold-style stateless hold). W=24 (24x1h) per parent.
struct UpJump : Strat<UpJump>{ double thr; int W; UpJump(double t,int w=24):thr(t),W(w){}
    int sig(const Series& s,int i)const{ if(i<W)return 0;
        for(int k=i;k>=W;--k){ double j=s.c[k]/s.c[k-W]-1.0;
            if(j>= thr) return 1;      // most recent event = up-jump -> long
            if(j<=-thr) return 0; }    // most recent event = down-jump -> flat
        return 0;} };

// Regime-switch: efficiency-ratio gate. trending -> TSMom; chop -> IBS; mid -> flat.
struct Regime : Strat<Regime>{ int N; double erhi,erlo; Regime(int n,double a,double b):N(n),erhi(a),erlo(b){}
    int sig(const Series& s,int i)const{ if(i<N+1)return 0;
        double net=std::fabs(s.c[i]-s.c[i-N]),vol=0; for(int j=i-N+1;j<=i;++j)vol+=std::fabs(s.c[j]-s.c[j-1]);
        double er=vol>0?net/vol:0;
        if(er>erhi){ double r=s.c[i]-s.c[i-(i>=50?50:i)]; return r>0?1:(r<0?-1:0); }   // trending -> momentum
        if(er<erlo){ double rng=s.h[i]-s.l[i]; if(rng<=0)return 0; double v=(s.c[i]-s.l[i])/rng;
                     if(v<0.15)return 1; if(v>0.85)return -1; }                          // chop -> mean-rev
        return 0; } };

// Donchian needs position persistence (hold until opposite break). Wrap it.
struct DonchHold : Strat<DonchHold>{ int N; explicit DonchHold(int n):N(n){}
    int sig(const Series& s,int i)const{ if(i<N)return 0;
        double hh=s.h[i],ll=s.l[i]; for(int k=1;k<=N;++k){hh=std::max(hh,s.h[i-k]);ll=std::min(ll,s.l[i-k]);}
        // re-derive last breakout by scanning back a little (stateless persistence)
        if(s.c[i]>=hh) return 1; if(s.c[i]<=ll) return -1;
        // hold: look back for most recent break
        for(int k=i-1;k>=N && k>i-N;--k){ double h2=s.h[k],l2=s.l[k];
            for(int j=1;j<=N;++j){h2=std::max(h2,s.h[k-j]);l2=std::min(l2,s.l[k-j]);}
            if(s.c[k]>=h2)return 1; if(s.c[k]<=l2)return -1; }
        return 0;} };

// ----------------------------- regimes --------------------------------------
struct Win{ const char* name; int64_t t0,t1; };
static const Win WINS[] = {
    {"2018_bear", 1514764800000LL, 1546214400000LL},
    {"2020_covid",1577836800000LL, 1609372800000LL},
    {"2021_bull", 1609459200000LL, 1640908800000LL},
    {"2022_bear", 1640995200000LL, 1672444800000LL},
    {"2023",      1672531200000LL, 1703980800000LL},
    {"2024",      1704067200000LL, 1735603200000LL},
    {"2025",      1735689600000LL, 1767139200000LL},
    {"2026ytd",   1767225600000LL, 1799999999000LL},
    {"LAST_6M",   1766534400000LL, 1799999999000LL},   // recent huge-swing stress (Dec'25->now)
    {"IS_17-22",  1483228800000LL, 1672444800000LL},   // walk-forward in-sample (extended)
    {"OOS_23-26", 1672531200000LL, 1799999999000LL},   // walk-forward out-of-sample
    {"FULL",      1483228800000LL, 1799999999000LL},
};

// daily mark-to-market equity dump (for portfolio blending)
template<class Strat>
static void dump_equity(const Series& s, const Cfg& cfg, const Strat& strat,
                        int64_t t0, int64_t t1){
    const int N=s.n(); const double cost=(cfg.cost_bps+2*cfg.half_spread_bps+cfg.slip_bps)*1e-4;
    int curpos=0; double entry=0,carry=0,size=1.0,realized=0;
    auto rv=[&](int i){ int lb=cfg.vt_lb; if(i<lb+1)return 0.0; double m=0,s2=0;int k=0;
        for(int j=i-lb+1;j<=i;++j){double r=(s.c[j]-s.c[j-1])/s.c[j-1];m+=r;++k;} m/=k;
        for(int j=i-lb+1;j<=i;++j){double r=(s.c[j]-s.c[j-1])/s.c[j-1];s2+=(r-m)*(r-m);} return std::sqrt(s2/k);};
    auto sz=[&](int i){ if(cfg.vt_target<=0)return 1.0; double v=rv(i); if(v<=0)return cfg.vt_min;
        return std::max(cfg.vt_min,std::min(cfg.vt_max,cfg.vt_target/v)); };
    for(int i=1;i<N;++i){
        if(s.ts[i]<t0||s.ts[i]>t1){ curpos=0; continue; }
        int want=strat.signal(s,i-1); if(!cfg.allow_short&&want<0)want=0;
        if(cfg.regime_ma>0 && want!=0 && i-1>=cfg.regime_ma){   // REGIME GATE: match run_bt + live --signal
            double sma=0; for(int j=i-cfg.regime_ma;j<i;++j) sma+=s.c[j]; sma/=cfg.regime_ma;
            if(want>0 && s.c[i-1]<sma) want=0;
            if(want<0 && s.c[i-1]>sma) want=0;
        }
        if(curpos!=0){ double tfa=cfg.use_real_fund?s.fund[i]:(cfg.annual_carry/(365.0*G_BARS_PER_DAY)); carry+=-curpos*tfa; }
        if(want!=curpos){ if(curpos!=0){ double ret=curpos*(s.o[i]-entry)/entry; realized+=size*(carry+ret-cost);}
            if(want!=0){entry=s.o[i];carry=0;size=sz(i-1);} curpos=want; }
        double unreal = curpos!=0 ? size*(carry + curpos*(s.c[i]-entry)/entry) : 0.0;
        std::printf("%lld,%.6f\n",(long long)s.ts[i], realized+unreal);
    }
}

// per-bar position + MTM cumret dump (for whole-month book reconstruction at a $ pool).
// prints `ts,pos,cumret` each bar in window. pos=-1/0/+1 (current exposure), cumret=realized+unreal
// in size-scaled return units (same basis as dump_equity). Honors regime_ma like run_bt.
template<class Strat>
static void dump_postrace(const Series& s, const Cfg& cfg, const Strat& strat,
                          int64_t t0, int64_t t1){
    const int N=s.n(); const double cost=(cfg.cost_bps+2*cfg.half_spread_bps+cfg.slip_bps)*1e-4;
    int curpos=0; double entry=0,carry=0,size=1.0,realized=0;
    auto rv=[&](int i){ int lb=cfg.vt_lb; if(i<lb+1)return 0.0; double m=0,s2=0;int k=0;
        for(int j=i-lb+1;j<=i;++j){double r=(s.c[j]-s.c[j-1])/s.c[j-1];m+=r;++k;} m/=k;
        for(int j=i-lb+1;j<=i;++j){double r=(s.c[j]-s.c[j-1])/s.c[j-1];s2+=(r-m)*(r-m);} return std::sqrt(s2/k);};
    auto sz=[&](int i){ if(cfg.vt_target<=0)return 1.0; double v=rv(i); if(v<=0)return cfg.vt_min;
        return std::max(cfg.vt_min,std::min(cfg.vt_max,cfg.vt_target/v)); };
    for(int i=1;i<N;++i){
        if(s.ts[i]<t0||s.ts[i]>t1){ curpos=0; continue; }
        int want=strat.signal(s,i-1); if(!cfg.allow_short&&want<0)want=0;
        if(cfg.regime_ma>0 && want!=0 && i-1>=cfg.regime_ma){
            double sma=0; for(int j=i-cfg.regime_ma;j<i;++j) sma+=s.c[j]; sma/=cfg.regime_ma;
            if(want>0 && s.c[i-1]<sma) want=0;
            if(want<0 && s.c[i-1]>sma) want=0;
        }
        if(curpos!=0){ double tfa=cfg.use_real_fund?s.fund[i]:(cfg.annual_carry/(365.0*G_BARS_PER_DAY)); carry+=-curpos*tfa; }
        if(want!=curpos){ if(curpos!=0){ double ret=curpos*(s.o[i]-entry)/entry; realized+=size*(carry+ret-cost);}
            if(want!=0){entry=s.o[i];carry=0;size=sz(i-1);} curpos=want; }
        double unreal = curpos!=0 ? size*(carry + curpos*(s.c[i]-entry)/entry) : 0.0;
        std::printf("%lld,%d,%.6f\n",(long long)s.ts[i], curpos, realized+unreal);
    }
}

// MIN-LOT dollar P&L: fixed 1 contract per trade (no vol-target), real $ accounting.
//   $/trade = dir*(exit-entry)*mult + carry$ - fee_usd ; carry$ = -dir*tfa*entry*mult per day
template<class Strat>
static void run_dollars(const char* lbl, const Series& s, const Cfg& cfg, const Strat& strat,
                        double mult, double fee_usd, int64_t t0, int64_t t1){
    const int N=s.n(); int curpos=0; double entry=0,carry=0,eq=0,pk=0,mdd=0,net=0; int n=0,wins=0;
    for(int i=1;i<N;++i){
        if(s.ts[i]<t0||s.ts[i]>t1){curpos=0;continue;}
        int want=strat.signal(s,i-1); if(!cfg.allow_short&&want<0)want=0;
        if(curpos!=0){ double tfa=cfg.use_real_fund?s.fund[i]:(cfg.annual_carry/(365.0*G_BARS_PER_DAY));
                       carry += -curpos*tfa*entry*mult; }
        if(want!=curpos){
            if(curpos!=0){ double pnl=curpos*(s.o[i]-entry)*mult+carry-fee_usd;
                net+=pnl; eq+=pnl; if(eq>pk)pk=eq; if(pk-eq>mdd)mdd=pk-eq; n++; if(pnl>0)wins++; }
            if(want!=0){entry=s.o[i];carry=0;} curpos=want;
        }
    }
    if(curpos!=0){ double pnl=curpos*(s.c[N-1]-entry)*mult+carry-fee_usd;
        net+=pnl;eq+=pnl;if(eq>pk)pk=eq;if(pk-eq>mdd)mdd=pk-eq;n++;if(pnl>0)wins++; }
    std::printf("  %-16s 1-contract (mult=%.2f): trades=%d win=%4.0f%% net=$%+8.2f maxDD=$%7.2f notional~$%.0f\n",
        lbl,mult,n,n?100.0*wins/n:0,net,mdd, s.c.back()*mult);
}

template<class S>
static void row(const char* strat, const Series& s, const Cfg& cfg, const S& st){
    for(const auto& w: WINS){
        Res r = run_bt(s,cfg,st,w.t0,w.t1);
        if(r.n<3) continue;
        std::printf("%-6s %-14s %-9s n=%-4d WR=%4.1f%% PF=%5.2f net=%+8.2f%% maxDD=%6.2f%%\n",
            s.sym.c_str(), strat, w.name, r.n, r.wr(), r.pf(), r.net=100*r.eq, 100*r.mdd);
    }
}

int main(int argc,char**argv){
    if(argc<3){ std::fprintf(stderr,"usage: %s SYM spot_1h.csv [funding.csv] [--carry-real]\n",argv[0]); return 1; }
    std::string sym=argv[1];
    if(const char* tf=getenv("BT_TF_MS")){ G_TF_MS=atoll(tf); if(G_TF_MS<=0)G_TF_MS=86400000LL; }
    G_BARS_PER_DAY = 86400000.0 / (double)G_TF_MS;
    Series s=load_daily(sym,argv[2]);
    if(s.n()<200){ std::fprintf(stderr,"not enough bars (%d)\n",s.n()); return 1; }
    const char* fund = (argc>3 && argv[3][0]!='-') ? argv[3] : nullptr;
    if(const char* fc=getenv("FUNDCSV")) if(fc[0]) fund=fc;   // funding overlay via env (postrace can't pass argv[3])
    overlay_funding(s,fund);
    std::printf("[IBKRCRYPTO-BT] %s  %d bars (tf=%lldms, %.2f/day)  px %.2f -> %.2f  (spot, LONG-ONLY -- no perp)\n",
        sym.c_str(), s.n(), (long long)G_TF_MS, G_BARS_PER_DAY, s.c.front(), s.c.back());

    Cfg cfg;  // baseline: 6bps RT + 1bp half-spread x2, 10%/yr carry, LONG-ONLY (shorts OFF; spot venue, no perp)
    if(const char* cb=getenv("COSTBPS")) cfg.cost_bps=atof(cb);
    if(const char* hs=getenv("HSPREAD")) cfg.half_spread_bps=atof(hs);
    if(const char* sb=getenv("SLIPBPS")) cfg.slip_bps=atof(sb);
    if(const char* tg=getenv("TRAILGB")) cfg.trail_gb=atof(tg);
    if(const char* ba=getenv("BE_ARM"))   cfg.be_arm=atof(ba);
    if(const char* bf=getenv("BE_FLOOR")) cfg.be_floor=atof(bf);
    if(const char* rg=getenv("REGIME_MA")) cfg.regime_ma=atoi(rg);
    if(const char* ac=getenv("ANNUAL_CARRY")) cfg.annual_carry=atof(ac);     // flat TFA fallback (NDX/equity legs)
    if(const char* uf=getenv("USE_REAL_FUND")) cfg.use_real_fund=(atoi(uf)!=0); // 1 = use overlaid funding (real SQF/perp basis)
    // --dollars MULT FEE_USD STRAT : min-lot (1 contract) dollar P&L, LAST_6M + FULL
    for(int i=1;i<argc;++i) if(std::string(argv[i])=="--dollars"){
        double mult=(i+1<argc)?atof(argv[i+1]):0.01;
        double fee =(i+2<argc)?atof(argv[i+2]):0.94;
        std::string st=(i+3<argc)?argv[i+3]:"TSMom50";
        Cfg d=cfg; d.vt_target=0.0;  // min lot = fixed 1 contract, NO vol-target
        int64_t S6=1766534400000LL, T1=1799999999000LL, F0=1483228800000LL;
        std::printf("[%s] %s  mult=%.2f fee=$%.2f/RT\n", s.sym.c_str(), st.c_str(), mult, fee);
        auto go=[&](const char* w,int64_t a,int64_t b){
            if(st=="TSMom50") run_dollars(w,s,d,TSMom(50),mult,fee,a,b);
            else if(st=="IBS") run_dollars(w,s,d,IBS(0.15,0.85),mult,fee,a,b);
            else if(st=="Donch40") run_dollars(w,s,d,DonchHold(40),mult,fee,a,b);
            else if(st.rfind("UpJump",0)==0){ double thr=atof(st.c_str()+6)/100.0; if(thr<=0)thr=0.05; run_dollars(w,s,d,UpJump(thr),mult,fee,a,b); }
        };
        go("LAST_6M",S6,T1); go("FULL_17-26",F0,T1);
        return 0;
    }
    // --signal STRAT : print current target position + vol-target size at the latest bar (for live/shadow book)
    for(int i=1;i<argc;++i) if(std::string(argv[i])=="--signal"){
        std::string st=(i+1<argc)?argv[i+1]:"TSMom50";
        const int N=s.n(); Cfg e=cfg; e.vt_target=0.02;
        auto vtsz=[&](int bi)->double{ int lb=e.vt_lb; if(bi<lb+1)return e.vt_min; double m=0,s2=0;
            for(int j=bi-lb+1;j<=bi;++j)m+=(s.c[j]-s.c[j-1])/s.c[j-1]; m/=lb;
            for(int j=bi-lb+1;j<=bi;++j){double r=(s.c[j]-s.c[j-1])/s.c[j-1];s2+=(r-m)*(r-m);}
            double rv=std::sqrt(s2/lb); if(rv<=0)return e.vt_min; double z=e.vt_target/rv;
            return std::max(e.vt_min,std::min(e.vt_max,z)); };
        // exit/invalidation level: the price at which TODAY's signal would stop being the
        // current direction (the engine flips/exits there). Found by bisecting the latest
        // close: a SHORT exits as price RISES, a LONG exits as price FALLS. Honest "where do
        // we get out" number for engines that exit on signal-turn (no fixed profit target).
        auto sigAt=[&](auto& strat,double px)->int{
            Series t2=s; int M=t2.n()-1; t2.c[M]=px;
            t2.h[M]=std::max(s.h[M],px); t2.l[M]=std::min(s.l[M],px);
            return strat.signal(t2,M); };
        auto flip=[&](auto strat,int cur)->double{
            if(cur==0) return 0.0;
            double base=s.c[N-1], lo,hi;
            if(cur<0){ lo=base; hi=base*2.0; } else { lo=base*0.02; hi=base; }
            double far=(cur<0)?hi:lo; if(sigAt(strat,far)==cur) return 0.0; // no flip in range
            for(int it=0;it<48;++it){ double mid=0.5*(lo+hi); int sg=sigAt(strat,mid);
                if(sg==cur){ if(cur<0)lo=mid; else hi=mid; } else { if(cur<0)hi=mid; else lo=mid; } }
            return 0.5*(lo+hi); };
        int t=0; double ex=0.0;
        if(st=="TSMom50"){ TSMom S(50); t=S.signal(s,N-1); ex=flip(S,t); }
        else if(st=="IBS"){ IBS S(0.15,0.85); t=S.signal(s,N-1); ex=flip(S,t); }
        else if(st=="EMAx"){ EMAx S(20,50); t=S.signal(s,N-1); ex=flip(S,t); }
        else if(st=="RSIrev"){ RSIrev S(14,30,70); t=S.signal(s,N-1); ex=flip(S,t); }
        else if(st=="Kelt"){ Kelt S(20,2.0); t=S.signal(s,N-1); ex=flip(S,t); }
        else if(st=="Regime"){ Regime S(20,0.40,0.25); t=S.signal(s,N-1); ex=flip(S,t); }
        else if(st=="Donch40"){ DonchHold S(40); t=S.signal(s,N-1); ex=flip(S,t); }
        else if(st=="Ichi"){ Ichi S(9,26); t=S.signal(s,N-1); ex=flip(S,t); }
        else if(st=="Macd"){ Macd S(12,26); t=S.signal(s,N-1); ex=flip(S,t); }
        else if(st=="Roc"){ Roc S(20,0.0); t=S.signal(s,N-1); ex=flip(S,t); }
        else if(st=="TSMom20"){ TSMom S(20); t=S.signal(s,N-1); ex=flip(S,t); }
        else if(st=="Donch20"){ DonchHold S(20); t=S.signal(s,N-1); ex=flip(S,t); }
        else if(st.rfind("UpJump",0)==0){ double thr=atof(st.c_str()+6)/100.0; if(thr<=0)thr=0.05; UpJump S(thr); t=S.signal(s,N-1); ex=flip(S,t); }
        // REGIME GATE (S-2026-06-30): honour REGIME_MA in the live --signal path. run_bt
        // already gates; --signal previously did NOT -> the live shadow legs ran ungated.
        // Per-symbol close>SMA(regime_ma): long only above the MA, short only below. 0=off.
        if(e.regime_ma>0 && t!=0 && N>e.regime_ma){
            double sma=0; for(int j=N-e.regime_ma;j<N;++j) sma+=s.c[j]; sma/=e.regime_ma;
            if(t>0 && s.c[N-1]<sma) t=0;
            else if(t<0 && s.c[N-1]>sma) t=0;
            if(t==0) ex=0.0;
        }
        // LONG-ONLY shadow book (operator hard rule 2026-07-04): the live execution
        // venue is spot-only -- WE CANNOT TRADE PERP. Never emit a short target on the
        // live --signal path, regardless of the strat's allow_short/"PERP UNLOCK" default.
        // A down-cross => flat (0), not short. Applies to every shadow leg (daily+intraday).
        if(t<0){ t=0; ex=0.0; }
        std::printf("%s %s target=%d size=%.2f px=%.2f exit=%.2f\n",
            s.sym.c_str(), st.c_str(), t, vtsz(N-1), s.c[N-1], ex);
        return 0;
    }
    // --equity STRAT : dump daily MTM equity curve (for portfolio blending), then exit
    for(int i=1;i<argc;++i) if(std::string(argv[i])=="--equity"){
        std::string st=(i+1<argc)?argv[i+1]:"TSMom50";
        Cfg e=cfg; e.vt_target=0.02;
        int64_t T0=1483228800000LL,T1=1799999999000LL;
        if(st=="TSMom50") dump_equity(s,e,TSMom(50),T0,T1);
        else if(st=="IBS") dump_equity(s,e,IBS(0.15,0.85),T0,T1);
        else if(st=="Donch40") dump_equity(s,e,DonchHold(40),T0,T1);
        else if(st=="TSMom20") dump_equity(s,e,TSMom(20),T0,T1);
        else if(st=="Donch20") dump_equity(s,e,DonchHold(20),T0,T1);
        else if(st=="EMAx") dump_equity(s,e,EMAx(20,50),T0,T1);
        else if(st=="Kelt") dump_equity(s,e,Kelt(20,2.0),T0,T1);
        else if(st=="Macd") dump_equity(s,e,Macd(12,26),T0,T1);
        else if(st=="Ichi") dump_equity(s,e,Ichi(9,26),T0,T1);
        else if(st=="Roc") dump_equity(s,e,Roc(20,0.0),T0,T1);
        else if(st=="RSIrev") dump_equity(s,e,RSIrev(14,30,70),T0,T1);
        else if(st=="Regime") dump_equity(s,e,Regime(20,0.40,0.25),T0,T1);
        else if(st.rfind("UpJump",0)==0){ double thr=atof(st.c_str()+6)/100.0; if(thr<=0)thr=0.05; dump_equity(s,e,UpJump(thr),T0,T1); }
        return 0;
    }
    // --postrace STRAT [FROM_MS TO_MS] : per-bar `ts,pos,cumret` (whole-month book reconstruction)
    for(int i=1;i<argc;++i) if(std::string(argv[i])=="--postrace"){
        std::string st=(i+1<argc)?argv[i+1]:"TSMom50";
        int64_t T0=(i+2<argc && argv[i+2][0]!='-')?atoll(argv[i+2]):1483228800000LL;
        int64_t T1=(i+3<argc && argv[i+3][0]!='-')?atoll(argv[i+3]):1799999999000LL;
        Cfg e=cfg; e.vt_target=0.02;
        if(st=="TSMom50") dump_postrace(s,e,TSMom(50),T0,T1);
        else if(st=="IBS") dump_postrace(s,e,IBS(0.15,0.85),T0,T1);
        else if(st=="Donch40") dump_postrace(s,e,DonchHold(40),T0,T1);
        else if(st=="TSMom20") dump_postrace(s,e,TSMom(20),T0,T1);
        else if(st=="Donch20") dump_postrace(s,e,DonchHold(20),T0,T1);
        else if(st=="EMAx") dump_postrace(s,e,EMAx(20,50),T0,T1);
        else if(st=="Kelt") dump_postrace(s,e,Kelt(20,2.0),T0,T1);
        else if(st=="Macd") dump_postrace(s,e,Macd(12,26),T0,T1);
        else if(st=="Ichi") dump_postrace(s,e,Ichi(9,26),T0,T1);
        else if(st=="Roc") dump_postrace(s,e,Roc(20,0.0),T0,T1);
        else if(st=="RSIrev") dump_postrace(s,e,RSIrev(14,30,70),T0,T1);
        else if(st=="Regime") dump_postrace(s,e,Regime(20,0.40,0.25),T0,T1);
        else if(st.rfind("UpJump",0)==0){ double thr=atof(st.c_str()+6)/100.0; if(thr<=0)thr=0.05; dump_postrace(s,e,UpJump(thr),T0,T1); }
        return 0;
    }
    std::printf("--- price strategies (carry=%.0f%%/yr modelled as daily TFA, LONG-ONLY / shorts OFF) ---\n",100*cfg.annual_carry);
    row("TSMom20", s,cfg, TSMom(20));
    row("TSMom50", s,cfg, TSMom(50));
    row("Donch20", s,cfg, DonchHold(20));
    row("Donch40", s,cfg, DonchHold(40));
    row("IBS",     s,cfg, IBS(0.15,0.85));
    row("BollMR",  s,cfg, BollMR(20,2.0));
    row("RSIrev",  s,cfg, RSIrev(14,30,70));
    row("EMAx",    s,cfg, EMAx(20,50));

    // long-only comparison (the OLD spot-only regime) for TSMom20
    Cfg lo=cfg; lo.allow_short=false;
    std::printf("--- long-only (old spot-only) baseline for contrast ---\n");
    row("TSMom20-LO", s,lo, TSMom(20));
    row("Donch20-LO", s,lo, DonchHold(20));

    // VOL-TARGET hardening pass: 2%/day notional vol target -> drawdown control
    Cfg vt=cfg; vt.vt_target=0.02;
    std::printf("--- VOL-TARGET 2%%/day sizing (hardening: cuts maxDD) ---\n");
    row("TSMom50.vt", s,vt, TSMom(50));
    row("Donch40.vt", s,vt, DonchHold(40));
    row("IBS.vt",     s,vt, IBS(0.15,0.85));
    row("BollMR.vt",  s,vt, BollMR(20,2.0));
    row("RSIrev.vt",  s,vt, RSIrev(14,30,70));
    row("EMAx.vt",    s,vt, EMAx(20,50));
    row("Ichi.vt",    s,vt, Ichi(9,26));
    row("Kelt.vt",    s,vt, Kelt(20,2.0));
    row("Macd.vt",    s,vt, Macd(12,26));
    row("Roc20.vt",   s,vt, Roc(20,0.0));
    row("Regime.vt",  s,vt, Regime(20,0.40,0.25));
    row("UpJump5.vt", s,vt, UpJump(0.05));
    row("UpJump8.vt", s,vt, UpJump(0.08));
    row("UpJump12.vt",s,vt, UpJump(0.12));

    // funding carry (perp-native) -- only meaningful where real funding exists
    if(fund){
        Cfg fc=cfg; fc.use_real_fund=true;
        std::printf("--- FUNDING CARRY (perp-native, real funding overlay) ---\n");
        row("Carry.03", s,fc, Carry(0.0003));   // 3bp/day threshold
        row("Carry.05", s,fc, Carry(0.0005));
    }
    return 0;
}
