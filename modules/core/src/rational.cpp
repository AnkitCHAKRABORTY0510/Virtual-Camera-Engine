#include "vcam/core/rational.hpp"

#include <array>
#include <cctype>   // std::isspace
#include <charconv>
#include <cmath>
#include <numeric>  // std::gcd

namespace vcam {

namespace {

// Floor division for 128-bit integers with a positive divisor.
// C++ integer division truncates towards zero (-7 / 2 == -3); for timestamps
// we need floor (-7 / 2 == -4) so negative values round consistently.
Int128 floor_div(Int128 numerator, Int128 divisor) {
    Int128 quotient = numerator / divisor;
    Int128 remainder = numerator % divisor;
    if (remainder != 0 && ((remainder < 0) != (divisor < 0))) {
        quotient -= 1;
    }
    return quotient;
}

// Ceil division built from floor division: ceil(a/b) = -floor(-a/b).
Int128 ceil_div(Int128 numerator, Int128 divisor) {
    return -floor_div(-numerator, divisor);
}

// Rounds a/b to the nearest integer, halves away from zero (2.5 -> 3, -2.5 -> -3).
Int128 nearest_div(Int128 numerator, Int128 divisor) {
    if (numerator >= 0) {
        return floor_div(2 * numerator + divisor, 2 * divisor);
    }
    return -floor_div(-2 * numerator + divisor, 2 * divisor);
}

// Parses a whole decimal number such as "30000". Returns false on any junk.
bool parse_int64(std::string_view text, int64_t& out) {
    if (text.empty()) {
        return false;
    }
    // std::from_chars: locale-independent, no allocation, reports where it stopped.
    const char* begin = text.data();
    const char* end = text.data() + text.size();
    auto [ptr, error] = std::from_chars(begin, end, out);
    return error == std::errc() && ptr == end;
}

std::string_view trim(std::string_view text) {
    while (!text.empty() && std::isspace(static_cast<unsigned char>(text.front()))) {
        text.remove_prefix(1);
    }
    while (!text.empty() && std::isspace(static_cast<unsigned char>(text.back()))) {
        text.remove_suffix(1);
    }
    return text;
}

}  // namespace

Rational make_rational(int64_t num, int64_t den) {
    if (den == 0) {
        return Rational{0, 0};
    }
    if (den < 0) {  // keep the sign in the numerator only
        num = -num;
        den = -den;
    }
    // std::gcd returns the greatest common divisor (always >= 0).
    int64_t divisor = std::gcd(num, den);
    if (divisor > 1) {
        num /= divisor;
        den /= divisor;
    }
    return Rational{num, den};
}

bool is_valid(Rational r) {
    return r.den > 0;
}

bool is_positive(Rational r) {
    return is_valid(r) && r.num > 0;
}

double to_double(Rational r) {
    if (!is_valid(r)) {
        return 0.0;
    }
    return static_cast<double>(r.num) / static_cast<double>(r.den);
}

int compare(Rational a, Rational b) {
    // a.num/a.den  <=>  b.num/b.den   is the same as   a.num*b.den <=> b.num*a.den
    // (denominators are positive, so cross-multiplying keeps the order).
    Int128 left = static_cast<Int128>(a.num) * b.den;
    Int128 right = static_cast<Int128>(b.num) * a.den;
    if (left < right) return -1;
    if (left > right) return 1;
    return 0;
}

bool operator==(Rational a, Rational b) { return compare(a, b) == 0; }
bool operator!=(Rational a, Rational b) { return compare(a, b) != 0; }

Rational invert(Rational r) {
    return make_rational(r.den, r.num);
}

int64_t rescale(int64_t value, Rational from_unit, Rational to_unit, Rounding rounding) {
    // result = value * (from.num / from.den) / (to.num / to.den)
    //        = (value * from.num * to.den) / (from.den * to.num)
    if (!is_valid(from_unit) || !is_positive(to_unit)) {
        return 0;
    }
    Int128 numerator = static_cast<Int128>(value) * from_unit.num * to_unit.den;
    Int128 divisor = static_cast<Int128>(from_unit.den) * to_unit.num;
    if (divisor < 0) {  // only possible if from_unit is negative; keep divisor positive
        numerator = -numerator;
        divisor = -divisor;
    }

    Int128 result = 0;
    switch (rounding) {
        case Rounding::Down:    result = floor_div(numerator, divisor); break;
        case Rounding::Up:      result = ceil_div(numerator, divisor); break;
        case Rounding::Nearest: result = nearest_div(numerator, divisor); break;
    }
    return static_cast<int64_t>(result);
}

Result<Rational> parse_rational(std::string_view raw_text) {
    std::string_view text = trim(raw_text);
    auto invalid = [&]() {
        return Status(StatusCode::InvalidArgument,
                      "invalid rational value '" + std::string(raw_text) +
                          "' (expected e.g. 30, 29.97 or 30000/1001)");
    };

    // Form 1: "num/den"
    size_t slash = text.find('/');
    if (slash != std::string_view::npos) {
        int64_t num = 0;
        int64_t den = 0;
        if (!parse_int64(trim(text.substr(0, slash)), num) ||
            !parse_int64(trim(text.substr(slash + 1)), den) || den == 0) {
            return invalid();
        }
        return make_rational(num, den);
    }

    // Form 2: "whole.fraction"
    size_t dot = text.find('.');
    if (dot != std::string_view::npos) {
        std::string_view whole_part = text.substr(0, dot);
        std::string_view fraction_part = text.substr(dot + 1);
        if (fraction_part.empty() || fraction_part.size() > 9) {
            return invalid();
        }
        int64_t whole = 0;
        int64_t fraction = 0;
        if (!whole_part.empty() && !parse_int64(whole_part, whole)) {
            return invalid();
        }
        if (!parse_int64(fraction_part, fraction) || fraction < 0) {
            return invalid();
        }
        int64_t scale = 1;
        for (size_t i = 0; i < fraction_part.size(); ++i) {
            scale *= 10;
        }
        Rational exact = make_rational(whole * scale + fraction, scale);

        // Snap the usual NTSC-style rates to their exact x000/1001 values.
        // Using double here is fine: it only decides WHICH exact fraction to
        // return; the returned value itself is exact.
        static constexpr std::array<int64_t, 5> kNtscNumerators = {24000, 30000, 48000, 60000, 120000};
        for (int64_t ntsc_num : kNtscNumerators) {
            double ntsc_value = static_cast<double>(ntsc_num) / 1001.0;
            if (std::fabs(to_double(exact) - ntsc_value) < 0.001) {
                return make_rational(ntsc_num, 1001);
            }
        }
        return exact;
    }

    // Form 3: plain integer "30"
    int64_t whole = 0;
    if (!parse_int64(text, whole)) {
        return invalid();
    }
    return make_rational(whole, 1);
}

std::string to_string(Rational r) {
    if (r.den == 1) {
        return std::to_string(r.num);
    }
    return std::to_string(r.num) + "/" + std::to_string(r.den);
}

}  // namespace vcam
