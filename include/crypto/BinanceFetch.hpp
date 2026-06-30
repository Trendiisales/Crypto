#pragma once
// HTTP+JSON data layer for the C++ fetch binary (T10). libcurl GET + nlohmann/json.
// Thin on purpose: this header only does transport (GET -> parsed json). The per-source
// pagination / append / format logic lives in src/fetch.cpp so each function mirrors its
// Python counterpart (fetch_crypto.py, fetch_crypto_intraday.py, fetch_funding.py,
// fetch_ndx.py) line-for-line for the parity gate (Checkpoint C).
#include <string>
#include <stdexcept>
#include <curl/curl.h>
#include "json.hpp"

namespace crypto {

using json = nlohmann::json;

inline size_t curl_sink_(char* p, size_t sz, size_t n, void* ud) {
    static_cast<std::string*>(ud)->append(p, sz * n);
    return sz * n;
}

// One-shot GET. Returns true on HTTP 2xx; fills `body`. `err` carries the failure reason.
// A non-default User-Agent is set because Yahoo's chart endpoint 429s the libcurl default.
inline bool http_get(const std::string& url, std::string& body, std::string& err,
                     long timeout_s = 30) {
    CURL* h = curl_easy_init();
    if (!h) { err = "curl_easy_init failed"; return false; }
    body.clear();
    char ebuf[CURL_ERROR_SIZE] = {0};
    curl_easy_setopt(h, CURLOPT_URL, url.c_str());
    curl_easy_setopt(h, CURLOPT_WRITEFUNCTION, curl_sink_);
    curl_easy_setopt(h, CURLOPT_WRITEDATA, &body);
    curl_easy_setopt(h, CURLOPT_TIMEOUT, timeout_s);
    curl_easy_setopt(h, CURLOPT_FOLLOWLOCATION, 1L);
    curl_easy_setopt(h, CURLOPT_USERAGENT, "Mozilla/5.0 (crypto-fetch)");
    curl_easy_setopt(h, CURLOPT_ERRORBUFFER, ebuf);
    CURLcode rc = curl_easy_perform(h);
    long code = 0;
    curl_easy_getinfo(h, CURLINFO_RESPONSE_CODE, &code);
    curl_easy_cleanup(h);
    if (rc != CURLE_OK) { err = ebuf[0] ? ebuf : curl_easy_strerror(rc); return false; }
    if (code < 200 || code >= 300) { err = "HTTP " + std::to_string(code) + ": " + body; return false; }
    return true;
}

// GET + parse. Throws std::runtime_error on transport or parse failure (callers mirror the
// Python try/except by catching and reporting per-symbol without aborting the whole run).
inline json get_json(const std::string& url, long timeout_s = 30) {
    std::string body, err;
    if (!http_get(url, body, err, timeout_s)) throw std::runtime_error(err);
    return json::parse(body);
}

// ---- URL builders (kept here so the endpoints live in one place) ----
inline std::string binance_klines_url(const std::string& sym, const std::string& interval,
                                      long long start_ms, int limit) {
    std::string u = "https://api.binance.com/api/v3/klines?symbol=" + sym +
                    "&interval=" + interval;
    if (start_ms > 0) u += "&startTime=" + std::to_string(start_ms);
    u += "&limit=" + std::to_string(limit);
    return u;
}

inline std::string binance_funding_url(const std::string& sym_usdt, long long start_ms, int limit) {
    return "https://fapi.binance.com/fapi/v1/fundingRate?symbol=" + sym_usdt + "USDT" +
           "&startTime=" + std::to_string(start_ms) + "&limit=" + std::to_string(limit);
}

// yfinance ^NDX replacement: Yahoo v8 chart endpoint, non-adjusted daily.
inline std::string yahoo_ndx_url(int range_days = 60) {
    return "https://query1.finance.yahoo.com/v8/finance/chart/%5ENDX?range=" +
           std::to_string(range_days) + "d&interval=1d&events=div%2Csplits";
}

} // namespace crypto
