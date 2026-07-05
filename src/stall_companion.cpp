// stall_companion — native C++ port of the 3 crypto-intraday stall_accountant.py books.
//
// REPLACES the last 3 live python cron lines (SKIP_OMEGA=1 stall_accountant.py):
//   intraday_sol_emax_4h · intraday_eth_donch_4h · intraday_sol_emax_1h
// (operator hard rule: NO python in the working system — feedback-no-python-working-system).
// Mirrors the Omega StallCompanion cutover + the wave_companion idiom: ONE cron-invoked exe,
// pure replay of the current parent state, writes each book's ledger + the crypto-desk aggregate.
//
// SEPARATE INDEPENDENT ENGINE (feedback-companion-independent-engine): observe-only paper shadow.
// Each StallBook mirrors a matched parent leg's live MFE and BANKS an early exit into its OWN book;
// it NEVER opens / moves / shrinks / closes a real position and is NEVER read by the parent. Judge
// STANDALONE / additive — never vs riding WIDE.
//
// FAITHFUL to stall_accountant.py SKIP_OMEGA path (spec verified against the live state.json):
//   * input  = CRYPTO_STATE slots[] (skip pos==0); row = {book=CRYPTO, eng=strat, sym, side=pos>0?
//              LONG:SHORT, entry=entry_px, current=px, upnl=unreal_usd}. closed[] ignored.
//   * gates  = %-gauge, arm mfe_pct>=GATE, REVERSAL clip at fav<=peak*(1-0.50), LOSS_CUT at
//              upnl<=-15, STALL_BARS=9999 (inert), RETRIG 5%. cold-loss book==CRYPTO -> -15.
//   * empty-omega guard DISABLED (skip_empty_guard=true): harvest+ENGINE_EXIT always run.
//   * companion_closed.csv byte-compatible (ledger continuity); companion_positions/clipped.json
//     seed-migrated to the C++ .tsv on first run so open+clipped continuity survives the cutover.
//
// Build:  crypto_exe(stall_companion src/stall_companion.cpp Threads::Threads)  [json.hpp + stdlib]
#include <cstdio>
#include <cstdlib>
#include <ctime>
#include <string>
#include <vector>
#include <fstream>
#include <sstream>
#include <map>
#include "json.hpp"
#include "crypto/Roster.hpp"          // env_or
#include "crypto/StallCompanion.hpp"  // omega::StallBook + StallLiveRow (vendored from Omega, pure stdlib)

using json = nlohmann::json;
using namespace crypto;              // env_or
using omega::StallBook;
using omega::StallLiveRow;

static std::string home_() { const char* h = std::getenv("HOME"); return h ? h : "."; }
static bool file_exists_(const std::string& p) { std::ifstream f(p); return f.good(); }

// ── one-time seed migration: python companion_positions.json / companion_clipped.json -> the
//    C++ .tsv StallBook::load_pos_ reads. Runs only when the .tsv is absent (first C++ run in a
//    dir); preserves open-companion + suppressed-reopen continuity across the cutover. ──
static void migrate_seed_(const std::string& dir) {
    const std::string pos_tsv = dir + "/companion_positions.tsv";
    const std::string pos_json = dir + "/companion_positions.json";
    if (!file_exists_(pos_tsv) && file_exists_(pos_json)) {
        try {
            std::ifstream f(pos_json); json d = json::parse(f);
            std::ofstream o(pos_tsv, std::ios::trunc);
            for (auto it = d.begin(); it != d.end(); ++it) {
                const json& p = it.value();
                auto S = [&](const char* k) { return p.contains(k) && p[k].is_string() ? p[k].get<std::string>() : std::string(); };
                auto D = [&](const char* k) { return p.contains(k) && p[k].is_number() ? p[k].get<double>() : 0.0; };
                auto L = [&](const char* k) { return p.contains(k) && p[k].is_number() ? (long long)p[k].get<double>() : 0LL; };
                // field order per StallBook::load_pos_: key book eng sym side entry open_bar ext_bar mfe_pct mfe_usd stall last_upnl
                o << it.key() << '\t' << S("book") << '\t' << S("eng") << '\t' << S("sym") << '\t' << S("side")
                  << '\t' << D("entry") << '\t' << L("open_bar") << '\t' << L("ext_bar") << '\t' << D("mfe_pct")
                  << '\t' << D("mfe_usd") << '\t' << (int)D("stall") << '\t' << D("last_upnl") << '\n';
            }
            std::fprintf(stderr, "[stall-companion] seed-migrated %s -> companion_positions.tsv\n", pos_json.c_str());
        } catch (...) { std::fprintf(stderr, "[stall-companion] WARN seed migrate failed: %s\n", pos_json.c_str()); }
    }
    const std::string clp_tsv = dir + "/companion_clipped.tsv";
    const std::string clp_json = dir + "/companion_clipped.json";
    if (!file_exists_(clp_tsv) && file_exists_(clp_json)) {
        try {
            std::ifstream f(clp_json); json d = json::parse(f);
            std::ofstream o(clp_tsv, std::ios::trunc);
            if (d.is_object())
                for (auto it = d.begin(); it != d.end(); ++it) {
                    double peak = it.value().is_number() ? it.value().get<double>() : 0.0;
                    o << it.key() << '\t' << peak << '\n';
                }
            else if (d.is_array())   // legacy list form -> peak 0.0
                for (auto& k : d) if (k.is_string()) o << k.get<std::string>() << "\t0\n";
        } catch (...) { std::fprintf(stderr, "[stall-companion] WARN clipped migrate failed: %s\n", clp_json.c_str()); }
    }
}

