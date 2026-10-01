// =============================================================================
// slab_buffer.hpp — full-preload buffers: RamBuffer and DiskBackedBuffer
//
// Storage layout (both classes):
//
//   slab 0: [frame 0][frame 1] ... [frame k-1]     each slab ≈ 64 MiB
//   slab 1: [frame k][frame k+1] ...
//
// Frames have a fixed size, so frame i lives at
//     slab[i / frames_per_slab] + (i % frames_per_slab) * slot_bytes
// which is O(1), needs no per-frame allocation and never fragments memory.
//
// The two classes differ only in where a slab comes from:
//   RamBuffer         anonymous memory (mmap MAP_ANONYMOUS)
//   DiskBackedBuffer  a region of a temporary file mapped into memory
//                     (mmap MAP_SHARED); the kernel's page cache decides what
//                     stays in RAM, so clips larger than RAM still work.
//
// Thread safety: one writer, any number of readers. Slabs never move once
// allocated (the slab table is reserved up front), and a frame becomes
// visible to readers only after commit_write() publishes the new count with
// release semantics.
// =============================================================================
#pragma once

#include <atomic>
#include <cstdint>
#include <string>
#include <vector>

#include "vcam/buffer/frame_buffer.hpp"

namespace vcam {

class SlabBuffer : public FrameBuffer {
public:
    // max_bytes: hard limit for the stored frames (the memory/disk budget).
    explicit SlabBuffer(uint64_t max_bytes);
    ~SlabBuffer() override = default;

    SlabBuffer(const SlabBuffer&) = delete;
    SlabBuffer& operator=(const SlabBuffer&) = delete;

    Status configure(const FrameFormat& format, std::optional<uint64_t> expected_frames) override;
    const FrameFormat& format() const override { return format_; }

    Result<MutableFrameView> begin_write() override;
    Status commit_write(const FrameTiming& timing) override;
    void mark_complete() override { complete_.store(true, std::memory_order_release); }

    bool is_complete() const override { return complete_.load(std::memory_order_acquire); }
    uint64_t frame_count() const override { return committed_.load(std::memory_order_acquire); }
    FrameLease acquire(uint64_t source_index) const override;
    std::optional<uint64_t> newest_index() const override;
    const Timeline* timeline() const override { return &timeline_; }

    void set_time_base(Rational time_base) { timeline_.set_time_base(time_base); }

    BufferStats stats() const override;
    void clear() override;

    // Exposed for tests and diagnostics.
    uint64_t frames_per_slab() const { return frames_per_slab_; }
    size_t slot_bytes() const { return slot_bytes_; }

protected:
    // Returns memory for slab number `slab_index` (slab_bytes() long), or nullptr.
    virtual uint8_t* allocate_slab(size_t slab_index, size_t slab_bytes) = 0;
    virtual void release_slab(uint8_t* slab, size_t slab_bytes) = 0;
    // Hook called when a reader enters a new slab (disk buffer uses it for read-ahead).
    virtual void on_reader_enters_slab(size_t /*slab_index*/) const {}

    size_t slab_bytes() const { return slab_bytes_; }
    // Number of allocated slabs; safe to call from reader threads.
    size_t slab_count() const { return allocated_slabs_.load(std::memory_order_acquire); }
    const uint8_t* slab_at(size_t index) const { return slabs_[index]; }
    void release_all_slabs();
    virtual const char* kind_name() const = 0;
    virtual bool stores_in_ram() const = 0;

private:
    uint64_t max_bytes_;
    FrameFormat format_{};
    bool configured_ = false;

    size_t slot_bytes_ = 0;          // frame size rounded up to 64 bytes
    uint64_t frames_per_slab_ = 0;
    size_t slab_bytes_ = 0;          // frames_per_slab * slot_bytes, page aligned
    size_t max_slabs_ = 0;
    uint64_t expected_frames_ = 0;

    std::vector<uint8_t*> slabs_;    // reserved to max_slabs_: never reallocates
    std::atomic<size_t> allocated_slabs_{0};  // slabs_.size(), readable from any thread
    Timeline timeline_;
    std::atomic<uint64_t> committed_{0};
    std::atomic<bool> complete_{false};
    mutable std::atomic<size_t> reader_slab_{static_cast<size_t>(-1)};
};

// Full preload into RAM.
class RamBuffer final : public SlabBuffer {
public:
    explicit RamBuffer(uint64_t max_bytes) : SlabBuffer(max_bytes) {}
    ~RamBuffer() override { release_all_slabs(); }

protected:
    uint8_t* allocate_slab(size_t slab_index, size_t slab_bytes) override;
    void release_slab(uint8_t* slab, size_t slab_bytes) override;
    const char* kind_name() const override { return "ram"; }
    bool stores_in_ram() const override { return true; }
};

// Full preload into a temporary file in `spill_directory`. The file is deleted
// from the directory right after creation, so it disappears automatically
// when the process exits — even after a crash.
class DiskBackedBuffer final : public SlabBuffer {
public:
    DiskBackedBuffer(std::string spill_directory, uint64_t max_bytes);
    ~DiskBackedBuffer() override;

    // Creates the temporary file. Call before configure().
    Status open();

protected:
    uint8_t* allocate_slab(size_t slab_index, size_t slab_bytes) override;
    void release_slab(uint8_t* slab, size_t slab_bytes) override;
    void on_reader_enters_slab(size_t slab_index) const override;
    const char* kind_name() const override { return "disk"; }
    bool stores_in_ram() const override { return false; }

private:
    std::string spill_directory_;
    int fd_ = -1;
};

}  // namespace vcam
