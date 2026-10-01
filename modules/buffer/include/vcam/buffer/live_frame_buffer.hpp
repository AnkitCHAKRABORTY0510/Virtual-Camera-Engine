// =============================================================================
// live_frame_buffer.hpp — buffer for live sources (scripts, generators)
//
// A live source produces frames at its own pace; the camera must always show
// the NEWEST frame at each output slot (like a real sensor). Old frames are
// simply overwritten.
//
// Implementation: a few slots (default 3 = "triple buffering"):
//   * the writer always fills a slot that is neither the newest frame nor
//     pinned by a reader, so the reader always has a complete newest frame;
//   * a reader pins the slot it is publishing (FrameLease), so it can never
//     be overwritten mid-copy.
// A mutex guards only the tiny slot bookkeeping; pixel copies happen outside it.
// =============================================================================
#pragma once

#include <cstdint>
#include <mutex>
#include <vector>

#include "vcam/buffer/frame_buffer.hpp"

namespace vcam {

class LiveFrameBuffer final : public FrameBuffer, private LeaseOwner {
public:
    explicit LiveFrameBuffer(size_t slot_count = 3);

    Status configure(const FrameFormat& format, std::optional<uint64_t> expected_frames) override;
    const FrameFormat& format() const override { return format_; }

    Result<MutableFrameView> begin_write() override;
    Status commit_write(const FrameTiming& timing) override;
    void mark_complete() override;

    bool is_complete() const override;
    uint64_t frame_count() const override;
    FrameLease acquire(uint64_t source_index) const override;
    std::optional<uint64_t> newest_index() const override;
    const Timeline* timeline() const override { return nullptr; }

    BufferStats stats() const override;
    void clear() override;

private:
    struct Slot {
        OwnedFrame frame;
        FrameTiming timing{};
        bool has_frame = false;   // holds a committed frame
        int pins = 0;             // readers currently using it
    };

    void release_lease(uint32_t token) const override;

    size_t slot_count_;
    FrameFormat format_{};

    mutable std::mutex mutex_;
    mutable std::vector<Slot> slots_;
    int writing_slot_ = -1;       // slot handed out by begin_write()
    int newest_slot_ = -1;        // slot with the newest committed frame
    uint64_t committed_ = 0;
    bool complete_ = false;
};

}  // namespace vcam
