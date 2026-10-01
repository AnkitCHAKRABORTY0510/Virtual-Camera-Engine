#include "vcam/buffer/memory_budget.hpp"

#include <sys/statvfs.h>  // statvfs: filesystem statistics

#include <fstream>
#include <sstream>

#include "vcam/core/text_format.hpp"

namespace vcam {

std::optional<uint64_t> available_memory_bytes() {
    // /proc/meminfo lines look like "MemAvailable:   7654321 kB".
    std::ifstream meminfo("/proc/meminfo");
    std::string line;
    while (std::getline(meminfo, line)) {
        if (line.rfind("MemAvailable:", 0) == 0) {
            std::istringstream fields(line.substr(13));
            uint64_t kibibytes = 0;
            fields >> kibibytes;
            return kibibytes * 1024;
        }
    }
    return std::nullopt;
}

std::optional<uint64_t> free_disk_bytes(const std::string& directory) {
    struct statvfs info {};
    if (statvfs(directory.c_str(), &info) != 0) {
        return std::nullopt;
    }
    // f_bavail = blocks available to unprivileged users, f_frsize = block size.
    return static_cast<uint64_t>(info.f_bavail) * info.f_frsize;
}

uint64_t estimate_buffer_bytes(const FrameFormat& format, uint64_t frames) {
    const uint64_t slot = (format.size_bytes + 63) / 64 * 64;
    return slot * frames;
}

uint64_t default_ram_budget() {
    const std::optional<uint64_t> available = available_memory_bytes();
    return available ? *available / 2 : (2ull << 30);  // fall back to 2 GiB
}

Status check_fits(const std::string& what, uint64_t needed, uint64_t budget, const std::string& advice) {
    if (needed <= budget) {
        return Status::ok_status();
    }
    return Status(StatusCode::ResourceExhausted,
                  what + " needs about " + format_bytes(needed) + " but only " + format_bytes(budget) +
                      " is allowed. " + advice);
}

}  // namespace vcam
