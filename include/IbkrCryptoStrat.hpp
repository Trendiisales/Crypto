// IbkrCryptoStrat.hpp -- IBKRCrypto validated signal + sizing core.
//
// The ONLY strategy logic ported from ChimeraCrypto's EdgeEngine that survived the
// faithful, walk-forward, extended-history (2017-2026, 4 bear regimes) backtest:
//   * IBS mean-reversion (LONG+SHORT)   -- ALL-WEATHER on BTC + SOL
//   * TSMom50 trend     (LONG+SHORT)    -- trend SATELLITE (ETH; crash-exposed)
// Both vol-targeted (2%/day). Params below are LOCKED from ibkrcrypto_bt.cpp.
//
// Pure price (daily OHLC) -> runs on CME SQF. No funding/L2/CVD/microstructure
// (those Chimera engines were culled -- IB SQF can't supply them). Standalone,
// header-only, matches the backtest bar-for-bar.
#ifndef IBKRCRYPTO_STRAT_H
#define IBKRCRYPTO_STRAT_H
#include <deque>
#include <cmath>
#include <algorithm>
#include <string>

namespace ibkrcrypto {

struct Bar { double o,h,l,c; };

// ---- locked params (from extended-history walk-forward BT, 2026-06-24) ----
struct StratParams {
    // IBS
    double ibs_lo = 0.15, ibs_hi = 0.85;
    // TSMom
    int    tsmom_lb = 50;
    // EMA-cross trend
    int    ema_fast = 20, ema_slow = 50;
    // RSI reversal
    int    rsi_n = 14; double rsi_lo = 30, rsi_hi = 70;
    // Keltner channel breakout
    int    kelt_n = 20; double kelt_m = 2.0;
    // Regime-switch (efficiency-ratio gate)
    int    reg_n = 20; double er_hi = 0.40, er_lo = 0.25;
    // vol-target sizing
    double vt_target = 0.02;   // 2%/day notional vol
    int    vt_lb     = 20;
    double vt_min = 0.10, vt_max = 1.50;
    bool   allow_short = true;  // PERP UNLOCK
};

enum class Mode { IBS, TSMOM, EMAX, RSIREV, KELT, REGIME };

// Rolling daily-bar strategy. Feed one completed daily bar; get target position
// (-1/0/+1) and a vol-target size multiplier. Mirrors run_bt() exactly.
class IbkrCryptoStrat {
public:
    IbkrCryptoStrat(Mode m, StratParams p=StratParams{}) : mode_(m), p_(p) {}

    void on_daily_bar(const Bar& b){
        bars_.push_back(b);
        if((int)bars_.size() > keep_) bars_.pop_front();
    }

    // target position at the close just pushed (entered next open, live).
    int target() const {
        const int n=(int)bars_.size();
        if(mode_==Mode::IBS){
            const Bar& b=bars_.back(); double rng=b.h-b.l; if(rng<=0) return 0;
            double v=(b.c-b.l)/rng;
            if(v<p_.ibs_lo) return 1;
            if(v>p_.ibs_hi) return p_.allow_short?-1:0;
            return 0;
        } else if(mode_==Mode::TSMOM){
            if(n<p_.tsmom_lb+1) return 0;
            double r=bars_[n-1].c - bars_[n-1-p_.tsmom_lb].c;
            if(r>0) return 1; if(r<0) return p_.allow_short?-1:0; return 0;
        } else if(mode_==Mode::EMAX){
            if(n<4*p_.ema_slow) return 0;
            auto ema=[&](int p)->double{ int st=n-1-4*p; if(st<0)st=0; double a=2.0/(p+1),e=bars_[st].c;
                for(int j=st+1;j<=n-1;++j)e=a*bars_[j].c+(1-a)*e; return e; };
            double ef=ema(p_.ema_fast),es=ema(p_.ema_slow);
            if(ef>es) return 1; if(ef<es) return p_.allow_short?-1:0; return 0;
        } else if(mode_==Mode::RSIREV){
            if(n<p_.rsi_n+1) return 0; double g=0,l=0;
            for(int j=n-p_.rsi_n;j<n;++j){ double d=bars_[j].c-bars_[j-1].c; if(d>0)g+=d; else l-=d; }
            g/=p_.rsi_n; l/=p_.rsi_n; double rs=l>0?g/l:999.0, rsi=100.0-100.0/(1.0+rs);
            if(rsi<p_.rsi_lo) return 1; if(rsi>p_.rsi_hi) return p_.allow_short?-1:0; return 0;
        } else if(mode_==Mode::KELT){
            int N=p_.kelt_n; if(n<N+1) return 0; double a=2.0/(N+1),e=bars_[n-1-N].c;
            for(int j=n-N;j<=n-1;++j)e=a*bars_[j].c+(1-a)*e;
            double atr=0; for(int j=n-N;j<=n-1;++j){double tr=std::max(bars_[j].h-bars_[j].l,std::max(std::fabs(bars_[j].h-bars_[j-1].c),std::fabs(bars_[j].l-bars_[j-1].c)));atr+=tr;} atr/=N;
            double c=bars_[n-1].c; if(atr<=0)return 0;
            if(c>e+p_.kelt_m*atr) return 1; if(c<e-p_.kelt_m*atr) return p_.allow_short?-1:0; return 0;
        } else { // REGIME (ER-gated trend/MR)
            int N=p_.reg_n; if(n<N+1) return 0; double net=std::fabs(bars_[n-1].c-bars_[n-1-N].c),vol=0;
            for(int j=n-N;j<=n-1;++j)vol+=std::fabs(bars_[j].c-bars_[j-1].c);
            double er=vol>0?net/vol:0;
            if(er>p_.er_hi){ int lb=(n-1>=50?50:n-1); double r=bars_[n-1].c-bars_[n-1-lb].c; return r>0?1:(r<0?(p_.allow_short?-1:0):0); }
            if(er<p_.er_lo){ const Bar&b=bars_[n-1]; double rng=b.h-b.l; if(rng<=0)return 0; double v=(b.c-b.l)/rng;
                if(v<p_.ibs_lo)return 1; if(v>p_.ibs_hi)return p_.allow_short?-1:0; }
            return 0;
        }
    }

