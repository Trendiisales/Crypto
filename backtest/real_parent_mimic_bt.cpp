// real_parent_mimic_bt.cpp — REAL-PARENT-replay mimic_floor g-sweep, S-2026-07-16.
//
// The operator's option-b mandate (handoff 2026-07-16): re-test the ETH/LDO + 5
// REGIME floored-mimic proposal against the REAL parent engine windows — NOT the
// crude +/-thr up-jump proxy the first pass used (that mis-framed a viable ETH cell
// as NO-GO). feedback-verify-kill-replicates-mechanism / feedback-test-operator-
// spec-before-verdict: replicate the REAL parent, judge the mimic STANDALONE.
//
// THREE parent kinds, each the REAL deployed engine (parity by construction):
//   REGIME  (ADA/DOT/NEAR/SUSHI/THETA) — chimera::EdgeEngine REGIME_SWITCH on D1
//            (1h aggregated -> D1), make_regime config (main.cpp:4049). Parent state
//            read per bar via in_position()/entry_px().
//   ETH     — chimera::UpJumpLadderCompanion jump_floor cell PJ7W24
//            (W=24 thr=7% s=400bp g=1.0) on 1h. Parent state via jf_in_position()/
//            jf_entry_px().
//   LDO     — chimera::CryptoCampaignManager LDO-CAMP-W8 (CW8-7.0) on 1h. Parent
//            state via campaign_open()/campaign_entry_px().
//
// The MIMIC is an INDEPENDENT additive book (feedback-companion-independent-engine):
// a fresh UpJumpLadderCompanion in mimic_floor mode, driven by the parent's settled
// (in_position, entry_px) each bar via observe() — EXACTLY the live wiring
// (main.cpp:1479). Params = the DEPLOYED make_be_mimic (main.cpp:4081): confirm 20bp
// BE-entry, cap=1 single leg, mimic_floor BE-floor + HWM trail by g (swept), reclip
// +5%, loss_cut 60bp le-anchored, RT = cost. Judged STANDALONE: own book net+ after
// MEASURED cost (base 28bp = CryptoCostLedger safe_cost floor / ETH depth-measured),
// PF>=1.3, WF both halves, AND at 2x cost (56bp, the thin-coin depth-slip stress).
//
// Build: clang++ -std=c++17 -O2 -I/Users/jo/ChimeraCrypto/include real_parent_mimic_bt.cpp -o real_parent_mimic_bt
// Data:  /Users/jo/Crypto/backtest/data/<COIN>USDT_1h.csv  (open_time_ms,o,h,l,c)
// Env:   RP_COIN(ETH) RP_KIND(auto|regime|eth|ldo) RP_RT(28) RP_CONFIRM(20)
//        RP_RECLIP(0.05) RP_LOSSCUT(60) MF_G(comma list) RP_YEAR_MIN(2023, omit 2022)

#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <cmath>
#include <string>
#include <vector>
#include <map>
#include <algorithm>
#include <fstream>
#include <sstream>
#include <unistd.h>
#include <fcntl.h>
#include "core/EdgeEngine.hpp"
#include "core/UpJumpLadderCompanion.hpp"
#include "core/CryptoCampaignManager.hpp"

using chimera::EdgeEngine;
using chimera::UpJumpLadderCompanion;
using chimera::CryptoCampaignManager;

struct Bar { int64_t ts; double o,h,l,c; };
// one driven parent bar: parent settled state + the bar's close/low for the mimic
struct Tape { int64_t ts; bool in_pos; double epx, close, low; };

static std::vector<std::string> split(const std::string& s){
    std::vector<std::string> v; std::stringstream ss(s); std::string t;
    while(std::getline(ss,t,',')) v.push_back(t); return v; }

static std::vector<Bar> load_1h(const std::string& coin){
    std::string dir = "/Users/jo/Crypto/backtest/data";
    std::vector<Bar> b; std::ifstream f(dir + "/" + coin + "USDT_1h.csv");
    if(!f){ std::fprintf(stderr,"no 1h data for %s in %s\n",coin.c_str(),dir.c_str()); return b; }
    std::string ln; std::getline(f,ln);   // header
    while(std::getline(f,ln)){ auto v=split(ln); if(v.size()<5) continue;
        Bar x; x.ts=(int64_t)std::stoll(v[0]); x.o=std::stod(v[1]); x.h=std::stod(v[2]);
        x.l=std::stod(v[3]); x.c=std::stod(v[4]);
        if(x.o>0&&x.h>0&&x.l>0&&x.c>0) b.push_back(x); }
    return b;
}

