// CryptoWaveCompanion — WAVE-STACKED long-only companion engine (operator ask 2026-07-04).
//
// A SEPARATE INDEPENDENT engine (operator hard rule, both systems): it NEVER touches the
// parent it rides; every companion clip is a STANDALONE additive book, judged all-6, NEVER
// vs riding WIDE (CompanionDominanceError). Long-only.
//
// NO 200DMA anywhere (operator hard rule 2026-07-06, feedback-no-200dma-crypto): the former
// daily-close>200DMA BULL-GATE is REMOVED. Waves arm on price structure alone, any regime.
//
// MODEL (locked from backtest/crypto_ladder_companion.py + crypto_wave_companions.py,
// verdict S-2026-07-04):
//   * ARM STEP  : a wave arms on the first +STEP% advance above a trailing-low reference;
//                 each further +STEP% NEW HIGH opens ANOTHER stacked companion (uncapped).
//   * REVERSAL  : price gives back REV% from the wave peak  -> close ALL companions.
//   * STAGNATION: no new wave high within STAG_H hours (PER-COIN) -> close ALL companions.
//   * COLD CUT  : per-companion, if its OWN price falls COLD% below its arm while the wave
//                 is still open, cut THAT companion early at its stop (independent book).
//                 Backtested S-2026-07-05: this cut is the ONLY per-trade reversal guard
//                 (REV/STAG close the whole stack, not a per-companion loss). Tighter=safer:
//                 4% DOMINATES 8% at the per-coin STAGs -- MORE net AND ~half the worst clip,
//                 all-6 pass, both halves + all thirds positive, smooth (non-overfit) gradient.
//
// Defaults (locked config S-2026-07-05): STEP 1% / STAG ETH 24h + BTC 48h / REV 15% / COLD 4%.
// Parity target — SUPERSEDED by the 2026-07-06 200DMA removal. The figures below were the
// bull-GATED backtest (5.5yr Binance 1h, raw price-unit net, COLD 4%):
//     ETH@24h ~ $64,911   BTC@48h ~ $683,010   (companions/wave mean ETH 4.9 / BTC 5.4; worst clip
//     ETH -$293  BTC -$5,165). REV 15% kept: tightening it hurts net + gives NO worst-clip gain.
// Un-gated (arms every regime) the book is LARGER but NOT independently backtested for the extra
// bear-regime waves it now takes -- treat forward figures as observe-only until re-swept.
//
// Faithful-by-construction: each run REPLAYS the full 1h history (a pure function of the
// data), so the book can never drift from the backtest. State = the emitted ledger.
//
// Build:  crypto_exe(wave_companion src/wave_companion.cpp Threads::Threads)  [pure stdlib]
#include <cstdio>
#include <cmath>
#include <string>
#include <vector>
#include <map>
#include <algorithm>
#include <numeric>
#include <fstream>
#include <sstream>
#include <sys/stat.h>
#include "json.hpp"
#include "crypto/Roster.hpp"      // env_or, csv_dir
#include "crypto/DataHealth.hpp"  // integrity_gate, data_age_days
#include "crypto/StateJson.hpp"   // round_n, json
#include "crypto/ShadowBook.hpp"  // utc_now_min

using crypto::json;
using namespace crypto;

static bool file_exists(const std::string& p){ struct stat s; return stat(p.c_str(),&s)==0; }
static std::string fmt(double v,int p){ char b[64]; std::snprintf(b,sizeof(b),"%.*f",p,v); return b; }

// ---- CSV loaders (open_time_ms,open,high,low,close) ----
struct Bar { long ms; double c; };
static std::vector<Bar> load_1h(const std::string& path){
    std::vector<Bar> v; std::ifstream f(path); if(!f.is_open()) return v;
    std::string ln; bool first=true;
    while(std::getline(f,ln)){
        if(ln.empty()) continue;
        if(first){ first=false; if(ln[0]<'0'||ln[0]>'9') continue; }
        long t; double o,h,l,c;
        if(std::sscanf(ln.c_str(),"%ld,%lf,%lf,%lf,%lf",&t,&o,&h,&l,&c)!=5) continue;
        if(t<1000000000000L) t*=1000;
        v.push_back({t,c});
    }
    std::sort(v.begin(),v.end(),[](const Bar&a,const Bar&b){return a.ms<b.ms;});
    return v;
}
struct Comp { long arm_ms; double arm_px, cold_stop; bool cold; long exit_ms; double exit_px; };
struct Wave { long start_ms; std::vector<Comp> comps; double peak; long peak_ms;
              long exit_ms; double exit_px; bool open; };