// ── read CRYPTO_STATE slots[] -> live rows (faithful to poll_crypto: skip pos==0) ──
static std::vector<StallLiveRow> poll_crypto_(const std::string& state_path) {
    std::vector<StallLiveRow> rows;
    std::ifstream f(state_path);
    if (!f.good()) { std::fprintf(stderr, "[stall-companion] CRYPTO_STATE unreadable: %s\n", state_path.c_str()); return rows; }
    json d;
    try { d = json::parse(f); } catch (...) { std::fprintf(stderr, "[stall-companion] CRYPTO_STATE parse fail\n"); return rows; }
    if (!d.contains("slots") || !d["slots"].is_array()) return rows;
    for (const auto& s : d["slots"]) {
        try {
            double pos = s.contains("pos") && s["pos"].is_number() ? s["pos"].get<double>() : 0.0;
            if (pos == 0.0) continue;                                  // flat leg — ignored
            StallLiveRow r;
            r.book = "CRYPTO";
            r.eng  = s.value("strat", std::string("?"));
            r.sym  = s.value("sym",   std::string("?"));
            r.side = pos > 0 ? "LONG" : "SHORT";
            auto num = [&](const char* k) { return s.contains(k) && s[k].is_number() ? s[k].get<double>() : 0.0; };
            r.entry   = num("entry_px");
            r.current = num("px");
            r.upnl    = num("unreal_usd");
            rows.push_back(std::move(r));
        } catch (...) { /* skip bad slot (faithful try/except) */ }
    }
    return rows;
}