static int year_of(int64_t ms){ return 1970 + (int)(ms/1000/31557600); }

// 1h -> D1 aggregation (UTC day bucket). o=first h=max l=min c=last.
static std::vector<Bar> agg_d1(const std::vector<Bar>& h){
    std::vector<Bar> d; int64_t cur=-1; Bar acc{};
    for(const auto& x : h){ int64_t day = x.ts/86400000LL;
        if(day!=cur){ if(cur>=0) d.push_back(acc); cur=day;
            acc.ts=day*86400000LL; acc.o=x.o; acc.h=x.h; acc.l=x.l; acc.c=x.c; }
        else { acc.h=std::max(acc.h,x.h); acc.l=std::min(acc.l,x.l); acc.c=x.c; } }
    if(cur>=0) d.push_back(acc);
    return d;
}

// ── PARENT #1: REGIME_SWITCH D1 (real EdgeEngine, make_regime config) ──────────
static std::vector<Tape> tape_regime(const std::string& coin, double cost){
    std::vector<Bar> d1 = agg_d1(load_1h(coin));
    std::vector<Tape> tp;
    if((int)d1.size() < 70) return tp;
    std::string low=coin; for(auto&ch:low) ch=(char)tolower((unsigned char)ch); low+="usdt";
    EdgeEngine::Config c{};
    c.symbol=low; c.tag=coin+"-REGIME_SWITCH"; c.kind=chimera::StrategyKind::REGIME_SWITCH;
    c.tf_secs=86400; c.lookback=20; c.hold_bars=12; c.sl_atr_mult=3.0;
    c.atr_period=14; c.ride_to_flip=true; c.round_trip_bp=cost; c.max_history=64;
    c.hard_floor_bp=0.0; c.early_kill_bp=0.0; c.early_kill_mfe=0.0;
    c.early_kill_min_hold_ms=0; c.giveback_arm_bp=0.0; c.signal_confirm_bars=1;
    c.ratchet_start_bp=20.0; c.be_arm_bp=30.0; c.ratchet_lock_pct=0.75;
    c.prog_lock_pct_2=0.85; c.prog_lock_pct_3=0.90; c.prog_lock_pct_4=0.95;
    c.trail_arm_atr=1.0; c.trail_dist_atr=0.4;
    c.trail_tighten_atr=3.0; c.trail_tighten_dist_atr=0.25;
    c.realistic_gap_fill=false;                     // Phase-2 parity (stops fill AT level)
    // suppress engine prints
    fflush(stdout); int saved=dup(1); { int dn=open("/dev/null",O_WRONLY); dup2(dn,1); close(dn); }
    EdgeEngine eng(c);
    int seed=60;
    std::vector<EdgeEngine::SeedBar> sb; sb.reserve(seed);
    for(int i=0;i<seed;i++){ EdgeEngine::SeedBar s; s.open_ts_ms=d1[i].ts; s.o=d1[i].o; s.h=d1[i].h; s.l=d1[i].l; s.c=d1[i].c; sb.push_back(s); }
    eng.seed_bars(sb);
    for(int i=seed;i<(int)d1.size();i++){ const Bar& k=d1[i]; int64_t t0=k.ts;
        int64_t step=(c.tf_secs*1000)/4;
        eng.on_tick(k.o,t0); eng.on_tick(k.l,t0+step); eng.on_tick(k.h,t0+step*2);
        int64_t ts_c=t0+c.tf_secs*1000+1000;
        tp.push_back({ts_c, eng.in_position(), eng.entry_px(), k.c, k.l}); }
    eng.graceful_close(d1.back().c, d1.back().ts+c.tf_secs*1000+2000);
    fflush(stdout); dup2(saved,1); close(saved);
    return tp;
}

