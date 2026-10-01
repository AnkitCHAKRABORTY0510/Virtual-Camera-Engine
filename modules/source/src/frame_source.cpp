#include "vcam/source/frame_source.hpp"

namespace vcam {

Result<FrameFormat> make_output_format(const SourceInfo& info, PixelFormat pixel_format) {
    // Output resolution is ALWAYS the source resolution (architecture v0.2).
    Result<FrameFormat> format =
        make_frame_format(info.width, info.height, pixel_format, ColorSpace::BT601, ColorRange::Limited);
    if (!format.ok()) {
        return Status(format.status().code(),
                      "source '" + info.uri + "': " + format.status().message());
    }
    return format;
}

}  // namespace vcam
