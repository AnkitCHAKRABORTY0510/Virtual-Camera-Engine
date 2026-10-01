#include "vcam/buffer/live_frame_buffer.hpp"

#include <algorithm>

namespace vcam {

LiveFrameBuffer::LiveFrameBuffer(size_t slot_count) : slot_count_(std::max<size_t>(slot_count, 3)) {}

Status LiveFrameBuffer::configure(const FrameFormat& format, std::optional<uint64_t> /*expected_frames*/) {
    std::lock_guard<std::mutex> lock(mutex_);
    format_ = format;
    slots_.clear();
    slots_.resize(slot_count_);
    for (Slot& slot : slots_) {
        slot.frame = OwnedFrame(format);
    }
    writing_slot_ = -1;
    newest_slot_ = -1;
    committed_ = 0;
    complete_ = false;
    return Status::ok_status();
}

Result<MutableFrameView> LiveFrameBuffer::begin_write() {
    std::lock_guard<std::mutex> lock(mutex_);
    if (slots_.empty()) {
        return Status(StatusCode::Internal, "live buffer: begin_write() before configure()");
    }
    if (writing_slot_ >= 0 && slots_[static_cast<size_t>(writing_slot_)].pins == 0) {
        return slots_[static_cast<size_t>(writing_slot_)].frame.mutable_view();  // reuse unfinished slot
    }
    // Pick a free slot (not the newest frame, not pinned by a reader):
    // an empty one if possible, otherwise the one holding the oldest frame.
    int chosen = -1;
    for (size_t i = 0; i < slots_.size(); ++i) {
        const Slot& slot = slots_[i];
        if (static_cast<int>(i) == newest_slot_ || slot.pins > 0) {
            continue;
        }
        if (!slot.has_frame) {
            chosen = static_cast<int>(i);
            break;
        }
        if (chosen < 0 || slot.timing.source_index < slots_[static_cast<size_t>(chosen)].timing.source_index) {
            chosen = static_cast<int>(i);
        }
    }
    if (chosen < 0) {
        return Status(StatusCode::ResourceExhausted, "live buffer: all slots are in use");
    }
    Slot& slot = slots_[static_cast<size_t>(chosen)];
    slot.has_frame = false;  // invisible to readers while being written
    writing_slot_ = chosen;
    return slot.frame.mutable_view();
}

Status LiveFrameBuffer::commit_write(const FrameTiming& timing) {
    std::lock_guard<std::mutex> lock(mutex_);
    if (writing_slot_ < 0) {
        return Status(StatusCode::Internal, "live buffer: commit_write() without begin_write()");
    }
    Slot& slot = slots_[static_cast<size_t>(writing_slot_)];
    slot.timing = timing;
    slot.timing.source_index = committed_;  // live frames are numbered by arrival
    slot.has_frame = true;
    newest_slot_ = writing_slot_;
    writing_slot_ = -1;
    ++committed_;
    return Status::ok_status();
}

void LiveFrameBuffer::mark_complete() {
    std::lock_guard<std::mutex> lock(mutex_);
    complete_ = true;
}

bool LiveFrameBuffer::is_complete() const {
    std::lock_guard<std::mutex> lock(mutex_);
    return complete_;
}

uint64_t LiveFrameBuffer::frame_count() const {
    std::lock_guard<std::mutex> lock(mutex_);
    return committed_;
}

FrameLease LiveFrameBuffer::acquire(uint64_t source_index) const {
    std::lock_guard<std::mutex> lock(mutex_);
    for (size_t i = 0; i < slots_.size(); ++i) {
        Slot& slot = slots_[i];
        if (slot.has_frame && slot.timing.source_index == source_index) {
            ++slot.pins;
            return FrameLease(slot.frame.view(slot.timing), this, static_cast<uint32_t>(i));
        }
    }
    return FrameLease();  // already overwritten or not yet arrived
}

std::optional<uint64_t> LiveFrameBuffer::newest_index() const {
    std::lock_guard<std::mutex> lock(mutex_);
    if (newest_slot_ < 0) {
        return std::nullopt;
    }
    return slots_[static_cast<size_t>(newest_slot_)].timing.source_index;
}

void LiveFrameBuffer::release_lease(uint32_t token) const {
    std::lock_guard<std::mutex> lock(mutex_);
    if (token < slots_.size() && slots_[token].pins > 0) {
        --slots_[token].pins;
    }
}

BufferStats LiveFrameBuffer::stats() const {
    std::lock_guard<std::mutex> lock(mutex_);
    BufferStats stats;
    stats.kind = "live";
    stats.frames_loaded = committed_;
    stats.capacity_frames = slots_.size();
    stats.frame_bytes = format_.size_bytes;
    stats.memory_bytes = static_cast<uint64_t>(slots_.size()) * format_.size_bytes;
    stats.complete = complete_;
    return stats;
}

void LiveFrameBuffer::clear() {
    std::lock_guard<std::mutex> lock(mutex_);
    for (Slot& slot : slots_) {
        slot.has_frame = false;
        slot.pins = 0;
    }
    writing_slot_ = -1;
    newest_slot_ = -1;
    committed_ = 0;
    complete_ = false;
}

}  // namespace vcam
