// CrossSectionalAllocator.hpp -- C++ port of the validated Chimera CSM allocator.
//
// Faithful transcription of backtest/xsec_allocator.py (which itself reproduces the
// vault entity [[CrossSectionalMomentum]] OOS figures EXACTLY). This is the LIVE spot
// sleeve: the per-symbol EdgeEngine never had a portfolio path, so nothing emitted
// spot targets. This allocator ranks the daily-close universe each rebalance, goes
// LONG the top-K positive-momentum coins, inverse-vol weights them.
//
// BTC>200d-SMA MACRO GATE REMOVED 2026-07-06 (operator hard rule, feedback-no-200dma-crypto:
// NO 200DMA anywhere in crypto). macro_gate=false / regime_ma=0 -> is_bull() is always true;
// the allocator no longer sits in cash below the 200d SMA. Do NOT re-add the BTC 200DMA gate.
//
// Operator directive "live crypto = C++ producers": the reference lived in Python; this is the
// live producer. Long-only retained; the [[BearSpotNoEdge]] 200DMA gate is gone.
//
// Validated config (the plateau centre): lb=30, K=3, rebal=14, invvol (macro_gate now OFF).
#ifndef IBKRCRYPTO_CROSS_SECTIONAL_ALLOCATOR_H
#define IBKRCRYPTO_CROSS_SECTIONAL_ALLOCATOR_H

#include <string>
#include <vector>
#include <unordered_map>
#include <map>
#include <algorithm>
#include <cmath>
#include <fstream>
#include <sstream>

namespace ibkrcrypto {

// A coin's aligned daily close series (index = union-day index; NaN = no bar that day).
struct DailySeries { std::string internal; std::vector<double> close; };

class CrossSectionalAllocator {
public:
    int    lb        = 30;      // trailing-return lookback (days)
    int    K         = 3;       // top-K long
    int    vol_win   = 30;      // realized-vol window for inverse-vol weights
    int    regime_ma = 0;       // BTC macro-gate REMOVED 2026-07-06 (feedback-no-200dma-crypto)
    bool   macro_gate = false;  // NO 200DMA anywhere in crypto (operator hard rule)
    std::string btc_key = "btcusdt";

    // ---- signal primitives (faithful to xsec_allocator.py) ---------------------
    static bool nz(double x){ return x==x; }  // not-NaN

    static double sma(const std::vector<double>& s, int i, int n){
        if(i < n) return NAN;
        int cnt=0; double sum=0.0;
        for(int j=i-n;j<i;++j) if(nz(s[j])){ sum+=s[j]; ++cnt; }
        if(cnt < (int)(n*0.8)) return NAN;
        return sum/cnt;
    }
    static double trailing_ret(const std::vector<double>& s, int i, int lb){
        if(i < lb) return NAN;
        double a=s[i-lb], b=s[i];
        if(!nz(a)||!nz(b)||a<=0.0) return NAN;
        return b/a - 1.0;
    }
    static double realized_vol(const std::vector<double>& s, int i, int n){
        if(i < n+1) return NAN;
        std::vector<double> rs;
        for(int j=i-n;j<i;++j){ double a=s[j-1],b=s[j];
            if(nz(a)&&nz(b)&&a>0.0) rs.push_back(b/a-1.0); }
        if((int)rs.size() < (int)(n*0.6)) return NAN;
        double m=0.0; for(double x:rs) m+=x; m/=rs.size();
        double var=0.0; for(double x:rs) var+=(x-m)*(x-m); var/=rs.size();
        return var>0.0 ? std::sqrt(var) : NAN;
    }

    // BTC 200d-SMA macro gate — DISABLED (macro_gate=false by default, 2026-07-06 no-200dma rule).
    // Always returns true now; SMA path kept only for the disabled/legacy branch.
    bool is_bull(const std::unordered_map<std::string,std::vector<double>>& close, int i) const {
        if(!macro_gate) return true;
        auto it = close.find(btc_key);
        if(it==close.end()) return true;
        const auto& btc = it->second;
        double m = sma(btc, i, regime_ma);
        return nz(m) && nz(btc[i]) && btc[i] > m;
    }

    // Return {internal: weight}, weights sum to <=1 (cash = remainder). Empty = all cash.
    std::unordered_map<std::string,double>
    target_weights(const std::unordered_map<std::string,std::vector<double>>& close, int i) const {
        std::unordered_map<std::string,double> w;
        if(!is_bull(close, i)) return w;
        std::vector<std::pair<double,std::string>> scores;
        for(const auto& kv : close){
            double sc = trailing_ret(kv.second, i, lb);
            if(!nz(sc) || sc <= 0.0) continue;         // long-only: positive momentum only
            scores.emplace_back(sc, kv.first);
        }
        std::sort(scores.begin(), scores.end(),
                  [](const auto& a, const auto& b){ return a.first > b.first; });
        std::vector<std::string> picks;
        for(int k=0;k<(int)scores.size() && k<K;++k) picks.push_back(scores[k].second);
        if(picks.empty()) return w;
        std::unordered_map<std::string,double> iv; double tot=0.0;
        for(const auto& s : picks){
            double v = realized_vol(close.at(s), i, vol_win);
            double val = (nz(v) && v>0.0) ? 1.0/v : 0.0;
            iv[s]=val; tot+=val;
        }
        if(tot > 0.0) for(const auto& s:picks) w[s]=iv[s]/tot;
        else          for(const auto& s:picks) w[s]=1.0/picks.size();
        return w;
    }

    // ---- data loading ----------------------------------------------------------
    // Load a coin's daily CSV (time,open,high,low,close[,vol]) -> {day_index: close}.
    // Handles ms (ts>1e12) or sec timestamps; header rows fail the numeric parse.
    static std::map<long,double> load_daily_csv(const std::string& path){
        std::map<long,double> out;
        std::ifstream f(path);
        std::string line;
        while(std::getline(f,line)){
            if(line.empty()) continue;
            std::stringstream ss(line); std::string tok; double v[5]; int col=0; bool ok=true;
            while(std::getline(ss,tok,',') && col<5){
                char* e=nullptr; v[col]=std::strtod(tok.c_str(),&e);
                if(e==tok.c_str()){ ok=false; break; } ++col;
            }
            if(!ok || col<5) continue;
            long ts=(long)v[0];
            long day = (ts > 1000000000000L) ? ts/86400000L : ts/86400L;
            out[day]=v[4];   // last row for a day wins (daily bars are already 1/day)
        }
        return out;
    }

    // Build union-day-aligned close series across coins. Returns the aligned map and
    // sets last_i to the final union-day index (the bar to allocate ON).
    static std::unordered_map<std::string,std::vector<double>>
    align(const std::unordered_map<std::string,std::map<long,double>>& raw, int& last_i){
        std::vector<long> days;
        for(const auto& kv:raw) for(const auto& d:kv.second) days.push_back(d.first);
        std::sort(days.begin(),days.end());
        days.erase(std::unique(days.begin(),days.end()),days.end());
        std::unordered_map<std::string,std::vector<double>> close;
        for(const auto& kv:raw){
            std::vector<double> v; v.reserve(days.size());
            for(long d:days){ auto it=kv.second.find(d); v.push_back(it!=kv.second.end()?it->second:NAN); }
            close[kv.first]=std::move(v);
        }
        last_i = days.empty()? -1 : (int)days.size()-1;
        return close;
    }
};

} // namespace ibkrcrypto
#endif
