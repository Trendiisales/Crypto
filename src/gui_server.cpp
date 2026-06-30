// gui_server.cpp (T13) — C++ port of gui/server.py. Crypto-book dashboard on :8090,
// SEPARATE from the Omega GUI (same IBKR gateway, two ports — see gui/GUI_SPLIT.md).
// Serves the six /api routes the dashboard polls + the static index.html, reproducing:
//   - server-side freshness guard (_fresh) injected into /api/state + /api/state_intraday
//     so a dead producer badges RED instead of letting browser-side WS marks fake liveness;
//   - real-time NDX from OUR IBKR feed (background thread tails the last NQ trade over a
//     multiplexed ssh to omega-vps, scales NQ-future -> ^NDX by the 0.991 carry basis).
// Faithful port of server.py — same env knobs, same fallbacks, same JSON shapes.
#include "json.hpp"
#include "httplib.h"
#include "crypto/Roster.hpp"   // env_or, data_dir
#include <string>
#include <vector>
#include <fstream>
#include <sstream>
#include <mutex>
#include <thread>
#include <atomic>
#include <chrono>
#include <ctime>
#include <cstdio>
#include <cstdlib>
#include <cmath>
#include <sys/stat.h>

using nlohmann::json;
using crypto::env_or;

// ---- paths (mirror server.py: env override > local gui copy > backtest output) ----
static std::string gui_dir()  { return env_or("IBKRCRYPTO_GUIDIR", "/Users/jo/Crypto/gui"); }
static std::string home_dir() { return env_or("HOME", "/Users/jo"); }
static bool exists(const std::string& p) { struct stat st; return ::stat(p.c_str(), &st) == 0; }
static std::string dirname_of(const std::string& p) {
    size_t sl = p.find_last_of('/');
    return (sl == std::string::npos) ? "." : p.substr(0, sl);
}
// KILL_FLAT marker = book held flat (see shadow_refresh.cpp / refresh_shadow_intraday.py /
// refresh_luke_shadow.py). It lives in the book's state-dir; producers honor it durably.
static bool killed_for(const std::string& statePath) {
    return exists(dirname_of(statePath) + "/KILL_FLAT");
}
static std::string first_existing(const std::vector<std::string>& cands, const std::string& fallback) {
    for (auto& p : cands) if (exists(p)) return p;
    return fallback;
}
static std::string STATE, COMP, STATE_INTRA, COMP_INTRA, STATE_LUKE;
static void resolve_paths() {
    const std::string H = gui_dir();
    STATE = env_or("IBKRCRYPTO_STATE",
        first_existing({H + "/state.json", H + "/../backtest/data/ibkrcrypto/state.json"},
                       H + "/state.json"));
    COMP = env_or("COMPANION_STATE", home_dir() + "/stall-accountant/companion_state.json");
    STATE_INTRA = env_or("IBKRCRYPTO_STATE_INTRADAY", H + "/../backtest/data/ibkrcrypto_intraday/state.json");
    COMP_INTRA  = env_or("COMPANION_STATE_INTRADAY", home_dir() + "/IBKRCrypto/companion_intraday/companion_state.json");
    STATE_LUKE  = env_or("IBKRCRYPTO_STATE_LUKE",
        first_existing({H + "/state_luke.json", H + "/../backtest/data/ibkrcrypto_luke/state.json"},
                       H + "/state_luke.json"));
}

static std::string read_file(const std::string& p) {
    std::ifstream f(p, std::ios::binary);
    std::ostringstream ss; ss << f.rdbuf();
    return ss.str();
}

// ---- freshness guard (server.py _freshness, 1:1) ----
static const long MARK_MAX_S = 900, MTIME_MAX_S = 1200;
static long sig_cap(const std::string& kind) { return kind == "intraday" ? 100 * 60 : 30 * 3600; }

// parse "YYYY-MM-DD HH:MM[:SS]" (UTC) -> epoch, or -1. server.py strips " UTC"/Z/T.
static long parse_utc(const std::string& in) {
    if (in.empty()) return -1;
    std::string s = in;
    auto rm = [&](const std::string& a, const std::string& b) {
        size_t pos;
        while ((pos = s.find(a)) != std::string::npos) s.replace(pos, a.size(), b);
    };
    rm(" UTC", ""); rm("Z", ""); rm("T", " ");
    // trim
    while (!s.empty() && (s.front() == ' ')) s.erase(s.begin());
    while (!s.empty() && (s.back() == ' ' || s.back() == '\n' || s.back() == '\r')) s.pop_back();
    for (const char* fmt : {"%Y-%m-%d %H:%M:%S", "%Y-%m-%d %H:%M"}) {
        struct tm tm{};
        if (strptime(s.c_str(), fmt, &tm)) return (long)timegm(&tm);
    }
    return -1;
}

