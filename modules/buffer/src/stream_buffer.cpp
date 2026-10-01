#include "vcam/buffer/stream_buffer.hpp"

#include <algorithm>
#include <chrono>

namespace vcam {

StreamBuffer::StreamBuffer(size_t capacity_frames) : capacity_(std::max<size_t>(capacity_frames, 4)) {}

Status StreamBuffer::configure(const FrameFormat& format, std::optional<uint64_t> /*expected_frames*/) {
    std::lock_guard<std::mutex> lock(mutex_);
    format_ = format;
    slots_.clear();
    slots_.resize(capacity_);
    for (Slot& slot : slots_) {
        slot.frame = OwnedFrame(format);  // all memory allocated once, up front
    }
    head_ = tail_ = valid_from_ = 0;
    shown_.reset();
    committed_total_ = 0;
    complete_ = false;
    return Status::ok_status();
}

// ---------------------------------------------------------------------------
// Writer
// ---------------------------------------------------------------------------

bool StreamBuffer::wait_for_space(int timeout_ms) {
    std::unique_lock<std::mutex> lock(mutex_);
    // condition_variable::wait_for sleeps until notified AND the condition is
    // true, or until the timeout: no CPU is used while the window is full.
    return space_available_.wait_for(lock, std::chrono::milliseconds(timeout_ms),
                                     [this] { return tail_ - head_ < capacity_; });
}

Result<MutableFrameView> StreamBuffer::begin_write() {
    std::lock_guard<std::mutex> lock(mutex_);
    if (slots_.empty()) {
        return Status(StatusCode::Internal, "stream buffer: begin_write() before configure()");
    }
    if (tail_ - head_ >= capacity_) {
        return Status(StatusCode::ResourceExhausted, "stream buffer full");
    }
    // Slot `tail_` is outside [head_, tail_): nobody reads it.
    return slot_for(tail_).frame.mutable_view();
}

Status StreamBuffer::commit_write(const FrameTiming& timing) {
    std::lock_guard<std::mutex> lock(mutex_);
    if (tail_ - head_ >= capacity_) {
        return Status(StatusCode::Internal, "stream buffer: commit without space");
    }
    Slot& slot = slot_for(tail_);
    slot.timing = timing;
    // timing.source_index keeps the source's own frame number (position in
    // the video); the buffer's sequence number is reported by pick().
    slot.pass = writer_pass_;
    ++tail_;
    ++committed_total_;
    return Status::ok_status();
}

void StreamBuffer::mark_complete() {
    std::lock_guard<std::mutex> lock(mutex_);
    complete_ = true;
}

void StreamBuffer::set_pass(uint64_t pass) {
    std::lock_guard<std::mutex> lock(mutex_);
    writer_pass_ = pass;
}

void StreamBuffer::restart() {
    std::lock_guard<std::mutex> lock(mutex_);
    // Keep only the frame on screen (the reader may be copying it right now).
    // Every frame after it is discarded by moving `tail_` back, which also
    // returns their slots to the writer immediately.
    if (shown_) {
        head_ = *shown_;
        tail_ = *shown_ + 1;
    } else {
        head_ = tail_;
    }
    valid_from_ = tail_;  // the shown frame is not a candidate any more
    complete_ = false;
    space_available_.notify_all();
}

// ---------------------------------------------------------------------------
// Reader
// ---------------------------------------------------------------------------

FrameLease StreamBuffer::lease_for(uint64_t sequence) const {
    const Slot& slot = slot_for(sequence);
    // No owner needed: a frame in [head_, tail_) is never overwritten, and the
    // shown frame stays at head_ until the next pick() by the same thread.
    return FrameLease(slot.frame.view(slot.timing), nullptr, 0);
}

StreamPick StreamBuffer::pick(int64_t source_ticks) {
    StreamPick result;
    std::lock_guard<std::mutex> lock(mutex_);

    const uint64_t first = std::max(head_, valid_from_);
    std::optional<uint64_t> chosen;
    // Candidates are sorted by pts: walk forward while the frame has started.
    // The window is small (≈ 30 frames), so a linear walk is cheap.
    for (uint64_t sequence = first; sequence < tail_; ++sequence) {
        if (slot_for(sequence).timing.pts_ticks <= source_ticks) {
            chosen = sequence;
        } else {
            break;
        }
    }

    if (!chosen) {
        if (first < tail_ && !shown_) {
            chosen = first;  // very first frame: show it even if it starts slightly later
        } else {
            // Nothing to show yet: before the first frame, or right after a
            // seek (restart) while the decoder catches up. Keep the current frame.
            // (A decoder that is merely late is detected below, as underflow.)
            result.waiting = true;
            if (shown_) {
                result.lease = lease_for(*shown_);
                result.sequence = *shown_;
                result.pass = slot_for(*shown_).pass;
            }
            return result;
        }
    }

    const uint64_t sequence = *chosen;
    const FrameTiming& timing = slot_for(sequence).timing;
    result.new_frame = !shown_ || *shown_ != sequence;
    if (shown_ && sequence > *shown_ + 1 && *shown_ >= valid_from_) {
        result.skipped = static_cast<uint32_t>(sequence - *shown_ - 1);
    }

    // Is the frame AFTER this one already due, but not decoded yet?
    const int64_t frame_end = timing.pts_ticks + std::max<int64_t>(timing.duration_ticks, 1);
    const bool newest = sequence + 1 == tail_;
    if (newest && source_ticks >= frame_end) {
        if (complete_) {
            result.past_end = true;
        } else {
            result.underflow = true;
        }
    }

    // Everything older than the chosen frame is no longer needed.
    if (head_ != sequence) {
        head_ = sequence;
        space_available_.notify_one();
    }
    shown_ = sequence;
    result.sequence = sequence;
    result.pass = slot_for(sequence).pass;
    result.lease = lease_for(sequence);
    return result;
}

size_t StreamBuffer::buffered_frames() const {
    std::lock_guard<std::mutex> lock(mutex_);
    const uint64_t first = std::max(head_, valid_from_);
    return tail_ > first ? static_cast<size_t>(tail_ - first) : 0;
}

bool StreamBuffer::is_full() const {
    std::lock_guard<std::mutex> lock(mutex_);
    return tail_ - head_ >= capacity_;
}

// ---------------------------------------------------------------------------
// FrameBuffer interface
// ---------------------------------------------------------------------------

bool StreamBuffer::is_complete() const {
    std::lock_guard<std::mutex> lock(mutex_);
    return complete_;
}

uint64_t StreamBuffer::frame_count() const {
    std::lock_guard<std::mutex> lock(mutex_);
    return committed_total_;
}

FrameLease StreamBuffer::acquire(uint64_t sequence) const {
    std::lock_guard<std::mutex> lock(mutex_);
    if (sequence < head_ || sequence >= tail_) {
        return FrameLease();
    }
    return lease_for(sequence);
}

std::optional<uint64_t> StreamBuffer::newest_index() const {
    std::lock_guard<std::mutex> lock(mutex_);
    if (tail_ == 0) {
        return std::nullopt;
    }
    return tail_ - 1;
}

BufferStats StreamBuffer::stats() const {
    std::lock_guard<std::mutex> lock(mutex_);
    BufferStats stats;
    stats.kind = "stream";
    const uint64_t first = std::max(head_, valid_from_);
    stats.frames_loaded = tail_ > first ? tail_ - first : 0;  // decoded, waiting to be shown
    stats.capacity_frames = capacity_;
    stats.frame_bytes = format_.size_bytes;
    stats.memory_bytes = static_cast<uint64_t>(capacity_) * ((format_.size_bytes + 63) / 64 * 64);
    stats.complete = complete_;
    return stats;
}

void StreamBuffer::clear() {
    std::lock_guard<std::mutex> lock(mutex_);
    head_ = tail_ = valid_from_ = 0;
    shown_.reset();
    committed_total_ = 0;
    complete_ = false;
    writer_pass_ = 0;
    space_available_.notify_all();
}

}  // namespace vcam
