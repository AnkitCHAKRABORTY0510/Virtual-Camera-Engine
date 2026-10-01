#include "vcam/core/text_format.hpp"

#include <cctype>
#include <cmath>
#include <cstdio>

namespace vcam {

std::string format_bytes(uint64_t bytes) {
    static const char* const kUnits[] = {"B", "KiB", "MiB", "GiB", "TiB"};
    double value = static_cast<double>(bytes);
    int unit = 0;
    while (value >= 1024.0 && unit < 4) {
        value /= 1024.0;
        ++unit;
    }
    char text[32];
    if (unit == 0) {
        std::snprintf(text, sizeof(text), "%llu B", static_cast<unsigned long long>(bytes));
    } else {
        std::snprintf(text, sizeof(text), "%.1f %s", value, kUnits[unit]);
    }
    return text;
}

std::string format_seconds(double seconds) {
    if (seconds < 0) {
        seconds = 0;
    }
    const auto whole = static_cast<long long>(seconds);
    const int millis = static_cast<int>(std::lround((seconds - static_cast<double>(whole)) * 1000.0)) % 1000;
    const long long hours = whole / 3600;
    const long long minutes = (whole / 60) % 60;
    const long long secs = whole % 60;
    char text[48];
    if (hours > 0) {
        std::snprintf(text, sizeof(text), "%lld:%02lld:%02lld.%03d", hours, minutes, secs, millis);
    } else {
        std::snprintf(text, sizeof(text), "%lld:%02lld.%03d", minutes, secs, millis);
    }
    return text;
}

bool parse_bytes(const std::string& text, uint64_t& out) {
    // Split "1.5GiB" into number "1.5" and suffix "GiB".
    size_t position = 0;
    while (position < text.size() &&
           (std::isdigit(static_cast<unsigned char>(text[position])) || text[position] == '.')) {
        ++position;
    }
    if (position == 0) {
        return false;
    }
    double number = 0;
    try {
        number = std::stod(text.substr(0, position));
    } catch (...) {
        return false;
    }
    std::string suffix;
    for (size_t i = position; i < text.size(); ++i) {
        if (!std::isspace(static_cast<unsigned char>(text[i]))) {
            suffix += static_cast<char>(std::toupper(static_cast<unsigned char>(text[i])));
        }
    }
    double multiplier = 1;
    if (suffix.empty() || suffix == "B") {
        multiplier = 1;
    } else if (suffix == "K" || suffix == "KB" || suffix == "KIB") {
        multiplier = 1024.0;
    } else if (suffix == "M" || suffix == "MB" || suffix == "MIB") {
        multiplier = 1024.0 * 1024.0;
    } else if (suffix == "G" || suffix == "GB" || suffix == "GIB") {
        multiplier = 1024.0 * 1024.0 * 1024.0;
    } else if (suffix == "T" || suffix == "TB" || suffix == "TIB") {
        multiplier = 1024.0 * 1024.0 * 1024.0 * 1024.0;
    } else {
        return false;
    }
    out = static_cast<uint64_t>(number * multiplier);
    return true;
}

}  // namespace vcam