static std::string jstr(const json& d, const char* k) {
    if (d.contains(k) && d[k].is_string()) return d[k].get<std::string>();
    return "";
}

static json freshness(const json& d, const std::string& path, const std::string& kind) {
    long now = (long)std::time(nullptr);
    std::vector<std::string> reasons;
    json ages = json::object();
    char buf[128];
    // 1) producer alive? (file mtime)
    struct stat st;
    if (::stat(path.c_str(), &st) == 0) {
        long age = now - (long)st.st_mtime;
        ages["file_age_s"] = (int)age;
        if (age > MTIME_MAX_S) {
            std::snprintf(buf, sizeof buf, "PRODUCER DEAD: file untouched %ldmin", age / 60);
            reasons.push_back(buf);
        }
    } else {
        reasons.push_back("PRODUCER DEAD: state file missing");
    }
    // 2) marks fresh? heartbeat = freshest of (live_mark_ts, updated)
    long mt2 = parse_utc(jstr(d, "live_mark_ts"));
    long su  = parse_utc(jstr(d, "updated"));
    if (mt2 >= 0) ages["mark_age_s"] = (int)(now - mt2);
    if (su  >= 0) ages["sig_age_s"]  = (int)(now - su);
    std::vector<long> hb;
    if (mt2 >= 0) hb.push_back(mt2);
    if (su  >= 0) hb.push_back(su);
    if (hb.empty()) {
        reasons.push_back("MARKS: no timestamps in state");
    } else {
        long h = hb[0];
        for (long x : hb) if (x > h) h = x;
        ages["heartbeat_age_s"] = (int)(now - h);
        if (now - h > MARK_MAX_S) {
            std::snprintf(buf, sizeof buf, "MARKS FROZEN %ldmin (live P&L not real)", (now - h) / 60);
            reasons.push_back(buf);
        }
    }
    // 3) signals fresh? (cadence-aware)
    long cap = sig_cap(kind);
    if (su >= 0 && now - su > cap) {
        std::snprintf(buf, sizeof buf, "SIGNALS STALE %ldh (>%ldh cap)", (now - su) / 3600, cap / 3600);
        reasons.push_back(buf);
    }
    // 4) upstream bar feeds
    if (d.contains("data_health") && d["data_health"].is_object()) {
        const json& dh = d["data_health"];
        if (dh.contains("all_fresh") && dh["all_fresh"].is_boolean() && dh["all_fresh"].get<bool>() == false) {
            std::string ss;
            if (dh.contains("stale_sources") && dh["stale_sources"].is_array()) {
                bool first = true;
                for (auto& s : dh["stale_sources"]) {
                    if (!s.is_string()) continue;
                    if (!first) ss += ",";
                    ss += s.get<std::string>(); first = false;
                }
            }
            reasons.push_back("DATA STALE: " + ss);
        }
    }
    // 5) staleness_alarm.py RED marker
    {
        std::string dir = path; size_t sl = dir.find_last_of('/');
        dir = (sl == std::string::npos) ? "." : dir.substr(0, sl);
        std::string mk = dir + "/ALARM_STALE.txt";
        if (exists(mk)) {
            std::string c = read_file(mk);
            size_t nl = c.find('\n'); if (nl != std::string::npos) c = c.substr(0, nl);
            // strip trailing whitespace
            while (!c.empty() && (c.back() == ' ' || c.back() == '\r')) c.pop_back();
            reasons.push_back("ALARM: " + c);
        }
    }
    json out = json::object();
    out["stale"] = !reasons.empty();
    out["reasons"] = reasons;
    out["ages"] = ages;
    out["checked_s"] = (int)now;
    return out;
}

static std::string inject_fresh(const std::string& body, const std::string& path, const std::string& kind) {
    try {
        json d = json::parse(body);
        d["_fresh"] = freshness(d, path, kind);
        d["killed"] = killed_for(path);  // instant badge: marker present even before producer rewrites state
        return d.dump();
    } catch (...) {
        return body;
    }
}

// ---- real-time NDX from our IBKR NQ trade feed (server.py _nq_poll_loop) ----
static const double NDX_MIN = 5000.0, NDX_MAX = 100000.0, NDX_NQ_BASIS = 0.991;
static std::mutex g_nq_mu;
static double g_nq_px = 0.0, g_nq_raw = 0.0, g_nq_ts = 0.0;

