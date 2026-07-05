// Byte-exact validation of the native UpJumpLadderCompanion vs the python
// crypto_upjump_tiered_ladder_sweep.py Leg/run_trade path (deploy rule #4).
// Reads roster_cfg.csv + parent_trades.csv + <COIN>USDT_1h.csv, replays every
// parent trade through the engine, writes cpp_clips.csv (coin,trade_idx,clip_seq,net_bp).
// Build: g++ -std=c++17 -I../../ChimeraCrypto/include validate_ladder.cpp -o validate_ladder
#include "core/UpJumpLadderCompanion.hpp"
#include <cstdio>
#include <fstream>
#include <sstream>
#include <string>
#include <vector>
#include <map>

using chimera::UpJumpLadderCompanion;
static const int64_t TF_SECS = 3600;

struct Cfg { double W, thr, cg, ta, ts, tg, wa, ws, wg, rc; int cap; };

static std::vector<std::string> split(const std::string& s) {
    std::vector<std::string> out; std::stringstream ss(s); std::string t;
    while (std::getline(ss, t, ',')) out.push_back(t);
    return out;
}

int main() {
    // roster_cfg
    std::map<std::string, Cfg> roster;
    { std::ifstream f("roster_cfg.csv"); std::string ln; std::getline(f, ln);
      while (std::getline(f, ln)) { auto v = split(ln); if (v.size() < 12) continue;
        Cfg c; c.W=std::stod(v[1]); c.thr=std::stod(v[2]); c.cg=std::stod(v[3]);
        c.ta=std::stod(v[4]); c.ts=std::stod(v[5]); c.tg=std::stod(v[6]);
        c.wa=std::stod(v[7]); c.ws=std::stod(v[8]); c.wg=std::stod(v[9]);
        c.rc=std::stod(v[10]); c.cap=std::stoi(v[11]); roster[v[0]] = c; } }

    // parent_trades grouped by coin (preserve order)
    std::vector<std::string> coin_order;
    std::map<std::string, std::vector<std::tuple<int,int,double>>> trades;  // coin -> (ei,xi,epx)
    { std::ifstream f("parent_trades.csv"); std::string ln; std::getline(f, ln);
      while (std::getline(f, ln)) { auto v = split(ln); if (v.size() < 4) continue;
        if (trades.find(v[0]) == trades.end()) coin_order.push_back(v[0]);
        trades[v[0]].emplace_back(std::stoi(v[1]), std::stoi(v[2]), std::stod(v[3])); } }

    std::ofstream out("cpp_clips.csv"); out << "coin,trade_idx,clip_seq,net_bp\n";

    for (const auto& coin : coin_order) {
        // load closes
        std::vector<double> c;
        { std::ifstream f("data/" + coin + "USDT_1h.csv"); std::string ln; std::getline(f, ln);
          while (std::getline(f, ln)) { auto v = split(ln); if (v.size() < 5) continue; c.push_back(std::stod(v[4])); } }
        const Cfg& cf = roster.at(coin);

        int ti = 0;
        for (auto& tr : trades[coin]) {
            int ei = std::get<0>(tr), xi = std::get<1>(tr); double epx = std::get<2>(tr);
            UpJumpLadderCompanion::Config c2;
            c2.parent_tag = coin + "-UPJUMP-H1"; c2.tag = coin + "-UPJUMP-CLIP"; c2.symbol = coin;
            c2.tight = {cf.ta, (int)cf.ts, cf.tg};
            c2.wide  = {cf.wa, (int)cf.ws, cf.wg};
            c2.reclip_pct = cf.rc; c2.cap = cf.cap; c2.cost_gate_bp = cf.cg;
            c2.tf_secs = TF_SECS; c2.round_trip_bp = 20.0;
            UpJumpLadderCompanion eng(c2);
            int seq = 0;
            eng.set_on_clip([&](const UpJumpLadderCompanion::ClipRecord& r) {
                char buf[64]; std::snprintf(buf, sizeof(buf), "%.4f", r.net_bp);
                out << coin << "," << ti << "," << seq++ << "," << buf << "\n";
            });
            for (int i = ei; i < xi; ++i)
                eng.observe(true, epx, c[i], (int64_t)i * TF_SECS * 1000);
            int last = (xi - 1 >= ei) ? (xi - 1) : ei;
            double last_px = (xi - 1 >= ei) ? c[xi - 1] : epx;
            eng.observe(false, epx, last_px, (int64_t)last * TF_SECS * 1000);  // parent-exit flush (MTM)
            ++ti;
        }
    }
    out.close();
    std::fprintf(stderr, "wrote cpp_clips.csv\n");
    return 0;
}
