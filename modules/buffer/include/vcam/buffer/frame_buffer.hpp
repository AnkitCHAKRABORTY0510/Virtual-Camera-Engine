// =============================================================================
// frame_buffer.hpp — where decoded frames live between decoding and output
//
// One writer (the loader thread) and one reader (the pacer thread):
//
//   writer:  MutableFrameView slot = begin_write();  ...decode into slot...
//            commit_write(timing);                    // frame becomes visible
//            ...
//            mark_complete();                         // no more frames
//
//   reader:  FrameLease lease = acquire(index);       // by source index
//
// Implementations (see docs/ARCHITECTURE.md §5):
//   RamBuffer          full preload in RAM               (finite sources)
//   DiskBackedBuffer   full preload in a temporary file  (long clips)
//   LiveFrameBuffer    newest few frames only            (live/script sources)
// =============================================================================
#pragma once

#include <cstdint>
#include <optional>
#include <string>

#include "vcam/buffer/frame_lease.hpp"
#include "vcam/buffer/timeline.hpp"
#include "vcam/core/frame.hpp"
#include "vcam/core/status.hpp"

namespace vcam {

struct BufferStats {
    std::string kind;                 // "stream", "ram", "disk", "live"
    uint64_t frames_loaded = 0;       // frames committed so far
    uint64_t frames_expected = 0;     // 0 = unknown
    uint64_t capacity_frames = 0;     // 0 = limited only by the byte budget
    size_t frame_bytes = 0;
    uint64_t memory_bytes = 0;        // RAM held by the buffer
    uint64_t disk_bytes = 0;          // disk space held by the buffer
    bool complete = false;
};

class FrameBuffer {
public:
    virtual ~FrameBuffer() = default;

    // Must be called once before writing. expected_frames is a hint (may be wrong).
    virtual Status configure(const FrameFormat& format, std::optional<uint64_t> expected_frames) = 0;
    virtual const FrameFormat& format() const = 0;

    // ---- writer side (one thread) ----
    // Returns memory for the next frame. Calling it twice without commit
    // returns the same slot (the previous attempt is simply overwritten).
    virtual Result<MutableFrameView> begin_write() = 0;
    // Publishes the slot from begin_write() with its timing.
    virtual Status commit_write(const FrameTiming& timing) = 0;
    // The source has no more frames.
    virtual void mark_complete() = 0;

    // ---- reader side ----
    virtual bool is_complete() const = 0;
    virtual uint64_t frame_count() const = 0;                 // frames committed so far
    virtual FrameLease acquire(uint64_t source_index) const = 0;
    virtual std::optional<uint64_t> newest_index() const = 0;

    // Timestamps of all frames (preload buffers only; nullptr for live buffers).
    // Safe to read once is_complete() is true.
    virtual const Timeline* timeline() const = 0;

    virtual BufferStats stats() const = 0;

    // Drops all frames. Only call while nobody is reading or writing.
    virtual void clear() = 0;
};

}  // namespace vcam