// base64 of the UTF-16-LE PowerShell command -> -EncodedCommand (no quotes -> survives cmd.exe).
static std::string b64_utf16le(const std::string& ascii) {
    static const char* T = "ABCDEFGHIJKLMNOPQRSTUVWXYZabcdefghijklmnopqrstuvwxyz0123456789+/";
    std::string bytes;
    for (char c : ascii) { bytes.push_back(c); bytes.push_back('\0'); }
    std::string out;
    for (size_t i = 0; i < bytes.size(); i += 3) {
        unsigned v = (unsigned char)bytes[i] << 16;
        int n = 1;
        if (i + 1 < bytes.size()) { v |= (unsigned char)bytes[i + 1] << 8; n = 2; }
        if (i + 2 < bytes.size()) { v |= (unsigned char)bytes[i + 2]; n = 3; }
        out.push_back(T[(v >> 18) & 0x3F]);
        out.push_back(T[(v >> 12) & 0x3F]);
        out.push_back(n >= 2 ? T[(v >> 6) & 0x3F] : '=');
        out.push_back(n >= 3 ? T[v & 0x3F] : '=');
    }
    return out;
}

static double now_s() {
    return std::chrono::duration<double>(std::chrono::system_clock::now().time_since_epoch()).count();
}

static void nq_poll_loop() {
    const std::string ps =
        "$f=Get-ChildItem C:\\Omega\\logs\\ibkr_l2\\ibkr_trades_NQ_*.csv|"
        "Sort-Object LastWriteTime|Select-Object -Last 1; Get-Content $f.FullName -Tail 1";
    const std::string b64 = b64_utf16le(ps);
    const std::string cmd =
        "ssh -o ConnectTimeout=5 -o BatchMode=yes -o ServerAliveInterval=15 "
        "-o ControlMaster=auto -o ControlPath=/tmp/ssh-omega-nq-%r@%h:%p -o ControlPersist=120 "
        "omega-vps \"powershell -NoProfile -EncodedCommand " + b64 + "\" 2>/dev/null";
    while (true) {
        std::string out;
        if (FILE* p = popen(cmd.c_str(), "r")) {
            char b[512]; while (fgets(b, sizeof b, p)) out += b;
            pclose(p);
        }
        // parse last CSV line: field[1] = price
        // trim
        while (!out.empty() && (out.back() == '\n' || out.back() == '\r' || out.back() == ' ')) out.pop_back();
        std::vector<std::string> parts; std::string cur; std::stringstream ssr(out);
        while (std::getline(ssr, cur, ',')) parts.push_back(cur);
        if (parts.size() >= 2) {
            try {
                double raw = std::stod(parts[1]);
                double px = raw * NDX_NQ_BASIS;
                if (px >= NDX_MIN && px <= NDX_MAX) {
                    std::lock_guard<std::mutex> lk(g_nq_mu);
                    g_nq_px = std::round(px * 100.0) / 100.0; g_nq_raw = raw; g_nq_ts = now_s();
                }
            } catch (...) {}
        }
        std::this_thread::sleep_for(std::chrono::seconds(2));
    }
}

static json ndx_quote() {
    std::lock_guard<std::mutex> lk(g_nq_mu);
    double now = now_s();
    json q = json::object();
    bool have = g_nq_ts > 0;
    double age = have ? std::round((now - g_nq_ts) * 10.0) / 10.0 : 0.0;
    bool stale = !have || age > 15;
    q["px"] = g_nq_px;
    q["src"] = "IBKR-NQ-rt";
    q["state"] = "";
    q["stale"] = stale;
    if (have) q["age_s"] = age; else q["age_s"] = nullptr;
    return q;
}

// ---- helpers to serve a JSON file with a fallback object ----
static void serve_json(httplib::Response& res, const std::string& body) {
    res.set_header("Cache-Control", "no-store");
    res.set_content(body, "application/json");
}
static void serve_file_or(httplib::Response& res, const std::string& path, const std::string& fallback) {
    std::ifstream f(path, std::ios::binary);
    if (f) { std::ostringstream ss; ss << f.rdbuf(); serve_json(res, ss.str()); }
    else serve_json(res, fallback);
}
static void serve_injected_or(httplib::Response& res, const std::string& path,
                              const std::string& kind, const std::string& fallback) {
    std::ifstream f(path, std::ios::binary);
    if (f) { std::ostringstream ss; ss << f.rdbuf(); serve_json(res, inject_fresh(ss.str(), path, kind)); }
    else serve_json(res, fallback);
}

