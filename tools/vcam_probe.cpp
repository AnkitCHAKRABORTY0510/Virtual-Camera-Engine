// =============================================================================
// vcam_probe — Phase 5 tool: look at the virtual camera FROM AN APPLICATION'S SIDE
//
// Opens /dev/videoN exactly like a webcam application (V4L2 capture with
// memory-mapped buffers), receives frames and reports:
//   * the format and frame rate the device advertises,
//   * the frame rate and timing jitter actually received,
//   * with --barcode (engine running `--input pattern`): every frame's number,
//     proving that frames arrive in order with no unexpected drops/duplicates.
//
// Examples (in a second terminal while the engine runs):
//   vcam_probe --device /dev/video10 --frames 300
//   vcam_probe --device /dev/video10 --frames 600 --barcode --csv received.csv
//
// Exit codes: 0 ok, 1 usage, 3 device error, 5 problems found (--barcode).
// =============================================================================
#include <fcntl.h>
#include <getopt.h>
#include <linux/videodev2.h>
#include <poll.h>
#include <sys/ioctl.h>
#include <sys/mman.h>
#include <unistd.h>

#include <cerrno>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <fstream>
#include <optional>
#include <string>
#include <vector>

#include "vcam/core/clock_time.hpp"
#include "vcam/core/frame.hpp"
#include "vcam/metrics/histogram.hpp"
#include "vcam/output/v4l2_loopback_camera.hpp"
#include "vcam/source/frame_barcode.hpp"

namespace {

int xioctl(int fd, unsigned long request, void* argument) {
    int result = 0;
    do {
        result = ioctl(fd, request, argument);
    } while (result == -1 && errno == EINTR);
    return result;
}

struct MappedBuffer {
    void* start = MAP_FAILED;
    size_t length = 0;
};

struct Received {
    uint64_t index = 0;
    int64_t receive_ns = 0;
    int64_t buffer_timestamp_ns = 0;
    uint32_t sequence = 0;
    std::optional<uint32_t> barcode;
};

void print_stats(const char* label, const vcam::LatencyHistogram& h) {
    std::printf("  %-22s: mean %.3f | P95 %.3f | P99 %.3f | max %.3f ms\n", label, h.mean_ns() / 1e6,
                static_cast<double>(h.percentile_ns(95)) / 1e6, static_cast<double>(h.percentile_ns(99)) / 1e6,
                static_cast<double>(h.max_ns()) / 1e6);
}

int fail(const std::string& message) {
    std::fprintf(stderr, "error: %s\n", message.c_str());
    return 3;
}

}  // namespace