// ── PARENT #2: ETH jump_floor cell PJ7W24 (real UpJumpLadderCompanion) ─────────
static std::vector<Tape> tape_eth(const std::string& coin, double cost){
    std::vector<Bar> h = load_1h(coin);
    std::vector<Tape> tp; if((int)h.size()<60) return tp;
    UpJumpLadderCompanion::Config c;
    c.parent_tag="ETH-PJ7W24-FEED"; c.tag="ETH-PJ7W24"; c.symbol="ethusdt";
    c.det_w=24; c.det_thr=0.070; c.jump_floor=true; c.jf_giveback=1.0; c.jf_prebe_stop_bp=400.0;
    c.tf_secs=3600; c.round_trip_bp=cost; c.confirm_bp=0.0; c.loss_cut_bp=0.0;
    c.be_floor=false; c.reclip_pct=0.0; c.cap=1; c.cost_gate_bp=0.0; c.size_mult=1.0;
    fflush(stdout); int saved=dup(1); { int dn=open("/dev/null",O_WRONLY); dup2(dn,1); close(dn); }
    UpJumpLadderCompanion eng(c);
    for(const auto& k : h){
        // jump_floor self-detects + fills intrabar; feed adverse-then-favourable then close.
        eng.stop_check_only(k.l, k.ts);
        eng.observe(true, 0.0, k.c, k.ts);
        tp.push_back({k.ts, eng.jf_in_position(), eng.jf_entry_px(), k.c, k.l});
    }
    fflush(stdout); dup2(saved,1); close(saved);
    return tp;
}

// ── PARENT #3: LDO campaign CW8-7.0 (real CryptoCampaignManager) ───────────────
static std::vector<Tape> tape_ldo(const std::string& coin, double cost){
    std::vector<Bar> h = load_1h(coin);
    std::vector<Tape> tp; if((int)h.size()<60) return tp;
    static chimera::CryptoCostLedger led; static chimera::CryptoOpportunityGate gate;
    led.configure("ldousdt", 20.0, 3.0, 2.0);
    // CellCfg: {tag, cell, W, thr, rt_bp, ?, ?, ?, g, prebe_stop_bp, retire_bp}
    CryptoCampaignManager::Config cc;
    cc.symbol="ldousdt"; cc.pfx="LDO"; cc.tf_secs=3600; cc.mimic_enabled=false;
    cc.cells = { {"LDO-CAMP-W8","CW8-7.0",8,0.070,20.0,411.0,342.0,48.0,0.25,-1400.0,40.0} };
    fflush(stdout); int saved=dup(1); { int dn=open("/dev/null",O_WRONLY); dup2(dn,1); close(dn); }
    CryptoCampaignManager mgr(cc, &led, &gate);
    for(const auto& k : h){
        // campaign on_tick drives on marks; feed o/l/h/c path within the bar
        int64_t step=(cc.tf_secs*1000)/4;
        mgr.on_tick(k.o, k.ts);
        mgr.on_tick(k.l, k.ts+step);
        mgr.on_tick(k.h, k.ts+step*2);
        mgr.on_tick(k.c, k.ts+step*3);
        tp.push_back({k.ts, mgr.campaign_open(), mgr.campaign_entry_px(), k.c, k.l});
    }
    fflush(stdout); dup2(saved,1); close(saved);
    return tp;
}

struct Rec { int64_t ts; double net; };
struct R { int n; double net,pf,worst; int neg; double negsum,h1,h2,bestepi; };

// drive the DEPLOYED mimic_floor over the parent tape at giveback g / cost rt
static R run_mimic(const std::vector<Tape>& tp, double g, double rt, double confirm,
                   double reclip, double losscut, int64_t tf_secs, int year_min){
    UpJumpLadderCompanion::Config c; c.parent_tag="BT"; c.tag="BT"; c.symbol="bt";
    c.tight={0.2,0,0.0,0}; c.reclip_pct=reclip; c.cap=1; c.cost_gate_bp=0.0;
    c.confirm_bp=confirm; c.be_floor=false; c.det_w=0; c.tf_secs=tf_secs; c.round_trip_bp=rt;
    c.loss_cut_bp=losscut; c.mimic_floor=true; c.mimic_giveback=g;
    fflush(stdout); int saved=dup(1); { int dn=open("/dev/null",O_WRONLY); dup2(dn,1); close(dn); }
    UpJumpLadderCompanion eng(c);
    std::vector<Rec> rows; int64_t cur_ts=0;
    eng.set_on_clip([&](const UpJumpLadderCompanion::ClipRecord& r){
        if(year_of(cur_ts) >= year_min) rows.push_back({cur_ts, r.net_bp_real}); });
    for(const auto& t : tp){ cur_ts=t.ts;
        eng.stop_check_only(t.low, t.ts);
        eng.observe(t.in_pos, t.epx, t.close, t.ts); }
    // flush: parent flat -> MTM any open leg
    if(!tp.empty()){ cur_ts=tp.back().ts; eng.observe(false, 0.0, tp.back().close, tp.back().ts+tf_secs*1000+2000); }
    fflush(stdout); dup2(saved,1); close(saved);

    double net=0,gw=0,gl=0,worst=0,negsum=0; int neg=0;
    for(auto&r:rows){ net+=r.net/100.0; if(r.net>0) gw+=r.net; else {gl-=r.net;neg++;negsum+=r.net/100.0;} if(r.net<worst) worst=r.net; }
    double pf=gl>0?gw/gl:(gw>0?999:0);
    std::vector<Rec> so=rows; std::sort(so.begin(),so.end(),[](const Rec&a,const Rec&c){return a.ts<c.ts;});
    double h1=0,h2=0; for(size_t k=0;k<so.size();k++){ if(k<so.size()/2) h1+=so[k].net/100.0; else h2+=so[k].net/100.0; }
    return R{(int)rows.size(),net,pf,worst,neg,negsum,h1,h2,0.0};
}

