// =============================================================================
// stream_buffer.hpp — low-memory "decode just ahead" buffer (default mode)
//
// Instead of decoding the whole video before streaming (RamBuffer /
// DiskBackedBuffer), the loader thread keeps only a short window of decoded
// frames AHEAD of what the camera is showing:
//
//      shown         read-ahead window (e.g. 1 s)            next to decode
//        |<-------------------------------------------------->|
//   ... [s] [s+1] [s+2] ...                         [tail-1]   [tail]
//        ^ head (oldest frame still needed)
//
// * Memory = capacity × frame size (≈ 20 MB for 30 frames of 800×410 YUYV),
//   independent of the video's length. No temporary file.
// * The writer (loader) blocks in wait_for_space() when the window is full,
//   so it decodes at exactly the camera's pace and uses no CPU otherwise.
// * The reader (pacer) never blocks: pick(time) returns the frame on screen at
//   that source time and frees every older frame. If the decoder ever falls
//   behind, pick() repeats the last frame and reports `underflow` — the camera
//   keeps its frame rate no matter what.
//
// Timestamps written into this buffer are "unwrapped": when the loader loops
// the video it adds the video's duration, so source time only ever increases
// and the reader needs no special loop logic.
//
// Thread safety: one writer thread, one reader thread; a mutex guards only the
// small index bookkeeping. Pixels are decoded into a slot outside the lock;
// the slot being shown is never written (the writer stops at `head`).
// =============================================================================
#pragma once

#include <condition_variable>
#include <cstdint>
#include <mutex>
#include <optional>
#include <vector>

#include "vcam/buffer/frame_buffer.hpp"

namespace vcam {

// Result of StreamBuffer::pick().
struct StreamPick {
    FrameLease lease;            // frame to show (invalid only before the very first frame)
    uint64_t sequence = 0;       // buffer sequence number of that frame
    bool new_frame = false;      // differs from the previously picked frame
    uint32_t skipped = 0;        // decoded frames jumped over since the previous pick
    bool underflow = false;      // the frame due now is not decoded yet: previous one repeated
    bool waiting = false;        // nothing new available (e.g. right after a seek)
    bool past_end = false;       // source finished and time is beyond the last frame
    uint64_t pass = 0;           // loop pass of that frame (see set_pass)
};

class StreamBuffer final : public FrameBuffer {
public:
    explicit StreamBuffer(size_t capacity_frames);

    Status configure(const FrameFormat& format, std::optional<uint64_t> expected_frames) override;
    const FrameFormat& format() const override { return format_; }

    // ---- writer (loader thread) ----
    // Waits up to `timeout_ms` for a free slot. True = begin_write() will succeed.
    bool wait_for_space(int timeout_ms);
    Result<MutableFrameView> begin_write() override;   // ResourceExhausted when full
    Status commit_write(const FrameTiming& timing) override;  // pts must increase (unwrapped)
    void mark_complete() override;                     // source ended (no looping)
    // Frames committed from now on belong to loop pass `pass` (0 = first time
    // through the video). The reader sees it in StreamPick::pass, so it can tell
    // exactly when the loop seam reaches the screen (the writer is ahead).
    void set_pass(uint64_t pass);
    // Drops all buffered frames (after a seek). The frame currently shown stays
    // valid until the reader picks a new one. Also clears "complete".
    void restart();

    // ---- reader (pacer thread) ----
    // The frame on screen at `source_ticks` (unwrapped ticks of the source time base).
    StreamPick pick(int64_t source_ticks);

    size_t buffered_frames() const;  // frames decoded and not yet shown/dropped
    size_t capacity() const { return capacity_; }
    bool is_full() const;

    // ---- FrameBuffer interface ----
    bool is_complete() const override;
    uint64_t frame_count() const override;  // total frames ever committed
    FrameLease acquire(uint64_t sequence) const override;
    std::optional<uint64_t> newest_index() const override;
    const Timeline* timeline() const override { return nullptr; }
    BufferStats stats() const override;
    void clear() override;

private:
    struct Slot {
        OwnedFrame frame;
        FrameTiming timing{};
        uint64_t pass = 0;
    };
    Slot& slot_for(uint64_t sequence) { return slots_[sequence % capacity_]; }
    const Slot& slot_for(uint64_t sequence) const { return slots_[sequence % capacity_]; }
    FrameLease lease_for(uint64_t sequence) const;

    size_t capacity_;
    FrameFormat format_{};
    std::vector<Slot> slots_;

    mutable std::mutex mutex_;
    std::condition_variable space_available_;
    uint64_t head_ = 0;          // oldest sequence still occupying a slot
    uint64_t tail_ = 0;          // next sequence to write
    uint64_t valid_from_ = 0;    // frames before this were dropped by restart()
    std::optional<uint64_t> shown_;  // sequence currently on screen
    uint64_t committed_total_ = 0;
    bool complete_ = false;
    uint64_t writer_pass_ = 0;
};

}  // namespace vcam
