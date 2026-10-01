# convert — pixel-format and colour conversion at identical resolution (libswscale).
# FFmpeg is linked PRIVATE: users of this module only see forward declarations.
vcam_add_module(convert
    SOURCES
        src/format_converter.cpp
    PUBLIC_DEPS
        vcam::core
    PRIVATE_DEPS
        PkgConfig::FFMPEG)

# The test builds AVFrames by hand, so it needs FFmpeg headers itself.
vcam_add_module_test(convert
    SOURCES
        tests/test_format_converter.cpp
    LINK
        PkgConfig::FFMPEG)
