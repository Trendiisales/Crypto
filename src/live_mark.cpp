// live_mark.cpp (T12) — C++ port of live_mark.py. Intraday LIVE re-mark of the OPEN
// crypto legs against Binance spot, so the companion's giveback/clip trips on real
// intraday give-back instead of the daily-close mark. Crypto only (24/7); NDX/weekend
// legs keep their existing mark. NON-destructive: only touches px/unreal of mapped open
// legs + the roll-up totals. Runs on cron every 5 min (one instance per book via
// IBKRCRYPTO_DATADIR). The NDX live-mark (SSH->aurora_NQ) lives in shadow_refresh.cpp,
// not here — live_mark.py never touched NDX.
//
// The SSH->aurora_NQ NDX mark (plan T12) was already ported in Phase 1's
// shadow_refresh.cpp (read_live_nq + 0.991 basis + sanity band + daily-close fallback),
// so this file is a faithful port of live_mark.py alone.
#include "crypto/BinanceFetch.hpp"
#include "crypto/Roster.hpp"     // data_dir()
#include "crypto/StateJson.hpp"  // round_n
#include <string>
#include <vector>
#include <fstream>
#include <cstdio>
#include <cmath>
#include <ctime>

using namespace crypto;

// slot.key prefix -> Binance spot symbol (insertion order mirrors the Python dict).
static const std::vector<std::pair<std::string, std::string>> BINANCE{
    {"eth", "ETHUSDT"}, {"btc", "BTCUSDT"}, {"sol", "SOLUSDT"}};

// float(field or 0): number -> value, numeric string -> parsed, null/missing/0 -> def.
static double jnum(const json& s, const char* k, double def = 0.0) {
    if (!s.contains(k) || s[k].is_null()) return def;
    const json& v = s[k];
    if (v.is_number())  return v.get<double>();
    if (v.is_string())  { try { return std::stod(v.get<std::string>()); } catch (...) { return def; } }
    if (v.is_boolean()) return v.get<bool>() ? 1.0 : 0.0;
    return def;
}

static double live_price(const std::string& binsym) {
    try {
        json r = get_json("https://api.binance.com/api/v3/ticker/price?symbol=" + binsym, 8);
        return std::stod(r["price"].get<std::string>());
    } catch (const std::exception& e) {
        std::printf("live_mark: %s fetch failed: %s\n", binsym.c_str(), e.what());
        return std::nan("");
    }
}

int main() {
    curl_global_init(CURL_GLOBAL_DEFAULT);
    const std::string STATE = data_dir() + "/state.json";

    std::ifstream sf(STATE);
    if (!sf) { std::printf("live_mark: no state.json\n"); curl_global_cleanup(); return 0; }
    json d;
    try { d = json::parse(sf); } catch (...) { std::printf("live_mark: no state.json\n"); curl_global_cleanup(); return 0; }
    sf.close();

    char tsb[32];
    { time_t t = std::time(nullptr); struct tm g; gmtime_r(&t, &g);
      std::strftime(tsb, sizeof tsb, "%Y-%m-%d %H:%M UTC", &g); }
    const std::string ts = tsb;

    int n_marked = 0;
    double tot_unreal_usd = 0.0, tot_unreal_pct = 0.0;

    if (d.contains("slots") && d["slots"].is_array()) {
        for (auto& s : d["slots"]) {
            if (jnum(s, "pos", 0.0) == 0.0) continue;   // open legs only (pos truthy)
            std::string key = s.value("key", std::string());
            for (auto& c : key) c = (char)std::tolower((unsigned char)c);
            std::string binsym;
            for (auto& [pre, bs] : BINANCE) if (key.rfind(pre, 0) == 0) { binsym = bs; break; }
            double entry = jnum(s, "entry_px", 0.0);
            if (!binsym.empty() && entry > 0) {
                double lp = live_price(binsym);
                if (std::isfinite(lp)) {
                    double old_pct = jnum(s, "unreal_pct", 0.0);
                    double pos = jnum(s, "pos", 0.0);
                    int side = pos > 0 ? 1 : -1;
                    double new_pct = side * (lp - entry) / entry * 100.0;
                    double mult = jnum(s, "mult", 0.0);
                    double contracts = jnum(s, "contracts", 0.0);
                    double new_usd = round_n(side * (lp - entry) * mult * contracts, 2);
                    s["px"] = lp;
                    s["unreal_pct"] = round_n(new_pct, 3);
                    s["unreal_usd"] = new_usd;
                    s["mark_src"] = "binance-live";
                    s["mark_ts"]  = ts;
                    n_marked++;
                    std::printf("live_mark: %s %s entry=%g live=%g -> unreal %+.2f%% (was %+.2f%%)\n",
                                s.value("sym", std::string()).c_str(), side > 0 ? "LONG" : "SHORT",
                                entry, lp, new_pct, old_pct);
                }
            }
            tot_unreal_usd += jnum(s, "unreal_usd", 0.0);   // all open legs (marked or not)
            tot_unreal_pct += jnum(s, "unreal_pct", 0.0);
        }
    }

    d["open_unreal_usd"] = round_n(tot_unreal_usd, 2);
    d["open_unreal_pct"] = round_n(tot_unreal_pct, 2);
    d["total_usd"] = round_n(tot_unreal_usd + jnum(d, "realized_usd", 0.0), 2);
    d["live_mark_ts"] = ts;

    // atomic write (temp + rename) so a concurrent shadow_refresh never reads a partial file
    std::string tmp = STATE + ".tmp";
    { std::ofstream of(tmp); of << d.dump(1); }
    std::rename(tmp.c_str(), STATE.c_str());

    std::printf("live_mark: re-marked %d crypto leg(s) live @ %s; open_unreal=$%.2f\n",
                n_marked, ts.c_str(), round_n(tot_unreal_usd, 2));
    curl_global_cleanup();
    return 0;
}