int main(int argc,char**argv){
    std::string coin = getenv("RP_COIN")?getenv("RP_COIN"):"ETH";
    std::string kind = getenv("RP_KIND")?getenv("RP_KIND"):"auto";
    double rt      = getenv("RP_RT")?atof(getenv("RP_RT")):28.0;
    double confirm = getenv("RP_CONFIRM")?atof(getenv("RP_CONFIRM")):20.0;
    double reclip  = getenv("RP_RECLIP")?atof(getenv("RP_RECLIP")):0.05;
    double losscut = getenv("RP_LOSSCUT")?atof(getenv("RP_LOSSCUT")):60.0;
    int year_min   = getenv("RP_YEAR_MIN")?atoi(getenv("RP_YEAR_MIN")):2023;
    std::vector<double> gs; { const char* ge=getenv("MF_G");
        std::string gl=ge?ge:"1.0,0.90,0.75,0.60,0.50,0.40,0.30,0.25,0.20,0.15,0.10,0.05";
        std::stringstream ss(gl); std::string t; while(std::getline(ss,t,',')) if(!t.empty()) gs.push_back(atof(t.c_str())); }

    if(kind=="auto"){ if(coin=="ETH") kind="eth"; else if(coin=="LDO") kind="ldo"; else kind="regime"; }
    int64_t tf = (kind=="regime") ? 86400 : 3600;

    std::vector<Tape> tp;
    if(kind=="regime") tp = tape_regime(coin, rt);
    else if(kind=="eth") tp = tape_eth(coin, rt);
    else if(kind=="ldo") tp = tape_ldo(coin, rt);
    if(tp.empty()){ std::fprintf(stderr,"empty tape for %s (%s)\n",coin.c_str(),kind.c_str()); return 1; }

    int in_bars=0; for(auto&t:tp) if(t.in_pos) in_bars++;
    std::printf("REAL-PARENT mimic_floor g-sweep — %s [%s]  confirm=%.0fbp reclip=%.1f%% losscut=%.0f RT=%.0fbp  tapeBars=%zu parentInPos=%d  gate>=%d\n",
        coin.c_str(),kind.c_str(),confirm,reclip*100,losscut,rt,tp.size(),in_bars,year_min);
    std::printf("%-5s | %5s %9s %6s %9s | %5s %9s | %9s %9s | %s\n",
        "g","n","net%","PF","worst_bp","nNeg","sumNeg%","H1%","H2%","GATE(base;2x)");
    for(double g:gs){ R r=run_mimic(tp,g,rt,confirm,reclip,losscut,tf,year_min);
        R r2=run_mimic(tp,g,rt*2.0,confirm,reclip,losscut,tf,year_min);
        bool gate = r.net>0&&r.pf>=1.3&&r.h1>0&&r.h2>0 && r2.net>0&&r2.pf>=1.3;
        std::printf("%5.2f | %5d %+9.0f %6.2f %+9.1f | %5d %+9.1f | %+9.0f %+9.0f | %s [2x net%+.0f pf%.2f]\n",
            g,r.n,r.net,r.pf,r.worst,r.neg,r.negsum,r.h1,r.h2, gate?"PASS":"fail", r2.net,r2.pf);
    }
    return 0;
}
