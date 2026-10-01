# resample — maps output slots to source frames (FPS conversion, pause, seek, loop).
# Pure logic over a Timeline: no pixels, no clocks, no devices.
vcam_add_module(resample
    SOURCES
        src/playhead.cpp
        src/source_clock.cpp
    PUBLIC_DEPS
        vcam::buffer
        vcam::core)

vcam_add_module_test(resample
    SOURCES
        tests/test_playhead.cpp
        tests/test_source_clock.cpp)
