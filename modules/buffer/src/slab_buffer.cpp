#include "vcam/buffer/slab_buffer.hpp"

#include <fcntl.h>     // open flags, posix_fallocate
#include <sys/mman.h>  // mmap, munmap, madvise
#include <unistd.h>    // close, unlink, sysconf

#include <algorithm>
#include <cerrno>
#include <cstring>

#include "vcam/core/log.hpp"
#include "vcam/core/text_format.hpp"

namespace vcam {

namespace {

constexpr const char* kModule = "buffer";
constexpr size_t kSlotAlignment = 64;                 // cache line / SIMD friendly
constexpr size_t kTargetSlabBytes = 64ull << 20;      // ~64 MiB per slab

size_t round_up(size_t value, size_t multiple) {
    return (value + multiple - 1) / multiple * multiple;
}

size_t page_size() {
    // sysconf(_SC_PAGESIZE): memory page size (4096 on x86-64). mmap offsets
    // and lengths work in whole pages.
    static const size_t size = static_cast<size_t>(sysconf(_SC_PAGESIZE));
    return size;
}

}  // namespace

// ============================================================================
// SlabBuffer
// ============================================================================

SlabBuffer::SlabBuffer(uint64_t max_bytes) : max_bytes_(max_bytes) {}

Status SlabBuffer::configure(const FrameFormat& format, std::optional<uint64_t> expected_frames) {
    if (format.size_bytes == 0) {
        return Status(StatusCode::InvalidArgument, "buffer: frame format has zero size");
    }
    clear();
    format_ = format;
    slot_bytes_ = round_up(format.size_bytes, kSlotAlignment);
    frames_per_slab_ = std::max<uint64_t>(1, kTargetSlabBytes / slot_bytes_);
    slab_bytes_ = round_up(static_cast<size_t>(frames_per_slab_) * slot_bytes_, page_size());

    // Enough slab pointers for the whole budget, reserved now so the table never
    // reallocates while a reader is using it.
    max_slabs_ = static_cast<size_t>(max_bytes_ / slab_bytes_) + 2;
    slabs_.clear();
    slabs_.reserve(max_slabs_);

    expected_frames_ = expected_frames.value_or(0);
    if (expected_frames_ > 0) {
        timeline_.reserve(static_cast<size_t>(expected_frames_));
    }
    configured_ = true;
    VCAM_DEBUG(kModule, kind_name() << " buffer: slot " << slot_bytes_ << " B, " << frames_per_slab_
                                    << " frames/slab, budget " << format_bytes(max_bytes_));
    return Status::ok_status();
}

Result<MutableFrameView> SlabBuffer::begin_write() {
    if (!configured_) {
        return Status(StatusCode::Internal, "buffer: begin_write() before configure()");
    }
    const uint64_t index = committed_.load(std::memory_order_relaxed);  // only the writer changes it
    const size_t slab_index = static_cast<size_t>(index / frames_per_slab_);
    const size_t slot_in_slab = static_cast<size_t>(index % frames_per_slab_);

    // Budget check per frame. (The last slab may extend past the budget, but
    // untouched pages of anonymous memory are never actually allocated.)
    if ((index + 1) * slot_bytes_ > max_bytes_) {
        return Status(StatusCode::ResourceExhausted,
                      std::string(kind_name()) + " buffer budget of " + format_bytes(max_bytes_) +
                          " exhausted after " + std::to_string(index) + " frames");
    }
    if (slab_index >= slabs_.size()) {
        if (slabs_.size() >= max_slabs_) {
            return Status(StatusCode::Internal, "buffer slab table full");
        }
        uint8_t* slab = allocate_slab(slabs_.size(), slab_bytes_);
        if (slab == nullptr) {
            return Status(StatusCode::ResourceExhausted,
                          std::string("cannot allocate ") + format_bytes(slab_bytes_) + " for the " + kind_name() +
                              " buffer: " + std::strerror(errno));
        }
        slabs_.push_back(slab);  // capacity reserved: no reallocation
        allocated_slabs_.store(slabs_.size(), std::memory_order_release);
    }

    uint8_t* slot = slabs_[slab_index] + slot_in_slab * slot_bytes_;
    return MutableFrameView{format_, std::span<uint8_t>(slot, format_.size_bytes)};
}

Status SlabBuffer::commit_write(const FrameTiming& timing) {
    timeline_.append(timing);
    // Release ordering: the frame bytes and the timeline entry written before
    // this store are visible to any reader that loads the new count (acquire).
    committed_.fetch_add(1, std::memory_order_release);
    return Status::ok_status();
}

FrameLease SlabBuffer::acquire(uint64_t source_index) const {
    if (source_index >= committed_.load(std::memory_order_acquire)) {
        return FrameLease();  // not loaded (yet)
    }
    const size_t slab_index = static_cast<size_t>(source_index / frames_per_slab_);
    const size_t slot_in_slab = static_cast<size_t>(source_index % frames_per_slab_);

    // Notify subclasses when the reader moves into another slab (cheap check).
    if (reader_slab_.exchange(slab_index, std::memory_order_relaxed) != slab_index) {
        on_reader_enters_slab(slab_index);
    }

    const uint8_t* slot = slabs_[slab_index] + slot_in_slab * slot_bytes_;
    FrameTiming timing{};
    if (is_complete() && source_index < timeline_.size()) {
        timing = timeline_[static_cast<size_t>(source_index)];
    } else {
        timing.source_index = source_index;
    }
    // Preloaded frames are never overwritten: the lease needs no owner.
    return FrameLease(FrameView{format_, std::span<const uint8_t>(slot, format_.size_bytes), timing}, nullptr, 0);
}

std::optional<uint64_t> SlabBuffer::newest_index() const {
    const uint64_t count = committed_.load(std::memory_order_acquire);
    if (count == 0) {
        return std::nullopt;
    }
    return count - 1;
}

BufferStats SlabBuffer::stats() const {
    BufferStats stats;
    stats.kind = kind_name();
    stats.frames_loaded = committed_.load(std::memory_order_acquire);
    stats.frames_expected = expected_frames_;
    stats.capacity_frames = slab_bytes_ > 0 ? (max_bytes_ / slot_bytes_) : 0;
    stats.frame_bytes = format_.size_bytes;
    // RAM: anonymous pages are only allocated when written, so the memory in
    // use is (frames stored) x (slot size), not the size of the reserved slabs.
    // Disk: posix_fallocate reserves whole slabs, so count those.
    const uint64_t used = stats.frames_loaded * slot_bytes_;
    const uint64_t reserved = static_cast<uint64_t>(slab_count()) * slab_bytes_;
    stats.memory_bytes = stores_in_ram() ? used : 0;
    stats.disk_bytes = stores_in_ram() ? 0 : reserved;
    stats.complete = is_complete();
    return stats;
}

void SlabBuffer::release_all_slabs() {
    for (uint8_t* slab : slabs_) {
        release_slab(slab, slab_bytes_);
    }
    slabs_.clear();
    allocated_slabs_.store(0, std::memory_order_release);
}

void SlabBuffer::clear() {
    release_all_slabs();
    timeline_.clear();
    committed_.store(0, std::memory_order_release);
    complete_.store(false, std::memory_order_release);
    reader_slab_.store(static_cast<size_t>(-1), std::memory_order_relaxed);
}

// ============================================================================
// RamBuffer
// ============================================================================

uint8_t* RamBuffer::allocate_slab(size_t /*slab_index*/, size_t bytes) {
    // Anonymous mmap: page-aligned, zero-filled memory straight from the kernel.
    // For multi-megabyte blocks it is returned to the OS immediately on munmap
    // (malloc might keep it), which matters when buffers are rebuilt.
    void* memory = mmap(nullptr, bytes, PROT_READ | PROT_WRITE, MAP_PRIVATE | MAP_ANONYMOUS, -1, 0);
    return memory == MAP_FAILED ? nullptr : static_cast<uint8_t*>(memory);
}

void RamBuffer::release_slab(uint8_t* slab, size_t bytes) {
    munmap(slab, bytes);
}

// ============================================================================
// DiskBackedBuffer
// ============================================================================

DiskBackedBuffer::DiskBackedBuffer(std::string spill_directory, uint64_t max_bytes)
    : SlabBuffer(max_bytes), spill_directory_(std::move(spill_directory)) {}

DiskBackedBuffer::~DiskBackedBuffer() {
    release_all_slabs();
    if (fd_ >= 0) {
        ::close(fd_);
    }
}

Status DiskBackedBuffer::open() {
    std::string path = spill_directory_ + "/vcam-spill-XXXXXX";
    // mkstemp replaces XXXXXX with a unique name and creates the file safely.
    fd_ = mkstemp(path.data());
    if (fd_ < 0) {
        return Status(StatusCode::IoError,
                      "cannot create spill file in '" + spill_directory_ + "': " + std::strerror(errno));
    }
    // Remove the name immediately: the data stays reachable through fd_ and the
    // kernel frees the disk space when the file is closed (also on a crash).
    unlink(path.c_str());
    VCAM_DEBUG(kModule, "disk buffer spill file created in " << spill_directory_);
    return Status::ok_status();
}

uint8_t* DiskBackedBuffer::allocate_slab(size_t slab_index, size_t bytes) {
    if (fd_ < 0) {
        errno = EBADF;
        return nullptr;
    }
    const auto offset = static_cast<off_t>(slab_index * bytes);
    // posix_fallocate reserves real disk blocks now, so a full disk is reported
    // here as an error instead of crashing later with SIGBUS on first write.
    const int result = posix_fallocate(fd_, offset, static_cast<off_t>(bytes));
    if (result != 0) {
        errno = result;
        return nullptr;
    }
    // MAP_SHARED: writes go to the file (through the page cache), so the kernel
    // can evict pages under memory pressure and reload them on access.
    void* memory = mmap(nullptr, bytes, PROT_READ | PROT_WRITE, MAP_SHARED, fd_, offset);
    return memory == MAP_FAILED ? nullptr : static_cast<uint8_t*>(memory);
}

void DiskBackedBuffer::release_slab(uint8_t* slab, size_t bytes) {
    munmap(slab, bytes);
}

void DiskBackedBuffer::on_reader_enters_slab(size_t slab_index) const {
    // Ask the kernel to start reading the NEXT slab from disk in the background
    // (MADV_WILLNEED is asynchronous), so the pacer does not wait on disk I/O.
    if (slab_index + 1 < slab_count()) {
        // const_cast is safe: madvise does not modify the data.
        madvise(const_cast<uint8_t*>(slab_at(slab_index + 1)), slab_bytes(), MADV_WILLNEED);
    }
}

}  // namespace vcam
