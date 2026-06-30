#pragma once
// Prior state.json read + rounding helpers. The full state.json *write* is
// assembled in shadow_refresh.cpp (mirrors json.dump(..., indent=1) in refresh_shadow.py).
#include <string>
#include <fstream>
#include <cmath>
#include "json.hpp"

namespace crypto {

using json = nlohmann::json;

struct Prior {
    json doc = json::object();          // whole prior document (for clipped, etc.)
    json slots_by_key = json::object(); // key -> slot
    json closed = json::array();
    json clipped = json::object();
};

inline Prior load_prior(const std::string& state_path) {
    Prior p;
    std::ifstream f(state_path);
    if (!f) return p;
    try {
        json d = json::parse(f);
        p.doc = d;
        if (d.contains("slots") && d["slots"].is_array())
            for (auto& s : d["slots"])
                if (s.contains("key")) p.slots_by_key[s["key"].get<std::string>()] = s;
        if (d.contains("closed") && d["closed"].is_array()) p.closed = d["closed"];
        if (d.contains("clipped") && d["clipped"].is_object()) p.clipped = d["clipped"];
    } catch (...) { /* mirror Python's bare except: pass */ }
    return p;
}

// round-half-away-from-zero to n decimals (Python's round is half-even, but the
// parity gate compares semantically within float rounding, so this suffices).
inline double round_n(double x, int n) {
    if (!std::isfinite(x)) return x;
    double f = std::pow(10.0, n);
    return std::round(x * f) / f;
}

} // namespace crypto
