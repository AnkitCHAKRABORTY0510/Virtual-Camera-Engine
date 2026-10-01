// =============================================================================
// v4l2_loopback_camera.hpp — the real virtual camera: a v4l2loopback device
//
// v4l2loopback (kernel module) creates /dev/videoN devices that behave like
// webcams for applications. We are the "producer": we open the device for
// writing, declare the format with VIDIOC_S_FMT and the frame rate with
// VIDIOC_S_PARM, then write() one frame per output slot. Applications open
// the same device as a normal camera and receive our frames.
//
// Producer timing = camera timing: consumers block until we write, and
// v4l2loopback never makes the writer wait for slow readers (they just miss
// frames), so a slow application cannot disturb our clock.
//
// Setup (once, as root): tools/setup_loopback.sh — see docs/ARCHITECTURE.md §3.
// =============================================================================
#pragma once

#include <cstdint>
#include <optional>
#include <string>

#include "vcam/output/virtual_camera.hpp"

namespace vcam {

// What VIDIOC_QUERYCAP reports about a device.
struct V4L2DeviceInfo {
    std::string path;
    std::string driver;    // "v4l2 loopback" for v4l2loopback
    std::string card;      // the name applications show (card_label)
    std::string bus_info;
    uint32_t capabilities = 0;
    bool is_loopback = false;
    bool can_output = false;  // accepts frames from a producer right now
};

// V4L2 fourcc code for a pixel format, e.g. YUYV -> 'YUYV'.
uint32_t v4l2_fourcc_for(PixelFormat format);
// Inverse of v4l2_fourcc_for (nullopt for formats the engine does not use).
std::optional<PixelFormat> pixel_format_for_fourcc(uint32_t fourcc);
// 'YUYV' -> "YUYV"
std::string fourcc_to_string(uint32_t fourcc);

class V4L2LoopbackCamera final : public VirtualCamera {
public:
    explicit V4L2LoopbackCamera(std::string device_path) : path_(std::move(device_path)) {}
    ~V4L2LoopbackCamera() override { close(); }

    // Checks that `path` is a usable v4l2loopback output device WITHOUT
    // keeping it open. Gives precise errors (missing module, permissions,
    // a real webcam, busy). Called early, before buffering (fail fast).
    static Result<V4L2DeviceInfo> probe(const std::string& path);

    Status open(const FrameFormat& format, Rational fps) override;
    PublishOutcome publish(const FrameView& frame) override;
    void close() override;
    bool is_open() const override { return fd_ >= 0; }
    std::string description() const override;

    const V4L2DeviceInfo& device_info() const { return info_; }

private:
    std::string path_;
    int fd_ = -1;
    V4L2DeviceInfo info_;
    size_t frame_bytes_ = 0;
};

}  // namespace vcam
