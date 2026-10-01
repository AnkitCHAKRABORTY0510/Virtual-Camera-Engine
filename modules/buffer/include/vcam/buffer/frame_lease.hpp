// =============================================================================
// frame_lease.hpp — temporary, safe read access to one buffered frame
//
// A FrameLease is a FrameView plus a promise: while the lease exists, the
// buffer will not overwrite that frame's memory. When the lease is destroyed
// it tells the buffer "I'm done" (RAII). For fully preloaded buffers nothing
// is ever overwritten, so their leases have no owner and cost nothing.
//
//     FrameLease lease = buffer.acquire(42);
//     if (lease) camera.publish(lease.view());   // memory guaranteed valid here
//     // lease released at end of scope
// =============================================================================
#pragma once

#include <cstdint>
#include <utility>

#include "vcam/core/frame.hpp"

namespace vcam {

// Implemented by buffers whose frames can be overwritten (e.g. the live buffer).
class LeaseOwner {
public:
    virtual ~LeaseOwner() = default;
    virtual void release_lease(uint32_t token) const = 0;
};

class FrameLease {
public:
    FrameLease() = default;  // invalid lease: "frame not available"
    FrameLease(FrameView view, const LeaseOwner* owner, uint32_t token)
        : view_(std::move(view)), owner_(owner), token_(token), valid_(true) {}

    ~FrameLease() { reset(); }

    // Move-only: exactly one object is responsible for releasing.
    FrameLease(FrameLease&& other) noexcept { *this = std::move(other); }
    FrameLease& operator=(FrameLease&& other) noexcept {
        if (this != &other) {
            reset();
            view_ = other.view_;
            owner_ = other.owner_;
            token_ = other.token_;
            valid_ = other.valid_;
            other.owner_ = nullptr;
            other.valid_ = false;
        }
        return *this;
    }
    FrameLease(const FrameLease&) = delete;
    FrameLease& operator=(const FrameLease&) = delete;

    explicit operator bool() const { return valid_; }
    const FrameView& view() const { return view_; }

    void reset() {
        if (owner_ != nullptr) {
            owner_->release_lease(token_);
            owner_ = nullptr;
        }
        valid_ = false;
    }

private:
    FrameView view_{};
    const LeaseOwner* owner_ = nullptr;
    uint32_t token_ = 0;
    bool valid_ = false;
};

}  // namespace vcam
