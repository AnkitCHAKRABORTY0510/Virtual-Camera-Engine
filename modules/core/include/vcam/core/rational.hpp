// =============================================================================
// rational.hpp — exact rational numbers for frame rates and time bases
//
// Why not double?
//   Frame rates such as 29.97 FPS are really 30000/1001, and container time
//   bases are fractions such as 1/15360 s. Keeping them as integer fractions
//   lets the engine compare timestamps EXACTLY, so "source frame 1 at 30 FPS"
//   and "output slot 1 at 30 FPS" are guaranteed to be the same instant with
//   no floating-point rounding (see docs/ARCHITECTURE.md §1.3 and §7.2).
//
// All multiplications that could overflow 64 bits use a 128-bit intermediate
// (GCC/Clang `__int128`), so values stay exact even for runs lasting years.
// =============================================================================
#pragma once

#include <cstdint>
#include <string>
#include <string_view>

#include "vcam/core/status.hpp"

namespace vcam {

// 128-bit signed integer used only for intermediate products.
// `__extension__` silences the -Wpedantic warning: __int128 is a GCC/Clang
// extension, which is fine because this project targets Linux with GCC/Clang.
__extension__ using Int128 = __int128;

struct Rational {
    int64_t num = 0;  // numerator
    int64_t den = 1;  // denominator, always > 0 after make_rational()
};

// Builds a reduced fraction with a positive denominator, e.g. (60, 2) -> 30/1.
// Returns {0, 0} (invalid) if den == 0.
Rational make_rational(int64_t num, int64_t den);

// A rational is usable as a rate/time base when den > 0.
bool is_valid(Rational r);

// True when the value is valid and strictly greater than zero.
bool is_positive(Rational r);

// Approximate value for printing only. Never use it for timing decisions.
double to_double(Rational r);

// Exact three-way comparison: returns -1 if a < b, 0 if equal, +1 if a > b.
int compare(Rational a, Rational b);

bool operator==(Rational a, Rational b);
bool operator!=(Rational a, Rational b);

// 1 / r  (e.g. frame rate 30/1 -> frame period 1/30 s).
Rational invert(Rational r);

enum class Rounding {
    Down,     // towards minus infinity (floor)
    Nearest,  // half away from zero
    Up,       // towards plus infinity (ceil)
};

// Converts a count of `from_unit` ticks into `to_unit` ticks:
//     result = value * from_unit / to_unit
// Example: 512 ticks of 1/15360 s -> nanoseconds (1/1000000000 s) = 33333333.
// Uses a 128-bit intermediate, so it does not overflow for realistic values.
int64_t rescale(int64_t value, Rational from_unit, Rational to_unit, Rounding rounding = Rounding::Nearest);

// Parses a frame-rate string. Accepted forms:
//   "30"          -> 30/1
//   "30000/1001"  -> 30000/1001
//   "29.97"       -> 30000/1001   (common NTSC rates 23.976, 29.97, 47.952,
//   "59.94"       -> 60000/1001    59.94, 119.88 are snapped to their exact
//                                   x000/1001 values, like FFmpeg does)
//   "12.5"        -> 25/2         (other decimals are converted exactly)
Result<Rational> parse_rational(std::string_view text);

// "30000/1001" or "30" when the denominator is 1.
std::string to_string(Rational r);

}  // namespace vcam
