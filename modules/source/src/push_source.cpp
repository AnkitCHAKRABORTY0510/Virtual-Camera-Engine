#include "vcam/source/push_source.hpp"

#include <poll.h>        // poll: wait until a descriptor is readable
#include <sys/socket.h>  // socket, bind, listen, accept4, send
#include <sys/stat.h>
#include <sys/un.h>      // sockaddr_un (Unix domain socket address)
#include <unistd.h>

#include <cerrno>
#include <cstring>

#include "vcam/convert/format_converter.hpp"
#include "vcam/core/clock_time.hpp"
#include "vcam/core/log.hpp"

namespace vcam {

namespace {

constexpr const char* kModule = "push";
constexpr size_t kHelloBytes = 16;
constexpr size_t kFrameHeaderBytes = 8;
constexpr int kPollSliceMs = 100;          // how often blocking waits check interrupt()
constexpr int kMidFrameTimeoutMs = 2000;   // a producer stalling inside a frame is dropped

constexpr PixelFormat kCodes[] = {PixelFormat::YUYV,  PixelFormat::UYVY,  PixelFormat::I420, PixelFormat::NV12,
                                  PixelFormat::RGB24, PixelFormat::BGR24, PixelFormat::GRAY8};

uint16_t read_u16(const uint8_t* bytes) {
    return static_cast<uint16_t>(bytes[0] | (bytes[1] << 8));
}

uint32_t read_u32(const uint8_t* bytes) {
    return static_cast<uint32_t>(bytes[0]) | (static_cast<uint32_t>(bytes[1]) << 8) |
           (static_cast<uint32_t>(bytes[2]) << 16) | (static_cast<uint32_t>(bytes[3]) << 24);
}

void write_u32(uint8_t* bytes, uint32_t value) {
    for (int i = 0; i < 4; ++i) {
        bytes[i] = static_cast<uint8_t>((value >> (8 * i)) & 0xFF);
    }
}

}  // namespace

std::optional<PixelFormat> push_code_to_pixel_format(uint16_t code) {
    if (code < std::size(kCodes)) {
        return kCodes[code];
    }
    return std::nullopt;
}

uint16_t pixel_format_to_push_code(PixelFormat format) {
    for (uint16_t code = 0; code < std::size(kCodes); ++code) {
        if (kCodes[code] == format) {
            return code;
        }
    }
    return 0;
}

PushSource::PushSource(std::string socket_path) : socket_path_(std::move(socket_path)) {}

PushSource::~PushSource() {
    close();
}

Status PushSource::listen_socket() {
    sockaddr_un address{};
    if (socket_path_.size() >= sizeof(address.sun_path)) {
        return Status(StatusCode::InvalidArgument, "socket path too long: " + socket_path_);
    }
    // Remove a stale socket left by a previous run — but never another kind of file.
    struct stat existing {};
    if (lstat(socket_path_.c_str(), &existing) == 0) {
        if (!S_ISSOCK(existing.st_mode)) {
            return Status(StatusCode::InvalidArgument, "'" + socket_path_ + "' exists and is not a socket");
        }
        unlink(socket_path_.c_str());
    }

    listen_fd_ = socket(AF_UNIX, SOCK_STREAM | SOCK_CLOEXEC, 0);
    if (listen_fd_ < 0) {
        return Status(StatusCode::IoError, std::string("socket(): ") + std::strerror(errno));
    }
    address.sun_family = AF_UNIX;
    std::memcpy(address.sun_path, socket_path_.c_str(), socket_path_.size() + 1);
    if (bind(listen_fd_, reinterpret_cast<sockaddr*>(&address), sizeof(address)) != 0) {
        Status error(StatusCode::IoError, "cannot create socket '" + socket_path_ + "': " + std::strerror(errno));
        ::close(listen_fd_);
        listen_fd_ = -1;
        return error;
    }
    chmod(socket_path_.c_str(), 0600);  // only the same user may push frames
    listen(listen_fd_, 1);
    return Status::ok_status();
}

Status PushSource::open() {
    close();
    interrupted_.store(false);
    info_ = SourceInfo{};
    stats_ = SourceStats{};
    next_index_ = 0;
    first_frame_ns_ = -1;

    Status status = listen_socket();
    if (!status.ok()) {
        return status;
    }
    VCAM_INFO(kModule, "waiting for a frame producer on " << socket_path_
                                                          << " (e.g. python3 tools/examples/push_frames.py)");
    while (!interrupted_.load()) {
        bool connected = false;
        status = accept_producer(kPollSliceMs, connected);
        if (!status.ok()) {
            return status;
        }
        if (connected) {
            return Status::ok_status();
        }
    }
    return Status(StatusCode::Cancelled, "stopped while waiting for a producer");
}

PushSource::IoResult PushSource::read_exact(int fd, uint8_t* data, size_t size, int idle_timeout_ms) {
    size_t received = 0;
    int idle_ms = 0;
    while (received < size) {
        if (interrupted_.load()) {
            return IoResult::Interrupted;
        }
        pollfd waiter{fd, POLLIN, 0};
        const int ready = poll(&waiter, 1, kPollSliceMs);
        if (ready == 0) {
            idle_ms += kPollSliceMs;
            if (idle_ms >= idle_timeout_ms) {
                return IoResult::Timeout;
            }
            continue;
        }
        if (ready < 0) {
            if (errno == EINTR) continue;
            return IoResult::Closed;
        }
        const ssize_t n = ::read(fd, data + received, size - received);
        if (n == 0) {
            return IoResult::Closed;  // producer disconnected
        }
        if (n < 0) {
            if (errno == EINTR || errno == EAGAIN) continue;
            return IoResult::Closed;
        }
        received += static_cast<size_t>(n);
        idle_ms = 0;
    }
    return IoResult::Done;
}

void PushSource::send_reply(int fd, bool accepted, const std::string& message, uint32_t frame_bytes) {
    std::string reply(accepted ? std::string("OK\0\0", 4) : std::string("ER\0\0", 4));
    uint8_t number[4];
    write_u32(number, accepted ? frame_bytes : static_cast<uint32_t>(message.size()));
    reply.append(reinterpret_cast<const char*>(number), 4);
    if (!accepted) {
        reply += message;
    }
    // MSG_NOSIGNAL: if the client already left, return an error instead of
    // killing the whole engine with SIGPIPE.
    send(fd, reply.data(), reply.size(), MSG_NOSIGNAL);
}

Status PushSource::accept_producer(int timeout_ms, bool& connected) {
    connected = false;
    pollfd waiter{listen_fd_, POLLIN, 0};
    if (poll(&waiter, 1, timeout_ms) <= 0) {
        return Status::ok_status();  // nobody yet
    }
    const int fd = accept4(listen_fd_, nullptr, nullptr, SOCK_CLOEXEC);
    if (fd < 0) {
        return Status::ok_status();
    }

    auto reject = [&](const std::string& message) {
        VCAM_WARN(kModule, "producer rejected: " << message);
        send_reply(fd, false, message, 0);
        ::close(fd);
        return Status::ok_status();
    };

    uint8_t hello[kHelloBytes];
    if (read_exact(fd, hello, sizeof(hello), kMidFrameTimeoutMs) != IoResult::Done) {
        ::close(fd);
        return Status::ok_status();
    }
    if (std::memcmp(hello, "VCAM", 4) != 0) {
        return reject("bad HELLO (expected magic 'VCAM')");
    }
    if (read_u16(hello + 4) != kPushProtocolVersion) {
        return reject("unsupported protocol version " + std::to_string(read_u16(hello + 4)));
    }
    const std::optional<PixelFormat> pixel_format = push_code_to_pixel_format(read_u16(hello + 6));
    if (!pixel_format) {
        return reject("unknown pixel format code " + std::to_string(read_u16(hello + 6)));
    }
    const auto width = static_cast<int>(read_u32(hello + 8));
    const auto height = static_cast<int>(read_u32(hello + 12));
    Result<FrameFormat> format = make_frame_format(width, height, *pixel_format, ColorSpace::BT601, ColorRange::Limited);
    if (!format.ok()) {
        return reject(format.status().message());
    }
    if (info_.width != 0 && (width != info_.width || height != info_.height)) {
        return reject("camera is " + std::to_string(info_.width) + "x" + std::to_string(info_.height) +
                      "; producers must keep that size");
    }

    client_format_ = format.value();
    if (scratch_.empty() || !same_layout(scratch_.format(), client_format_)) {
        scratch_ = OwnedFrame(client_format_);
    }
    if (info_.width == 0) {
        // The first producer defines the source.
        info_.uri = socket_path_;
        info_.container_name = "unix socket";
        info_.codec_name = "raw";
        info_.native_pixel_format = pixel_format_name(*pixel_format);
        info_.width = width;
        info_.height = height;
        info_.time_base = make_rational(1, kNanosPerSecond);  // timestamps = arrival time in ns
        info_.is_live = true;
        info_.is_seekable = false;
    }
    send_reply(fd, true, "", static_cast<uint32_t>(client_format_.size_bytes));
    client_fd_ = fd;
    ++connections_;
    connected = true;
    VCAM_INFO(kModule, "producer connected: " << describe(client_format_));
    return Status::ok_status();
}

void PushSource::drop_producer(const char* reason) {
    if (client_fd_ >= 0) {
        VCAM_INFO(kModule, "producer disconnected (" << reason << "); showing the last frame until one reconnects");
        ::close(client_fd_);
        client_fd_ = -1;
    }
}

Status PushSource::set_output_format(const FrameFormat& format) {
    if (format.width != info_.width || format.height != info_.height) {
        return Status(StatusCode::InvalidArgument, "output size must equal the producer's frame size");
    }
    converter_ = std::make_unique<FormatConverter>(format);
    return Status::ok_status();
}

ReadResult PushSource::read_next(MutableFrameView destination, FrameTiming& timing) {
    if (!converter_) {
        return ReadResult::error(Status(StatusCode::InvalidArgument, "set_output_format() not called"));
    }
    if (interrupted_.load()) {
        return ReadResult::again();
    }
    if (client_fd_ < 0) {
        bool connected = false;
        Status status = accept_producer(kPollSliceMs, connected);
        if (!status.ok()) {
            return ReadResult::error(status);
        }
        return ReadResult::again();
    }

    // Wait briefly for the next frame header; no data within 100 ms = Again.
    pollfd waiter{client_fd_, POLLIN, 0};
    if (poll(&waiter, 1, kPollSliceMs) <= 0) {
        return ReadResult::again();
    }

    uint8_t header[kFrameHeaderBytes];
    IoResult result = read_exact(client_fd_, header, sizeof(header), kMidFrameTimeoutMs);
    if (result != IoResult::Done) {
        drop_producer(result == IoResult::Closed ? "connection closed" : "timeout");
        return ReadResult::again();
    }
    if (std::memcmp(header, "FRAM", 4) != 0 || read_u32(header + 4) != client_format_.size_bytes) {
        ++stats_.decode_errors;
        drop_producer("protocol error: expected 'FRAM' + frame size");
        return ReadResult::again();
    }
    MutableFrameView scratch = scratch_.mutable_view();
    result = read_exact(client_fd_, scratch.bytes.data(), scratch.bytes.size(), kMidFrameTimeoutMs);
    if (result != IoResult::Done) {
        drop_producer(result == IoResult::Closed ? "closed in the middle of a frame" : "stalled mid-frame");
        return ReadResult::again();
    }

    Status converted = converter_->convert(scratch_.view(), destination);
    if (!converted.ok()) {
        return ReadResult::error(converted);
    }
    const int64_t now = monotonic_now_ns();
    if (first_frame_ns_ < 0) {
        first_frame_ns_ = now;
    }
    timing.source_index = next_index_++;
    timing.pts_ticks = now - first_frame_ns_;
    timing.duration_ticks = 0;
    ++stats_.frames_decoded;
    return ReadResult::frame();
}

void PushSource::close() {
    converter_.reset();
    if (client_fd_ >= 0) {
        ::close(client_fd_);
        client_fd_ = -1;
    }
    if (listen_fd_ >= 0) {
        ::close(listen_fd_);
        listen_fd_ = -1;
        unlink(socket_path_.c_str());
    }
}

}  // namespace vcam
