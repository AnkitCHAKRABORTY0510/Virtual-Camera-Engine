#include "vcam/output/v4l2_loopback_camera.hpp"

#include <fcntl.h>
#include <linux/videodev2.h>  // V4L2 structures and ioctl numbers (kernel UAPI header)
#include <sys/ioctl.h>
#include <sys/stat.h>
#include <unistd.h>

#include <cerrno>
#include <cstring>

#include "vcam/core/log.hpp"

namespace vcam {

namespace {

constexpr const char* kModule = "v4l2";

// ioctl() can be interrupted by a signal (EINTR); retry in that case.
int xioctl(int fd, unsigned long request, void* argument) {
    int result = 0;
    do {
        result = ioctl(fd, request, argument);
    } while (result == -1 && errno == EINTR);
    return result;
}

std::string c_string(const uint8_t* text, size_t size) {
    return std::string(reinterpret_cast<const char*>(text), strnlen(reinterpret_cast<const char*>(text), size));
}

std::string setup_hint() {
    return "load the v4l2loopback module first, e.g.: sudo tools/setup_loopback.sh "
           "(or: sudo modprobe v4l2loopback devices=1 video_nr=10 card_label=\"Virtual Camera Engine\" "
           "exclusive_caps=1)";
}

// Fills V4L2DeviceInfo from VIDIOC_QUERYCAP on an open descriptor.
Status query(int fd, const std::string& path, V4L2DeviceInfo& info) {
    v4l2_capability capability{};
    if (xioctl(fd, VIDIOC_QUERYCAP, &capability) != 0) {
        return Status(StatusCode::DeviceError, "'" + path + "' is not a V4L2 video device (VIDIOC_QUERYCAP: " +
                                                   std::strerror(errno) + ")");
    }
    info.path = path;
    info.driver = c_string(capability.driver, sizeof(capability.driver));
    info.card = c_string(capability.card, sizeof(capability.card));
    info.bus_info = c_string(capability.bus_info, sizeof(capability.bus_info));
    // device_caps describes THIS device node; capabilities the whole driver.
    info.capabilities = (capability.capabilities & V4L2_CAP_DEVICE_CAPS) ? capability.device_caps
                                                                            : capability.capabilities;
    info.is_loopback = info.driver == "v4l2 loopback" || info.driver == "v4l2loopback";
    info.can_output = (info.capabilities & V4L2_CAP_VIDEO_OUTPUT) != 0;
    return Status::ok_status();
}

}  // namespace

uint32_t v4l2_fourcc_for(PixelFormat format) {
    switch (format) {
        case PixelFormat::YUYV:  return V4L2_PIX_FMT_YUYV;
        case PixelFormat::UYVY:  return V4L2_PIX_FMT_UYVY;
        case PixelFormat::I420:  return V4L2_PIX_FMT_YUV420;  // 'YU12'
        case PixelFormat::NV12:  return V4L2_PIX_FMT_NV12;
        case PixelFormat::RGB24: return V4L2_PIX_FMT_RGB24;   // 'RGB3'
        case PixelFormat::BGR24: return V4L2_PIX_FMT_BGR24;   // 'BGR3'
        case PixelFormat::GRAY8: return V4L2_PIX_FMT_GREY;
    }
    return 0;
}

std::optional<PixelFormat> pixel_format_for_fourcc(uint32_t fourcc) {
    for (PixelFormat format : {PixelFormat::YUYV, PixelFormat::UYVY, PixelFormat::I420, PixelFormat::NV12,
                               PixelFormat::RGB24, PixelFormat::BGR24, PixelFormat::GRAY8}) {
        if (v4l2_fourcc_for(format) == fourcc) {
            return format;
        }
    }
    return std::nullopt;
}

std::string fourcc_to_string(uint32_t fourcc) {
    std::string text(4, ' ');
    for (int i = 0; i < 4; ++i) {
        text[static_cast<size_t>(i)] = static_cast<char>((fourcc >> (8 * i)) & 0xFF);
    }
    return text;
}

Result<V4L2DeviceInfo> V4L2LoopbackCamera::probe(const std::string& path) {
    struct stat file_status {};
    if (stat(path.c_str(), &file_status) != 0) {
        return Status(StatusCode::NotFound, "camera device '" + path + "' does not exist; " + setup_hint());
    }
    if (!S_ISCHR(file_status.st_mode)) {
        return Status(StatusCode::DeviceError, "'" + path + "' is not a device file");
    }

    const int fd = ::open(path.c_str(), O_WRONLY | O_NONBLOCK | O_CLOEXEC);
    if (fd < 0) {
        if (errno == EACCES || errno == EPERM) {
            return Status(StatusCode::PermissionDenied,
                          "no permission to open '" + path +
                              "'; add yourself to the 'video' group: sudo usermod -aG video $USER (then log in again)");
        }
        if (errno == EBUSY) {
            return Status(StatusCode::DeviceError, "'" + path + "' is busy (another program is writing to it)");
        }
        return Status(StatusCode::DeviceError, "cannot open '" + path + "': " + std::strerror(errno));
    }

    V4L2DeviceInfo info;
    Status status = query(fd, path, info);
    ::close(fd);
    if (!status.ok()) {
        return status;
    }
    if (!info.is_loopback) {
        return Status(StatusCode::DeviceError, "'" + path + "' is a real video device (driver '" + info.driver +
                                                   "', '" + info.card + "'), not a v4l2loopback device; " +
                                                   "choose the loopback device with --device");
    }
    if (!info.can_output) {
        // With exclusive_caps=1 the device advertises only CAPTURE while
        // another producer is streaming into it.
        return Status(StatusCode::DeviceError,
                      "'" + path + "' does not accept output right now: another producer is probably streaming to it");
    }
    return info;
}

Status V4L2LoopbackCamera::open(const FrameFormat& format, Rational fps) {
    close();
    fd_ = ::open(path_.c_str(), O_WRONLY | O_NONBLOCK | O_CLOEXEC);
    if (fd_ < 0) {
        return Status(StatusCode::DeviceError, "cannot open '" + path_ + "': " + std::strerror(errno));
    }
    Status status = query(fd_, path_, info_);
    if (!status.ok()) {
        close();
        return status;
    }

    // ---- Declare the format (VIDIOC_S_FMT on the OUTPUT side) ----
    const bool is_yuv = pixel_format_info(format.pixel_format).is_yuv;
    v4l2_format v4l2_fmt{};
    v4l2_fmt.type = V4L2_BUF_TYPE_VIDEO_OUTPUT;
    v4l2_fmt.fmt.pix.width = static_cast<uint32_t>(format.width);
    v4l2_fmt.fmt.pix.height = static_cast<uint32_t>(format.height);
    v4l2_fmt.fmt.pix.pixelformat = v4l2_fourcc_for(format.pixel_format);
    v4l2_fmt.fmt.pix.field = V4L2_FIELD_NONE;  // progressive frames
    v4l2_fmt.fmt.pix.bytesperline = static_cast<uint32_t>(format.planes[0].stride_bytes);
    v4l2_fmt.fmt.pix.sizeimage = static_cast<uint32_t>(format.size_bytes);
    // Colour signalling (docs/FORMATS.md): YUV = BT.601 limited, RGB = sRGB full.
    v4l2_fmt.fmt.pix.colorspace = is_yuv ? V4L2_COLORSPACE_SMPTE170M : V4L2_COLORSPACE_SRGB;
    v4l2_fmt.fmt.pix.ycbcr_enc = is_yuv ? V4L2_YCBCR_ENC_601 : V4L2_YCBCR_ENC_DEFAULT;
    v4l2_fmt.fmt.pix.quantization = is_yuv ? V4L2_QUANTIZATION_LIM_RANGE : V4L2_QUANTIZATION_FULL_RANGE;

    if (xioctl(fd_, VIDIOC_S_FMT, &v4l2_fmt) != 0) {
        Status error(StatusCode::DeviceError, "VIDIOC_S_FMT " + fourcc_to_string(v4l2_fourcc_for(format.pixel_format)) +
                                                  " " + std::to_string(format.width) + "x" +
                                                  std::to_string(format.height) + " failed on '" + path_ +
                                                  "': " + std::strerror(errno));
        close();
        return error;
    }
    // The driver may adjust values it does not support: verify.
    if (v4l2_fmt.fmt.pix.width != static_cast<uint32_t>(format.width) ||
        v4l2_fmt.fmt.pix.height != static_cast<uint32_t>(format.height) ||
        v4l2_fmt.fmt.pix.pixelformat != v4l2_fourcc_for(format.pixel_format) ||
        v4l2_fmt.fmt.pix.sizeimage < format.size_bytes) {
        Status error(StatusCode::DeviceError,
                     "device '" + path_ + "' changed the format to " + fourcc_to_string(v4l2_fmt.fmt.pix.pixelformat) +
                         " " + std::to_string(v4l2_fmt.fmt.pix.width) + "x" + std::to_string(v4l2_fmt.fmt.pix.height) +
                         " (it may be locked to another format by a previous producer; reload the module or use "
                         "another device)");
        close();
        return error;
    }

    // ---- Declare the frame rate (VIDIOC_S_PARM) ----
    // Consumers read it with VIDIOC_G_PARM. timeperframe = 1 / fps.
    v4l2_streamparm parameters{};
    parameters.type = V4L2_BUF_TYPE_VIDEO_OUTPUT;
    parameters.parm.output.timeperframe.numerator = static_cast<uint32_t>(fps.den);
    parameters.parm.output.timeperframe.denominator = static_cast<uint32_t>(fps.num);
    if (xioctl(fd_, VIDIOC_S_PARM, &parameters) != 0) {
        VCAM_WARN(kModule, "VIDIOC_S_PARM failed (" << std::strerror(errno)
                                                    << "); consumers may see a default frame rate");
    }

    frame_bytes_ = format.size_bytes;
    VCAM_INFO(kModule, "opened " << description() << ": " << fourcc_to_string(v4l2_fourcc_for(format.pixel_format))
                                 << " " << format.width << "x" << format.height << " @ " << to_string(fps) << " fps");
    return Status::ok_status();
}

PublishOutcome V4L2LoopbackCamera::publish(const FrameView& frame) {
    PublishOutcome outcome;
    // v4l2loopback copies the whole frame into its buffer in one write().
    const ssize_t result = ::write(fd_, frame.bytes.data(), frame_bytes_);
    if (result == static_cast<ssize_t>(frame_bytes_)) {
        return outcome;
    }
    if (result < 0 && (errno == EAGAIN || errno == EWOULDBLOCK)) {
        outcome.kind = PublishOutcome::Kind::Backpressure;
        return outcome;
    }
    outcome.kind = PublishOutcome::Kind::Error;
    if (result < 0) {
        // ENODEV/EIO/EBADF: the device disappeared (module unloaded) — unrecoverable.
        outcome.fatal = errno == ENODEV || errno == EIO || errno == EBADF || errno == ENXIO;
        outcome.status = Status(StatusCode::DeviceError, "write to '" + path_ + "' failed: " + std::strerror(errno));
    } else {
        outcome.status = Status(StatusCode::DeviceError, "short write to '" + path_ + "' (" + std::to_string(result) +
                                                             " of " + std::to_string(frame_bytes_) + " bytes)");
    }
    return outcome;
}

void V4L2LoopbackCamera::close() {
    if (fd_ >= 0) {
        ::close(fd_);
        fd_ = -1;
    }
}

std::string V4L2LoopbackCamera::description() const {
    if (info_.card.empty()) {
        return path_;
    }
    return path_ + " ('" + info_.card + "', " + info_.driver + ")";
}

}  // namespace vcam