// ── merge the 3 books' roll-ups -> crypto-desk aggregate (by_book.CRYPTO), matching the schema
//    /api/companion_intraday serves verbatim (companion_intraday/companion_state.json). ──
static void write_aggregate_(const std::vector<StallBook*>& books, const std::string& out_path, int64_t now) {
    using omega::StallRollUp;
    double rt = 0, rtd = 0, r7 = 0, r30 = 0; int open_total = 0;
    std::map<std::string, double> by_reason;
    std::map<std::string, json> per_engine;   // eng -> {open,closed,realized}
    json open_detail = json::array();
    for (const StallBook* b : books) {
        const StallRollUp& R = b->rollup();
        rt += R.realized_total; rtd += R.realized_today; r7 += R.realized_7d; r30 += R.realized_30d;
        open_total += R.open_companions;
        for (const auto& kv : R.by_reason) by_reason[kv.first] += kv.second;
        for (const auto& kv : R.per_engine) {
            json& e = per_engine[kv.first];
            if (e.is_null()) e = json{{"open",0},{"closed",0},{"realized",0.0}};
            e["open"]     = e["open"].get<int>()      + kv.second.open;
            e["closed"]   = e["closed"].get<int>()    + kv.second.closed;
            e["realized"] = e["realized"].get<double>() + kv.second.realized;
        }
        for (const auto& d : R.open_detail)
            open_detail.push_back({{"book",d.book},{"eng",d.eng},{"sym",d.sym},{"side",d.side},
                {"entry",d.entry},{"mfe_pct",d.mfe_pct},{"mfe_usd",d.mfe_usd},{"stall",d.stall},
                {"upnl",d.upnl},{"eligible",d.eligible}});
    }
    auto r2 = [](double v) { return std::floor(v * 100.0 + 0.5) / 100.0; };
    for (auto& kv : per_engine) kv.second["realized"] = r2(kv.second["realized"].get<double>());
    json br = json::object(); for (auto& kv : by_reason) br[kv.first] = r2(kv.second);
    json pe = json::object(); for (auto& kv : per_engine) pe[kv.first] = kv.second;

    char ub[40]; { std::time_t t = (std::time_t)now; std::tm tmv{}; gmtime_r(&t, &tmv);
        std::snprintf(ub, sizeof(ub), "%04d-%02d-%02d %02d:%02d UTC",
            tmv.tm_year+1900, tmv.tm_mon+1, tmv.tm_mday, tmv.tm_hour, tmv.tm_min); }

    json out = {
        {"updated", ub}, {"source", "stall_companion_cpp"},
        {"stall_bars", 9999}, {"gate_pct", 3.0}, {"reversal_giveback", 0.50},
        {"open_companions", open_total},
        {"realized_total", r2(rt)}, {"realized_today", r2(rtd)}, {"realized_7d", r2(r7)}, {"realized_30d", r2(r30)},
        {"by_reason", br},
        {"by_book", {{"CRYPTO", {{"realized", r2(rt)}, {"realized_today", r2(rtd)},
                                 {"realized_7d", r2(r7)}, {"realized_30d", r2(r30)}, {"by_reason", br}}}}},
        {"per_engine", pe},
        {"open_detail", open_detail}
    };
    const std::string tmp = out_path + ".tmp";
    { std::ofstream sf(tmp, std::ios::trunc); if (!sf.good()) return; sf << out.dump(1); }
    std::remove(out_path.c_str());
    std::rename(tmp.c_str(), out_path.c_str());
}

int main() {
    const std::string SA   = env_or("STALL_ACCT_DIR", home_() + "/stall-accountant");
    const std::string CS   = env_or("CRYPTO_STATE",   home_() + "/Crypto/backtest/data/ibkrcrypto_intraday/state.json");
    const std::string AGG  = env_or("COMPANION_STATE_INTRADAY", home_() + "/Crypto/companion_intraday/companion_state.json");
    const int64_t now = (int64_t)std::time(nullptr);

    // ── the 3 live crypto-intraday books (exact env values from the retired cron lines) ──
    std::vector<StallBook::Config> cfgs;
    auto mk = [&](const std::string& name, std::vector<std::string> inc, double gate, int tf_h) {
        StallBook::Config c;
        c.name = name; c.include = std::move(inc); c.exclude = {};
        c.gate_pct = gate; c.stall_bars = 9999; c.tf_sec = tf_h * 3600;
        c.rev_gb = 0.50; c.rev_gb_pts = 0.0; c.retrig_pct = 0.05;
        c.cold_loss_crypto = -15.0; c.bull_only = false; c.skip_empty_guard = true;
        c.dir = SA + "/" + name;
        return c;
    };
    cfgs.push_back(mk("intraday_sol_emax_4h",  {"emax 4h sol"},       3.0, 4));
    cfgs.push_back(mk("intraday_eth_donch_4h", {"donch40 4h eth met"}, 1.0, 4));
    cfgs.push_back(mk("intraday_sol_emax_1h",  {"emax 1h sol"},       3.0, 1));

    for (const auto& c : cfgs) migrate_seed_(c.dir);   // JSON->tsv once per dir before construction

    const auto rows = poll_crypto_(CS);

    std::vector<StallBook> books;
    books.reserve(cfgs.size());
    for (auto& c : cfgs) books.emplace_back(c);
    std::vector<StallBook*> bptr;
    for (auto& b : books) { b.step(rows, /*gold_bull*/ -1, now); bptr.push_back(&b); }

    write_aggregate_(bptr, AGG, now);

    int open_tot = 0; double real_tot = 0;
    for (const auto& b : books) { open_tot += b.rollup().open_companions; real_tot += b.rollup().realized_total; }
    std::printf("stall-companion: %zu books, %zu live rows, %d open companions, realized $%.2f\n",
                books.size(), rows.size(), open_tot, real_tot);
    return 0;
}
