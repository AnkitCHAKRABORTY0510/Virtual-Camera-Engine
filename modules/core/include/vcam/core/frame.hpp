// =============================================================================
// frame.hpp — the common frame representation shared by every module
//
//   FrameFormat       how to interpret a block of bytes as an image
//   FrameTiming       when a source frame should be shown (logical source time)
//   FrameView         read-only view of one frame's bytes   (no ownership)
//   MutableFrameView  writable view of one frame's bytes    (no ownership)
//   OwnedFrame        a frame that owns its memory (tools/tests; the engine's
//                     buffers own memory themselves)
//
// Views are cheap to copy (a pointer + size + a small format struct), so frames
// can be passed between modules WITHOUT copying pixels. Whoever owns the memory
// (e.g. the FrameBuffer) must keep it alive while views exist.
// =============================================================================
#pragma once

#include <array>
#include <cstddef>
#include <cstdint>
#include <memory>
#include <span>
#include <string>

#include "vcam/core/pixel_format.hpp"
#include "vcam/core/status.hpp"

namespace vcam {

// Location of one image plane inside the frame's byte block.
struct PlaneLayout {
    size_t offset = 0;        // first byte of the plane, from the start of the frame
    size_t stride_bytes = 0;  // bytes from the start of one row to the next
    size_t rows = 0;          // number of rows in this plane
    size_t row_bytes = 0;     // visible bytes per row (== stride for tightly packed layouts)
};

struct FrameFormat {
    int width = 0;
    int height = 0;
    PixelFormat pixel_format = PixelFormat::YUYV;
    ColorSpace color_space = ColorSpace::BT601;  // meaningful for YUV formats
    ColorRange color_range = ColorRange::Limited;
    int plane_count = 0;
    std::array<PlaneLayout, 3> planes{};
    size_t size_bytes = 0;  // total bytes of one frame (all planes)
};

// Computes the tightly packed layout for (width, height, pixel format).
// Fails with InvalidArgument when the size is impossible for that format,
// e.g. an odd width with YUYV (two pixels share one U/V pair).
Result<FrameFormat> make_frame_format(int width, int height, PixelFormat pixel_format,
                                      ColorSpace color_space = ColorSpace::BT601,
                                      ColorRange color_range = ColorRange::Limited);

// "1280x720 yuyv bt601/limited (1843200 bytes)"
std::string describe(const FrameFormat& format);

bool same_layout(const FrameFormat& a, const FrameFormat& b);

// Timing of one SOURCE frame, in the source's own time base (see SourceInfo::time_base).
// This is logical video time, not wall-clock time.
struct FrameTiming {
    uint64_t source_index = 0;   // 0, 1, 2, ... in presentation order
    int64_t pts_ticks = 0;       // presentation timestamp; first frame normalised to 0
    int64_t duration_ticks = 0;  // display duration (0 = unknown)
};

struct FrameView {
    FrameFormat format;
    std::span<const uint8_t> bytes;  // exactly format.size_bytes long
    FrameTiming timing;

    // Pointer to the first byte of plane `index` (0 = Y or packed data).
    const uint8_t* plane(int index) const { return bytes.data() + format.planes[static_cast<size_t>(index)].offset; }
};

struct MutableFrameView {
    FrameFormat format;
    std::span<uint8_t> bytes;  // exactly format.size_bytes long

    uint8_t* plane(int index) const { return bytes.data() + format.planes[static_cast<size_t>(index)].offset; }
};

// A frame that owns a 64-byte aligned memory block (aligned so SIMD code in
// libswscale can use fast paths). Movable, not copyable (copying would
// duplicate megabytes silently).
class OwnedFrame {
public:
    OwnedFrame() = default;
    explicit OwnedFrame(const FrameFormat& format);

    OwnedFrame(OwnedFrame&&) noexcept = default;
    OwnedFrame& operator=(OwnedFrame&&) noexcept = default;
    OwnedFrame(const OwnedFrame&) = delete;
    OwnedFrame& operator=(const OwnedFrame&) = delete;

    bool empty() const { return data_ == nullptr; }
    const FrameFormat& format() const { return format_; }

    MutableFrameView mutable_view();
    FrameView view(const FrameTiming& timing = {}) const;

private:
    // std::free is the matching deallocator for std::aligned_alloc.
    struct FreeDeleter {
        void operator()(uint8_t* pointer) const;
    };

    FrameFormat format_;
    std::unique_ptr<uint8_t[], FreeDeleter> data_;
};

}  // namespace vcam
