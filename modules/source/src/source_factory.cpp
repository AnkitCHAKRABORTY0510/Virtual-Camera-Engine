#include "vcam/source/source_factory.hpp"

#include <filesystem>

#include "vcam/source/image_sequence_source.hpp"
#include "vcam/source/pattern_source.hpp"
#include "vcam/source/push_source.hpp"
#include "vcam/source/video_file_source.hpp"

namespace vcam {

const char* source_type_name(SourceType type) {
    switch (type) {
        case SourceType::Auto:    return "auto";
        case SourceType::Video:   return "video";
        case SourceType::Images:  return "images";
        case SourceType::Pattern: return "pattern";
        case SourceType::Push:    return "push";
    }
    return "?";
}

Result<SourceType> parse_source_type(const std::string& text) {
    if (text == "auto") return SourceType::Auto;
    if (text == "video") return SourceType::Video;
    if (text == "images" || text == "image-sequence") return SourceType::Images;
    if (text == "pattern") return SourceType::Pattern;
    if (text == "push" || text == "script") return SourceType::Push;
    return Status(StatusCode::InvalidArgument,
                  "unknown source type '" + text + "' (auto, video, images, pattern, push)");
}

SourceType resolve_source_type(const SourceSpec& spec) {
    if (spec.type != SourceType::Auto) {
        return spec.type;
    }
    if (spec.path == "pattern") {
        return SourceType::Pattern;
    }
    std::error_code error;
    if (std::filesystem::is_directory(spec.path, error)) {
        return SourceType::Images;
    }
    return SourceType::Video;
}

Result<std::unique_ptr<FrameSource>> create_source(const SourceSpec& spec) {
    switch (resolve_source_type(spec)) {
        case SourceType::Video:
            if (spec.path.empty()) {
                return Status(StatusCode::InvalidArgument, "no input given (use --input)");
            }
            {
                auto video = std::make_unique<VideoFileSource>(spec.path);
                video->set_decoder_threads(spec.decoder_threads);
                return std::unique_ptr<FrameSource>(std::move(video));
            }
        case SourceType::Images:
            return std::unique_ptr<FrameSource>(std::make_unique<ImageSequenceSource>(spec.path, spec.fps));
        case SourceType::Pattern: {
            PatternOptions options;
            options.width = spec.pattern_width;
            options.height = spec.pattern_height;
            options.fps = spec.fps;
            options.frame_count = spec.pattern_frames;
            return std::unique_ptr<FrameSource>(std::make_unique<PatternSource>(options));
        }
        case SourceType::Push:
            return std::unique_ptr<FrameSource>(
                std::make_unique<PushSource>(spec.path.empty() ? "/tmp/vcam.sock" : spec.path));
        case SourceType::Auto:
            break;
    }
    return Status(StatusCode::Internal, "unresolved source type");
}

}  // namespace vcam
