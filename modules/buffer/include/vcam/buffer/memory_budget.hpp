// =============================================================================
// memory_budget.hpp — "will this clip fit?" checks done BEFORE loading
//
// Decoding a 15-minute HD clip into RAM can need 100 GiB. Instead of being
// killed by the kernel's out-of-memory killer halfway through, the engine
// estimates the size first and refuses with a helpful message.
// =============================================================================
#pragma once

#include <cstdint>
#include <optional>
#include <string>

#include "vcam/core/frame.hpp"
#include "vcam/core/status.hpp"

namespace vcam {

// MemAvailable from /proc/meminfo: RAM that can be used without swapping.
std::optional<uint64_t> available_memory_bytes();

// Free space in the filesystem containing `directory` (statvfs).
std::optional<uint64_t> free_disk_bytes(const std::string& directory);

// Bytes needed to store `frames` frames of `format` (with slot alignment).
uint64_t estimate_buffer_bytes(const FrameFormat& format, uint64_t frames);

// Default RAM budget: half of MemAvailable (leaves room for the rest of the system).
uint64_t default_ram_budget();

// Ok when `needed` fits in `budget`; otherwise ResourceExhausted with advice.
Status check_fits(const std::string& what, uint64_t needed, uint64_t budget, const std::string& advice);

}  // namespace vcam