int main(){
    const double STEP   = std::stod(env_or("STEP",   "0.01"));
    // STAG is PER-COIN (backtest S-2026-07-05): ETH 24h / BTC 48h -- each coin's net-max at the
    // locked COLD 4%. STAG_H (global) overrides BOTH coins when set >0 (parity sweeps only).
    const double STAG_H_OVR = std::stod(env_or("STAG_H","0"));
    const double REV    = std::stod(env_or("REV",    "0.15"));
    const double COLD   = std::stod(env_or("COLD",   "0.04"));   // 0 disables the per-companion cut
    const double COST_RT= std::stod(env_or("COST_RT","0.002"));
    const double POOL   = std::stod(env_or("POOL_USD","10000"));
    const double CUSD   = std::stod(env_or("COMPANION_USD","1000")); // $ notional per companion (shadow)
    const std::string DATADIR = env_or("WAVECOMP_DATADIR",
                                       "/Users/jo/Crypto/backtest/data/wave_companion");
    mkdir(DATADIR.c_str(),0755);
    const std::string STATE = DATADIR + "/state.json";
    const std::string LEDGER= DATADIR + "/ledger.csv";
    const std::string now = utc_now_min();
    const std::vector<std::string> COINS = { "BTC", "ETH" };  // backtested/validated scope only

    bool newled = !file_exists(LEDGER);
    std::ofstream led(LEDGER, std::ios::app);
    if(newled) led << "exit_ts,coin,wave_start_ms,arm_ms,arm_px,exit_px,ret_frac,pnl_usd,exit_kind\n";

    json coins_json = json::array();
    double tot_real_usd=0, tot_open_usd=0, tot_raw_unit=0;  // raw_unit = price-unit net (parity check)
    int tot_open_comp=0, tot_closed_comp=0;

    for(const auto& coin : COINS){
        // per-coin stagnation window (global STAG_H override wins for parity sweeps)
        const std::string stag_key = std::string("STAG_")+coin;
        double stag_h = STAG_H_OVR>0 ? STAG_H_OVR
                      : std::stod(env_or(stag_key.c_str(), coin=="BTC" ? "48" : "24"));
        const long STAG_MS = (long)(stag_h*3600000L);
        const std::string p1 = csv_dir()+"/"+coin+"USDT_1h.csv";
        auto bars = load_1h(p1);
        DataAge age = data_age_days(p1);
        bool integ = integrity_gate(p1);
        bool fresh = age.bar <= 3.0 && age.file <= 3.0;  // 1h book: ~3d bound
        if(bars.empty() || !integ){
            std::fprintf(stderr,"[wave-comp] %s SKIP (empty=%d integ=%d)\n",coin.c_str(),bars.empty(),integ);
            coins_json.push_back({{"coin",coin},{"skipped",true},{"integ",integ},{"fresh",fresh}});
            continue;
        }

        // ---- replay: reconstruct every wave + companion exactly as the backtest ----
        std::vector<Wave> waves;
        size_t n = bars.size(), i = 0;
        double ref = bars[0].c;
        while(i < n){
            long ms = bars[i].ms; double px = bars[i].c;
            if(px < ref) ref = px;
            if(px >= ref*(1+STEP)){
                // NO 200DMA gate (operator hard rule 2026-07-06): arm on structure, any regime.
                Wave w; w.start_ms=ms; w.peak=px; w.peak_ms=ms; w.open=true;
                double last_arm=px;
                w.comps.push_back({ms,px,px*(1-COLD),false,0,0.0});    // companion #1
                size_t j=i+1; w.exit_px=px; w.exit_ms=ms;
                for(; j<n; ++j){
                    long mj=bars[j].ms; double pj=bars[j].c;
                    if(pj > w.peak){
                        w.peak=pj; w.peak_ms=mj;
                        while(pj >= last_arm*(1+STEP)){                // stack a new companion each +STEP%
                            last_arm *= (1+STEP);
                            w.comps.push_back({mj,last_arm,last_arm*(1-COLD),false,0,0.0});
                        }
                    }
                    if(pj <= w.peak*(1-REV)){ w.exit_px=pj; w.exit_ms=mj; break; }   // REVERSAL
                    if(mj - w.peak_ms >= STAG_MS){ w.exit_px=pj; w.exit_ms=mj; break; } // STAGNATION
                    w.exit_px=pj; w.exit_ms=mj;
                    // per-companion COLD cut — ONLY on non-exit bars (Python clips(): mj < wexit_ms),
                    // fill at the stop. Runs after the exit checks so the exit bar is excluded.
                    if(COLD>0) for(auto& cmp: w.comps)
                        if(cmp.exit_ms==0 && pj<=cmp.cold_stop){ cmp.cold=true; cmp.exit_ms=mj; cmp.exit_px=cmp.cold_stop; }
                }
                w.open = (j>=n);                                       // still open only if we ran off the data
                // settle: any companion not cold-cut exits at the wave exit
                for(auto& cmp: w.comps) if(cmp.exit_ms==0){ cmp.exit_ms=w.exit_ms; cmp.exit_px=w.exit_px; }
                double exit_px_for_ref = w.exit_px;
                waves.push_back(std::move(w));
                ref = exit_px_for_ref;      // resume: a fresh +STEP advance above the exit re-arms
                i = j + 1;
            } else ++i;
        }

        // ---- book the clips ----
        double coin_real=0, coin_open=0, coin_raw=0; int nopen=0, nclosed=0;
        std::vector<int> counts;
        json open_comps = json::array();
        double last_px = bars.back().c;
        for(auto& w : waves){
            counts.push_back((int)w.comps.size());
            for(auto& cmp : w.comps){
                double ret = (cmp.exit_px - cmp.arm_px)/cmp.arm_px - COST_RT;
                double raw = (cmp.exit_px - cmp.arm_px) - COST_RT*cmp.arm_px;   // price-unit (parity)
                double usd = CUSD * ret;
                coin_raw += raw;
                if(w.open){
                    // live wave: companion still riding -> mark-to-market as OPEN (unreal)
                    double mret = (last_px - cmp.arm_px)/cmp.arm_px - COST_RT;
                    coin_open += CUSD*mret; ++nopen;
                    open_comps.push_back({
                        {"arm_px",round_n(cmp.arm_px,4)},{"cold_stop",round_n(cmp.cold_stop,4)},
                        {"mret_frac",round_n(mret,5)},{"unreal_usd",round_n(CUSD*mret,2)}
                    });
                } else {
                    coin_real += usd; ++nclosed;
                    std::string kind = cmp.cold ? "COLD" :
                        (w.exit_px <= w.peak*(1-REV) ? "REVERSAL" : "STAGNATION");
                    led << now << "," << coin << "," << w.start_ms << "," << cmp.arm_ms << ","
                        << fmt(cmp.arm_px,4) << "," << fmt(cmp.exit_px,4) << "," << fmt(ret,6) << ","
                        << fmt(usd,2) << "," << kind << "\n";
                }
            }
        }
        led.flush();
        std::sort(counts.begin(),counts.end());
        auto qc=[&](double q){ return counts.empty()?0:counts[std::min(counts.size()-1,(size_t)(q*counts.size()))]; };
        double meanc = counts.empty()?0: (double)std::accumulate(counts.begin(),counts.end(),0)/counts.size();

        std::fprintf(stderr,"[wave-comp] %s: %zu waves, %d closed comps, raw-unit net $%.0f, "
                     "realized(pool) $%.2f, open %d, comps/wave mean %.1f max %d\n",
                     coin.c_str(), waves.size(), nclosed, coin_raw, coin_real, nopen,
                     meanc, counts.empty()?0:counts.back());

        tot_real_usd+=coin_real; tot_open_usd+=coin_open; tot_raw_unit+=coin_raw;
        tot_open_comp+=nopen; tot_closed_comp+=nclosed;
        coins_json.push_back({
            {"coin",coin},{"skipped",false},{"fresh",fresh},{"integ",integ},{"stag_h",stag_h},
            {"waves",(int)waves.size()},{"closed_comps",nclosed},{"open_comps",nopen},
            {"realized_usd",round_n(coin_real,2)},{"open_unreal_usd",round_n(coin_open,2)},
            {"raw_unit_net",round_n(coin_raw,0)},
            {"comps_per_wave",{{"mean",round_n(meanc,1)},{"p50",qc(.5)},{"p90",qc(.9)},{"max",counts.empty()?0:counts.back()}}},
            {"open",open_comps},
            {"bar_age_d",round_n(age.bar,2)},{"file_age_d",round_n(age.file,2)}
        });
    }
    led.close();

    json out = {
        {"engine","CryptoWaveCompanion"},{"mode","SHADOW"},{"updated",now+" UTC"},
        {"config",{{"step",STEP},{"stag_per_coin","ETH 24h / BTC 48h"},
                   {"stag_h_override",STAG_H_OVR},{"rev",REV},{"cold",COLD},
                   {"cost_rt",COST_RT},{"pool_usd",POOL},{"companion_usd",CUSD},
                   {"bull_gate","NONE (200DMA removed 2026-07-06)"},{"long_only",true}}},
        {"realized_usd",round_n(tot_real_usd,2)},{"open_unreal_usd",round_n(tot_open_usd,2)},
        {"total_usd",round_n(tot_real_usd+tot_open_usd,2)},
        {"bank_bp",round_n((tot_real_usd+tot_open_usd)/POOL*10000.0,1)},
        {"raw_unit_net",round_n(tot_raw_unit,0)},
        {"open_comps",tot_open_comp},{"closed_comps",tot_closed_comp},
        {"coins",coins_json}
    };
    std::ofstream sf(STATE); sf << out.dump(1); sf.close();
    std::printf("wave-companion: %d coins, %d closed comps, %d open, realized(pool) $%.2f, raw-unit net $%.0f\n",
                (int)COINS.size(), tot_closed_comp, tot_open_comp, tot_real_usd, tot_raw_unit);
    return 0;
}
