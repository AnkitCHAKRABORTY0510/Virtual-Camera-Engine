// =============================================================================
// source_factory.hpp — create the right FrameSource from a description
//
//   --input video.mp4          -> VideoFileSource
//   --input frames/            -> ImageSequenceSource (directory)
//   --input pattern            -> PatternSource
//   --source-type push         -> PushSource (path = Unix socket)
// =============================================================================
#pragma once

#include <memory>
#include <string>

#include "vcam/source/frame_source.hpp"

namespace vcam {

enum class SourceType { Auto, Video, Images, Pattern, Push };

const char* source_type_name(SourceType type);
Result<SourceType> parse_source_type(const std::string& text);

struct SourceSpec {
    SourceType type = SourceType::Auto;
    std::string path;                // file, directory or socket path
    Rational fps{30, 1};             // images / pattern only
    int pattern_width = 1280;
    int pattern_height = 720;
    uint64_t pattern_frames = 300;
    int decoder_threads = 0;         // video files: 0 = one per CPU core
};

// Resolves Auto (directory -> images, "pattern" -> pattern, else video file).
SourceType resolve_source_type(const SourceSpec& spec);

Result<std::unique_ptr<FrameSource>> create_source(const SourceSpec& spec);

}  // namespace vcam
