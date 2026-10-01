// =============================================================================
// text_format.hpp — small helpers for human-readable numbers in logs and reports
// =============================================================================
#pragma once

#include <cstdint>
#include <string>

namespace vcam {

// 1536 -> "1.5 KiB", 1932735283 -> "1.8 GiB" (binary units, one decimal).
std::string format_bytes(uint64_t bytes);

// 3725.5 -> "1:02:05.500"; 12.25 -> "0:12.250"
std::string format_seconds(double seconds);

// Parses "512M", "2G", "1.5GiB", "1000000" (bytes). Returns false on junk.
bool parse_bytes(const std::string& text, uint64_t& out);

}  // namespace vcam