int main() {
    resolve_paths();
    int port = std::atoi(env_or("PORT", "8090").c_str());
    std::string bind = env_or("IBKRCRYPTO_BIND", "0.0.0.0");

    if (env_or("CRYPTO_SKIP_LIVE_NDX", "") != "1")
        std::thread(nq_poll_loop).detach();

    httplib::Server svr;

    svr.Get("/api/state_intraday", [](const httplib::Request&, httplib::Response& res) {
        serve_injected_or(res, STATE_INTRA, "intraday",
            R"({"engine":"IBKRCrypto-Intraday","mode":"SHADOW","slots":[],"closed":[],"_fresh":{"stale":true,"reasons":["state unreachable"]}})");
    });
    svr.Get("/api/state_luke", [](const httplib::Request&, httplib::Response& res) {
        std::ifstream f(STATE_LUKE, std::ios::binary);
        if (!f) {
            serve_json(res, R"({"book":"LukeCrypto","mode":"SHADOW","equity":0,"n_open":0,"positions":[],"closed_today":0,"last":"unreachable"})");
            return;
        }
        std::ostringstream ss; ss << f.rdbuf();
        try {
            json d = json::parse(ss.str());
            d["killed"] = killed_for(STATE_LUKE);
            serve_json(res, d.dump());
        } catch (...) { serve_json(res, ss.str()); }
    });
    svr.Get("/api/companion_intraday", [](const httplib::Request&, httplib::Response& res) {
        serve_file_or(res, COMP_INTRA, R"({"open_companions":0,"open_detail":[],"realized_total":0})");
    });
    svr.Get("/api/state", [](const httplib::Request&, httplib::Response& res) {
        serve_injected_or(res, STATE, "daily",
            R"({"engine":"IBKRCrypto","mode":"SHADOW","slots":[],"day_pnl":0,"_fresh":{"stale":true,"reasons":["state unreachable"]}})");
    });
    svr.Get("/api/ndx", [](const httplib::Request&, httplib::Response& res) {
        serve_json(res, ndx_quote().dump());
    });
    svr.Get("/api/companion", [](const httplib::Request&, httplib::Response& res) {
        serve_file_or(res, COMP,
            R"({"open_companions":0,"positions":[],"gate_pct":2.0,"stall_bars":3,"reversal_giveback":0.4,"realized_total":0})");
    });

    // ---- KILL button: per-book panic-flatten (paper/shadow only) ----
    // POST /api/flatten?book=daily|intraday|luke. Writes the durable KILL_FLAT marker into the
    // book's state-dir, then triggers an immediate producer run so the flatten BOOKS NOW (each
    // open leg closed at last mark) using the producer's own PnL math -- no duplicated accounting
    // here. The marker persists, so the book stays flat every run until the operator deletes it.
    // Real money is NOT reachable: these producers write shadow ledgers only.
    svr.Post("/api/flatten", [](const httplib::Request& req, httplib::Response& res) {
        std::string book = req.has_param("book") ? req.get_param_value("book") : "daily";
        std::string statePath, prod;
        const std::string BT = "/Users/jo/IBKRCrypto/backtest";
        if (book == "intraday") {
            statePath = STATE_INTRA;
            prod = "cd " + BT + " && /usr/bin/python3 refresh_shadow_intraday.py >/dev/null 2>&1";
        } else if (book == "luke") {
            statePath = STATE_LUKE;
            prod = "cd " + BT + " && /usr/bin/python3 refresh_luke_shadow.py >/dev/null 2>&1";
        } else if (book == "daily") {
            statePath = STATE;
            prod = "cd " + BT + " && /Users/jo/Crypto/build/shadow_refresh >/dev/null 2>&1";
        } else {
            res.status = 400;
            serve_json(res, R"({"ok":false,"error":"unknown book"})");
            return;
        }
        std::string marker = dirname_of(statePath) + "/KILL_FLAT";
        std::ofstream mf(marker, std::ios::trunc);
        if (!mf) {
            res.status = 500;
            serve_json(res, R"({"ok":false,"error":"could not write KILL_FLAT marker"})");
            return;
        }
        mf << "KILL_FLAT " << (long)std::time(nullptr) << " via gui kill button\n";
        mf.close();
        int rc = std::system(prod.c_str());
        json out = {{"ok", true}, {"book", book}, {"killed", true},
                    {"marker", marker}, {"producer_rc", rc}};
        serve_json(res, out.dump());
    });

    // no-store on every response (incl. the static mount) so the dashboard never serves a stale cached page
    svr.set_post_routing_handler([](const httplib::Request&, httplib::Response& res) {
        res.set_header("Cache-Control", "no-store");
    });
    // static dashboard (gui/index.html). mount serves /index.html; "/" redirects to it.
    svr.set_mount_point("/", gui_dir());
    svr.Get("/", [](const httplib::Request&, httplib::Response& res) {
        std::ifstream f(gui_dir() + "/index.html", std::ios::binary);
        std::ostringstream ss; ss << f.rdbuf();
        res.set_content(ss.str(), "text/html");
    });

    std::printf("[crypto-gui] http://%s:%d  (state: %s)\n", bind.c_str(), port, STATE.c_str());
    svr.listen(bind, port);
    return 0;
}
