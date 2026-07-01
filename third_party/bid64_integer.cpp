// bid64_integer.cpp — decimal64 (BID) encode/decode for TWS order quantities.
//
// HISTORY: this was an INTEGER-ONLY shim (enc_int + llround) on the assumption
// that order sizes are whole share counts. That silently broke fractional-coin
// crypto orders: __binary64_to_bid64 llround()'d 0.05 BTC -> 0, and a cashQty
// (fractional-notional) order round-tripped to garbage -> IB err 320 '-inf'.
// That single rounding was THE blocker on the spot-Paxos path.
//
// This version encodes the FULL fractional value into a valid IEEE-754-2008
// decimal64 (BID). It uses the small-coefficient form only: the coefficient is
// kept < 2^53 (<= ~15 significant digits) and the biased exponent <= 767, so
// the '11' large-form combination bits are never set and the existing decoder's
// small-coef branch stays exact. 15 sig digits is ample for any order qty or
// cashQty. Name kept (bid64_integer.cpp) so CMakeLists needs no edit; it is no
// longer integer-only.
#include <cstdint>
#include <cstring>
#include <cmath>
#include <cstdlib>
#include <cstdio>
typedef uint64_t Dec;
static const uint64_t EXP_BIAS = 398;
static const uint64_t COEF_MASK = (1ULL<<53)-1;   // 53-bit coefficient (< 2^53)

// Encode an arbitrary finite double into decimal64 BID (small-coefficient form).
static Dec enc(double d){
    if(!std::isfinite(d) || d==0.0) return (EXP_BIAS<<53);   // canonical zero (coef 0, exp 0)
    uint64_t sign = std::signbit(d) ? (1ULL<<63) : 0;
    double a = std::fabs(d);
    int exp = 0;
    // Normalise the coefficient into [1e15, 1e16): up to 16 significant digits.
    while(a <  1e15 && exp > -350){ a *= 10.0; --exp; }
    while(a >= 1e16 && exp <  350){ a /= 10.0; ++exp; }
    unsigned long long coef = (unsigned long long)std::llround(a);
    // Keep the coefficient < 2^53 so the small-coefficient form always applies.
    if(coef >= (1ULL<<53)){ coef = (unsigned long long)std::llround(a/10.0); ++exp; }
    // Strip trailing zeros for a canonical, compact encoding.
    while(coef!=0 && coef%10==0){ coef/=10; ++exp; }
    long long biased = (long long)exp + (long long)EXP_BIAS;
    if(biased < 0)   biased = 0;      // clamp (unreachable for order-sized values)
    if(biased > 767) biased = 767;
    return sign | ((uint64_t)biased << 53) | (coef & COEF_MASK);
}

// Decode decimal64 BID -> double. Exact for the small-coef form; approximates the
// rare large-coef ('11') form (which enc() never produces).
static double dec(Dec x){
    uint64_t sign = x>>63;
    uint64_t top2 = (x>>61)&3;
    double coef; int exp;
    if(top2==3){   // large-coefficient form (not produced here) — approximate
        coef = (double)((x & ((1ULL<<51)-1)) | (1ULL<<53));
        exp  = (int)((x>>51)&0x3FF) - (int)EXP_BIAS;
    } else {
        coef = (double)(x & COEF_MASK);
        exp  = (int)((x>>53)&0x3FF) - (int)EXP_BIAS;
    }
    double v = coef * std::pow(10.0, exp);
    return sign ? -v : v;
}

extern "C" {
Dec    __binary64_to_bid64(double d, unsigned, unsigned*){ return enc(d); }
double __bid64_to_binary64(Dec x, unsigned, unsigned*){ return dec(x); }
Dec    __bid64_from_string(char* s, unsigned, unsigned*){ return enc(s? std::strtod(s,nullptr):0.0); }
void   __bid64_to_string(char* out, Dec x, unsigned*){ if(out) std::snprintf(out,32,"%.15g",dec(x)); }
Dec    __bid64_add(Dec a, Dec b, unsigned, unsigned*){ return enc(dec(a)+dec(b)); }
Dec    __bid64_sub(Dec a, Dec b, unsigned, unsigned*){ return enc(dec(a)-dec(b)); }
Dec    __bid64_mul(Dec a, Dec b, unsigned, unsigned*){ return enc(dec(a)*dec(b)); }
Dec    __bid64_div(Dec a, Dec b, unsigned, unsigned*){ double db=dec(b); return enc(db!=0? dec(a)/db : 0.0); }
}