    // vol-target size multiplier (apply to base notional).
    double size_mult() const {
        if(p_.vt_target<=0) return 1.0;
        const int n=(int)bars_.size(); if(n<p_.vt_lb+1) return p_.vt_min;
        double m=0; int k=0;
        for(int j=n-p_.vt_lb;j<n;++j){ double rr=(bars_[j].c-bars_[j-1].c)/bars_[j-1].c; m+=rr; ++k; }
        m/=k; double s2=0;
        for(int j=n-p_.vt_lb;j<n;++j){ double rr=(bars_[j].c-bars_[j-1].c)/bars_[j-1].c; s2+=(rr-m)*(rr-m); }
        double rv=std::sqrt(s2/k); if(rv<=0) return p_.vt_min;
        return std::max(p_.vt_min,std::min(p_.vt_max,p_.vt_target/rv));
    }
    bool warm() const { int need=p_.vt_lb+1;
        if(mode_==Mode::TSMOM) need=p_.tsmom_lb+1;
        else if(mode_==Mode::EMAX) need=4*p_.ema_slow;
        else if(mode_==Mode::RSIREV) need=p_.rsi_n+1;
        else if(mode_==Mode::KELT) need=p_.kelt_n+1;
        else if(mode_==Mode::REGIME) need=52;
        return (int)bars_.size() >= need; }

private:
    Mode mode_; StratParams p_;
    std::deque<Bar> bars_;
    int keep_ = 256;
};

// The deployable roster (validated edges only -- everything else culled).
struct EdgeSpec { const char* sym; Mode mode; const char* grade; };
inline const EdgeSpec ROSTER[] = {
    // === TREND ENSEMBLE (EMAx + Keltner per symbol; lightly-correlated, ride to the turn, NO profit-stop) ===
    {"ethusdt", Mode::EMAX,   "ETH trend EMAx    OOS 2.19"},
    {"ethusdt", Mode::KELT,   "ETH trend Keltner OOS 1.91 / bear 1.33"},
    {"btcusdt", Mode::EMAX,   "BTC trend EMAx    OOS 1.93"},
    {"btcusdt", Mode::KELT,   "BTC trend Keltner OOS 2.48 / bear 1.29"},
    {"solusdt", Mode::EMAX,   "SOL trend EMAx    OOS 1.84 / bear 1.86"},
    {"solusdt", Mode::KELT,   "SOL trend Keltner OOS 1.56 / bear 2.07"},
    {"ndx",     Mode::TSMOM,  "NDX trend TSMom   OOS 1.64 (index)"},
    // === ORTHOGONAL DIVERSIFIERS (regime-robust) ===
    {"btcusdt", Mode::REGIME, "BTC regime-switch OOS 1.60 / bear 1.06 (both-regime)"},
    {"ethusdt", Mode::REGIME, "ETH regime-switch OOS 1.20 / bear 1.28 (both-regime)"},
    {"solusdt", Mode::REGIME, "SOL regime-switch OOS 1.33 / bear 1.25 (both-regime)"},
    {"btcusdt", Mode::IBS,    "BTC IBS  mean-rev OOS 1.05 (orthogonal rho -0.1)"},
    {"solusdt", Mode::IBS,    "SOL IBS  mean-rev OOS 1.09 / bear 1.37"},
    {"ndx",     Mode::RSIREV, "NDX RSIrev        OOS 1.30 / bear 1.27 (index MR)"},
    // PROTECTION: NO per-trade profit-stop (kills trend edge, proven); protection = vol-target sizing
    // + exit-on-turn + orthogonal-leg diversification at the book level.
    // CULLED: BollMR, Pairs(BTC-ETH), Carry, XRP, ETH-IBS, SOL-TSMom, crypto-RSIrev, MACD/Ichi/ROC (trend-redundant).
};

} // namespace ibkrcrypto
#endif
