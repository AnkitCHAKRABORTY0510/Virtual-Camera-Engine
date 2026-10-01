# source — FrameSource interface and its implementations:
#   VideoFileSource (FFmpeg), ImageSequenceSource, PatternSource, PushSource.
# Uses the convert module and FFmpeg internally (PRIVATE): modules that use a
# FrameSource never see FFmpeg or the converter.
vcam_add_module(source
    SOURCES
        src/frame_barcode.cpp
        src/frame_source.cpp
        src/image_sequence_source.cpp
        src/pattern_source.cpp
        src/push_source.cpp
        src/source_factory.cpp
        src/timestamp_stats.cpp
        src/video_file_source.cpp
    PUBLIC_DEPS
        vcam::core
    PRIVATE_DEPS
        vcam::convert
        PkgConfig::FFMPEG)

vcam_add_module_test(source
    NEEDS_MEDIA
    SOURCES
        tests/test_timestamp_stats.cpp
        tests/test_video_file_source.cpp
        tests/test_other_sources.cpp)
