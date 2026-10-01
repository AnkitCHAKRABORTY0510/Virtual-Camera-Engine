# core — types every other module shares: Frame, FrameFormat, PixelFormat,
# Rational, Status, logging, monotonic time. Depends on nothing but the OS.
vcam_add_module(core
    SOURCES
        src/clock_time.cpp
        src/frame.cpp
        src/log.cpp
        src/pixel_format.cpp
        src/rational.cpp
        src/status.cpp
        src/text_format.cpp
    PUBLIC_DEPS
        Threads::Threads)

vcam_add_module_test(core
    SOURCES
        tests/test_rational.cpp
        tests/test_pixel_format.cpp
        tests/test_log.cpp
        tests/test_text_format.cpp)
