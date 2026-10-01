// =============================================================================
// virtual_camera.hpp — HOW frames reach Linux applications
//
// The pacer calls publish() exactly once per output slot. A backend must not
// care where frames came from (file, images, script) — it only sees a
// FrameView in the configured format.
//
// Backends:
//   V4L2LoopbackCamera  writes to a v4l2loopback device (/dev/videoN) — the real camera
//   NullCamera          discards frames (timing benchmarks, tests)
//   RawFileCamera       appends raw frames to a file (verification, debugging)
// =============================================================================
#pragma once

#include <cstdint>
#include <memory>
#include <string>

#include "vcam/core/frame.hpp"
#include "vcam/core/rational.hpp"
#include "vcam/core/status.hpp"

namespace vcam {

struct PublishOutcome {
    enum class Kind {
        Ok,            // frame delivered
        Backpressure,  // device temporarily refused (EAGAIN); frame dropped, try next slot
        Error,         // write failed
    };
    Kind kind = Kind::Ok;
    bool fatal = false;  // the device is gone (e.g. module unloaded): stop streaming
    Status status;

    static PublishOutcome ok() { return {}; }
};

class VirtualCamera {
public:
    virtual ~VirtualCamera() = default;

    // Prepares the output for frames of `format` at `fps`.
    virtual Status open(const FrameFormat& format, Rational fps) = 0;

    // Delivers one frame (format must equal the one given to open()).
    // Must never block for long: the timing thread calls it.
    virtual PublishOutcome publish(const FrameView& frame) = 0;

    virtual void close() = 0;
    virtual bool is_open() const = 0;

    // Human-readable target, e.g. "/dev/video10 ('Virtual Camera Engine', v4l2 loopback)".
    virtual std::string description() const = 0;
};

class NullCamera final : public VirtualCamera {
public:
    Status open(const FrameFormat& format, Rational fps) override;
    PublishOutcome publish(const FrameView& frame) override;
    void close() override { open_ = false; }
    bool is_open() const override { return open_; }
    std::string description() const override { return "null output (frames discarded)"; }
    uint64_t frames_published() const { return frames_; }

private:
    bool open_ = false;
    uint64_t frames_ = 0;
};

class RawFileCamera final : public VirtualCamera {
public:
    explicit RawFileCamera(std::string path) : path_(std::move(path)) {}
    ~RawFileCamera() override { close(); }

    Status open(const FrameFormat& format, Rational fps) override;
    PublishOutcome publish(const FrameView& frame) override;
    void close() override;
    bool is_open() const override { return fd_ >= 0; }
    std::string description() const override { return "raw file " + path_; }

private:
    std::string path_;
    int fd_ = -1;
    size_t frame_bytes_ = 0;
};

}  // namespace vcam
