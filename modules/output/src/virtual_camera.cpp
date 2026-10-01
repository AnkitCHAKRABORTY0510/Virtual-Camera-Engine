#include "vcam/output/virtual_camera.hpp"

#include <fcntl.h>
#include <unistd.h>

#include <cerrno>
#include <cstring>

namespace vcam {

// ---- NullCamera ------------------------------------------------------------------

Status NullCamera::open(const FrameFormat& /*format*/, Rational /*fps*/) {
    open_ = true;
    frames_ = 0;
    return Status::ok_status();
}

PublishOutcome NullCamera::publish(const FrameView& /*frame*/) {
    ++frames_;
    return PublishOutcome::ok();
}

// ---- RawFileCamera ---------------------------------------------------------------

Status RawFileCamera::open(const FrameFormat& format, Rational /*fps*/) {
    close();
    // O_TRUNC: start a fresh file; 0644 = owner read/write, others read.
    fd_ = ::open(path_.c_str(), O_WRONLY | O_CREAT | O_TRUNC | O_CLOEXEC, 0644);
    if (fd_ < 0) {
        return Status(StatusCode::IoError, "cannot create '" + path_ + "': " + std::strerror(errno));
    }
    frame_bytes_ = format.size_bytes;
    return Status::ok_status();
}

PublishOutcome RawFileCamera::publish(const FrameView& frame) {
    PublishOutcome outcome;
    size_t written = 0;
    // write() may write less than requested; loop until the whole frame is out.
    while (written < frame.bytes.size()) {
        const ssize_t result = ::write(fd_, frame.bytes.data() + written, frame.bytes.size() - written);
        if (result < 0) {
            if (errno == EINTR) {
                continue;
            }
            outcome.kind = PublishOutcome::Kind::Error;
            outcome.fatal = true;
            outcome.status = Status(StatusCode::IoError, "writing '" + path_ + "': " + std::strerror(errno));
            return outcome;
        }
        written += static_cast<size_t>(result);
    }
    return outcome;
}

void RawFileCamera::close() {
    if (fd_ >= 0) {
        ::close(fd_);
        fd_ = -1;
    }
}

}  // namespace vcam
