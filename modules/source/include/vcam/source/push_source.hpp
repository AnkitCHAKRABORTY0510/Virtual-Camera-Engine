// =============================================================================
// push_source.hpp — frames pushed by another program (script input, Phase 8)
//
// A producer (Python script, C/C++ program, ...) connects to a Unix domain
// socket and sends raw frames. The ENGINE stays in charge of camera timing:
// at each output slot it shows the newest frame that has arrived. A slow
// producer causes repeated frames, a fast one causes skipped frames — both
// are counted, neither disturbs the camera clock.
//
// Wire protocol (all integers little-endian), see docs/SCRIPT_INPUT.md:
//
//   client -> engine  HELLO  "VCAM" u16 version=1  u16 pixel_format  u32 width  u32 height   (16 bytes)
//   engine -> client  "OK\0\0" u32 frame_bytes            (accepted)
//                  or "ER\0\0" u32 length  <message>      (rejected; connection closed)
//   client -> engine  FRAME  "FRAM" u32 length  <length bytes of pixels>   (repeated)
//
//   pixel_format: 0 yuyv, 1 uyvy, 2 i420, 3 nv12, 4 rgb24, 5 bgr24, 6 gray8
//
// The first producer defines the camera resolution. Producers may disconnect
// and reconnect (the camera keeps showing the last frame meanwhile); a later
// producer must use the same width and height.
//
// Why a socket and not shared memory (the original proposal)? Same
// interface, far simpler client code (Python needs only the standard
// library), and a Unix socket moves several GB/s, far above the ~250 MB/s of
// 1080p60. Shared memory can be added behind the same API if ever needed.
// =============================================================================
#pragma once

#include <atomic>
#include <cstdint>
#include <memory>
#include <optional>
#include <string>

#include "vcam/source/frame_source.hpp"

namespace vcam {

class FormatConverter;

constexpr uint16_t kPushProtocolVersion = 1;

// Protocol code <-> pixel format.
std::optional<PixelFormat> push_code_to_pixel_format(uint16_t code);
uint16_t pixel_format_to_push_code(PixelFormat format);

class PushSource final : public FrameSource {
public:
    explicit PushSource(std::string socket_path);
    ~PushSource() override;

    // Creates the socket and WAITS for the first producer's HELLO (it defines
    // the resolution). Returns Cancelled if interrupt() is called meanwhile.
    Status open() override;
    const SourceInfo& info() const override { return info_; }
    Status set_output_format(const FrameFormat& format) override;

    // Returns Frame when a complete frame arrived, Again after ~100 ms without one.
    ReadResult read_next(MutableFrameView destination, FrameTiming& timing) override;
    SourceStats stats() const override { return stats_; }
    void interrupt() override { interrupted_.store(true); }
    void close() override;

    const std::string& socket_path() const { return socket_path_; }
    uint64_t connections() const { return connections_; }

private:
    enum class IoResult { Done, Timeout, Closed, Interrupted };

    Status listen_socket();
    // Waits up to `timeout_ms` for a producer; performs the HELLO handshake.
    // Returns ok + connected, ok + not connected (timeout), or an error.
    Status accept_producer(int timeout_ms, bool& connected);
    IoResult read_exact(int fd, uint8_t* data, size_t size, int idle_timeout_ms);
    void send_reply(int fd, bool accepted, const std::string& message, uint32_t frame_bytes);
    void drop_producer(const char* reason);

    std::string socket_path_;
    SourceInfo info_;
    SourceStats stats_;
    std::atomic<bool> interrupted_{false};

    int listen_fd_ = -1;
    int client_fd_ = -1;
    FrameFormat client_format_{};  // format the producer sends
    OwnedFrame scratch_;           // receives one frame before conversion
    std::unique_ptr<FormatConverter> converter_;
    uint64_t next_index_ = 0;
    uint64_t connections_ = 0;
    int64_t first_frame_ns_ = -1;
};

}  // namespace vcam