int main(int argc, char** argv) {
    std::string device = "/dev/video10";
    unsigned long frames_wanted = 300;
    bool check_barcode = false;
    int timeout_s = 5;
    std::string csv_path;

    static const option kOptions[] = {
        {"device", required_argument, nullptr, 'd'}, {"frames", required_argument, nullptr, 'n'},
        {"barcode", no_argument, nullptr, 'b'},      {"timeout", required_argument, nullptr, 't'},
        {"csv", required_argument, nullptr, 'c'},    {"help", no_argument, nullptr, 'h'},
        {nullptr, 0, nullptr, 0},
    };
    int letter = 0;
    while ((letter = getopt_long(argc, argv, "h", kOptions, nullptr)) != -1) {
        switch (letter) {
            case 'd': device = optarg; break;
            case 'n': frames_wanted = std::strtoul(optarg, nullptr, 10); break;
            case 'b': check_barcode = true; break;
            case 't': timeout_s = std::atoi(optarg); break;
            case 'c': csv_path = optarg; break;
            default:
                std::printf("Usage: %s [--device /dev/video10] [--frames N] [--barcode] [--timeout S] [--csv PATH]\n",
                            argv[0]);
                return letter == 'h' ? 0 : 1;
        }
    }

    // ---- 1. Open like an application and check capabilities --------------------
    const int fd = ::open(device.c_str(), O_RDWR | O_CLOEXEC);
    if (fd < 0) {
        return fail("cannot open " + device + ": " + std::strerror(errno));
    }
    v4l2_capability capability{};
    if (xioctl(fd, VIDIOC_QUERYCAP, &capability) != 0) {
        return fail(device + " is not a V4L2 device");
    }
    const uint32_t caps = (capability.capabilities & V4L2_CAP_DEVICE_CAPS) ? capability.device_caps
                                                                            : capability.capabilities;
    std::printf("Device   : %s\n  card   : %s\n  driver : %s\n  capture: %s, streaming: %s\n", device.c_str(),
                reinterpret_cast<const char*>(capability.card), reinterpret_cast<const char*>(capability.driver),
                (caps & V4L2_CAP_VIDEO_CAPTURE) ? "yes" : "NO", (caps & V4L2_CAP_STREAMING) ? "yes" : "NO");
    if (!(caps & V4L2_CAP_VIDEO_CAPTURE)) {
        return fail("device does not offer capture: is the engine streaming into it? (exclusive_caps=1 shows capture "
                    "only while a producer is active)");
    }

    // ---- 2. Read the advertised format and frame rate ------------------------------
    v4l2_format format{};
    format.type = V4L2_BUF_TYPE_VIDEO_CAPTURE;
    if (xioctl(fd, VIDIOC_G_FMT, &format) != 0) {
        return fail(std::string("VIDIOC_G_FMT: ") + std::strerror(errno));
    }
    v4l2_streamparm parameters{};
    parameters.type = V4L2_BUF_TYPE_VIDEO_CAPTURE;
    double declared_fps = 0;
    if (xioctl(fd, VIDIOC_G_PARM, &parameters) == 0 && parameters.parm.capture.timeperframe.numerator > 0) {
        declared_fps = static_cast<double>(parameters.parm.capture.timeperframe.denominator) /
                       parameters.parm.capture.timeperframe.numerator;
    }
    const v4l2_pix_format& pix = format.fmt.pix;
    std::printf("Format   : %s %ux%u, bytesperline %u, sizeimage %u, colorspace %u\n",
                vcam::fourcc_to_string(pix.pixelformat).c_str(), pix.width, pix.height, pix.bytesperline,
                pix.sizeimage, pix.colorspace);
    std::printf("Declared : %.3f fps\n", declared_fps);

    std::optional<vcam::FrameFormat> frame_format;
    if (auto pixel_format = vcam::pixel_format_for_fourcc(pix.pixelformat)) {
        auto made = vcam::make_frame_format(static_cast<int>(pix.width), static_cast<int>(pix.height), *pixel_format);
        if (made.ok()) {
            frame_format = made.value();
        }
    }
    if (check_barcode && !frame_format) {
        return fail("--barcode: unsupported pixel format " + vcam::fourcc_to_string(pix.pixelformat));
    }

    // ---- 3. Memory-mapped streaming (what OpenCV, FFmpeg, browsers do) -------------
    v4l2_requestbuffers request{};
    request.count = 4;
    request.type = V4L2_BUF_TYPE_VIDEO_CAPTURE;
    request.memory = V4L2_MEMORY_MMAP;
    if (xioctl(fd, VIDIOC_REQBUFS, &request) != 0 || request.count == 0) {
        return fail(std::string("VIDIOC_REQBUFS: ") + std::strerror(errno));
    }
    std::vector<MappedBuffer> buffers(request.count);
    for (uint32_t i = 0; i < request.count; ++i) {
        v4l2_buffer buffer{};
        buffer.type = V4L2_BUF_TYPE_VIDEO_CAPTURE;
        buffer.memory = V4L2_MEMORY_MMAP;
        buffer.index = i;
        if (xioctl(fd, VIDIOC_QUERYBUF, &buffer) != 0) {
            return fail(std::string("VIDIOC_QUERYBUF: ") + std::strerror(errno));
        }
        buffers[i].length = buffer.length;
        buffers[i].start = mmap(nullptr, buffer.length, PROT_READ | PROT_WRITE, MAP_SHARED, fd, buffer.m.offset);
        if (buffers[i].start == MAP_FAILED) {
            return fail(std::string("mmap: ") + std::strerror(errno));
        }
        if (xioctl(fd, VIDIOC_QBUF, &buffer) != 0) {
            return fail(std::string("VIDIOC_QBUF: ") + std::strerror(errno));
        }
    }
    v4l2_buf_type type = V4L2_BUF_TYPE_VIDEO_CAPTURE;
    if (xioctl(fd, VIDIOC_STREAMON, &type) != 0) {
        return fail(std::string("VIDIOC_STREAMON: ") + std::strerror(errno));
    }

    // ---- 4. Receive ------------------------------------------------------------------
    std::printf("\nReceiving %lu frames...\n", frames_wanted);
    std::fflush(stdout);
    std::vector<Received> received;
    received.reserve(frames_wanted);
    while (received.size() < frames_wanted) {
        pollfd waiter{fd, POLLIN, 0};
        const int ready = poll(&waiter, 1, timeout_s * 1000);
        if (ready <= 0) {
            std::fprintf(stderr, "no frame for %d s (engine stopped or paused with output off?)\n", timeout_s);
            break;
        }
        v4l2_buffer buffer{};
        buffer.type = V4L2_BUF_TYPE_VIDEO_CAPTURE;
        buffer.memory = V4L2_MEMORY_MMAP;
        if (xioctl(fd, VIDIOC_DQBUF, &buffer) != 0) {
            if (errno == EAGAIN) continue;
            std::fprintf(stderr, "VIDIOC_DQBUF: %s\n", std::strerror(errno));
            break;
        }
        Received item;
        item.index = received.size();
        item.receive_ns = vcam::monotonic_now_ns();
        item.buffer_timestamp_ns = static_cast<int64_t>(buffer.timestamp.tv_sec) * 1'000'000'000 +
                                   static_cast<int64_t>(buffer.timestamp.tv_usec) * 1000;
        item.sequence = buffer.sequence;
        if (check_barcode && frame_format && buffer.bytesused >= frame_format->size_bytes) {
            const auto* data = static_cast<const uint8_t*>(buffers[buffer.index].start);
            vcam::FrameView view{*frame_format, std::span<const uint8_t>(data, frame_format->size_bytes), {}};
            item.barcode = vcam::read_barcode(view);
        }
        received.push_back(item);
        xioctl(fd, VIDIOC_QBUF, &buffer);  // give the buffer back
    }
    xioctl(fd, VIDIOC_STREAMOFF, &type);
    for (MappedBuffer& buffer : buffers) {
        munmap(buffer.start, buffer.length);
    }
    ::close(fd);

    // ---- 5. Report ---------------------------------------------------------------------
    if (received.size() < 2) {
        return fail("received fewer than 2 frames");
    }
    const double span_s = static_cast<double>(received.back().receive_ns - received.front().receive_ns) / 1e9;
    const double measured_fps = static_cast<double>(received.size() - 1) / span_s;
    const int64_t period_ns = declared_fps > 0 ? static_cast<int64_t>(1e9 / declared_fps) : 0;

    vcam::LatencyHistogram receive_jitter;
    vcam::LatencyHistogram timestamp_jitter;
    uint64_t sequence_gaps = 0;
    for (size_t i = 1; i < received.size(); ++i) {
        const int64_t interval = received[i].receive_ns - received[i - 1].receive_ns;
        const int64_t stamp_interval = received[i].buffer_timestamp_ns - received[i - 1].buffer_timestamp_ns;
        if (period_ns > 0) {
            receive_jitter.add(std::llabs(interval - period_ns));
            timestamp_jitter.add(std::llabs(stamp_interval - period_ns));
        }
        if (received[i].sequence != received[i - 1].sequence + 1) {
            ++sequence_gaps;
        }
    }

    std::printf("\nConsumer-side results\n");
    std::printf("  %-22s: %zu in %.3f s\n", "frames received", received.size(), span_s);
    std::printf("  %-22s: %.4f (declared %.3f)\n", "measured fps", measured_fps, declared_fps);
    if (period_ns > 0) {
        print_stats("receive jitter", receive_jitter);
        print_stats("buffer-timestamp jitter", timestamp_jitter);
    }
    std::printf("  %-22s: %llu\n", "V4L2 sequence gaps", static_cast<unsigned long long>(sequence_gaps));

    int exit_code = 0;
    if (check_barcode) {
        uint64_t unreadable = 0, duplicates = 0, gaps = 0, missing = 0, backwards = 0;
        std::optional<uint32_t> previous;
        for (const Received& item : received) {
            if (!item.barcode) {
                ++unreadable;
                continue;
            }
            if (previous) {
                if (*item.barcode == *previous) {
                    ++duplicates;
                } else if (*item.barcode > *previous + 1) {
                    ++gaps;
                    missing += *item.barcode - *previous - 1;
                } else if (*item.barcode < *previous) {
                    ++backwards;  // a loop restart (or real reordering)
                }
            }
            previous = item.barcode;
        }
        std::printf("Barcode check (pattern source)\n");
        std::printf("  %-22s: %llu\n", "unreadable frames", static_cast<unsigned long long>(unreadable));
        std::printf("  %-22s: %llu\n", "repeated frames", static_cast<unsigned long long>(duplicates));
        std::printf("  %-22s: %llu gaps, %llu frames\n", "missing frames", static_cast<unsigned long long>(gaps),
                    static_cast<unsigned long long>(missing));
        std::printf("  %-22s: %llu (loop restarts count here)\n", "backward jumps",
                    static_cast<unsigned long long>(backwards));
        if (unreadable > 0 || missing > 0) {
            exit_code = 5;
        }
    }

    if (!csv_path.empty()) {
        std::ofstream csv(csv_path);
        csv << "index,receive_ns,buffer_timestamp_ns,sequence,barcode\n";
        for (const Received& item : received) {
            csv << item.index << ',' << item.receive_ns << ',' << item.buffer_timestamp_ns << ',' << item.sequence
                << ',' << (item.barcode ? std::to_string(*item.barcode) : std::string("")) << '\n';
        }
    }
    return exit_code;
}
