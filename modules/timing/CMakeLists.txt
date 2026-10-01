# timing — WHEN frames are emitted: clocks and the absolute-deadline scheduler.
vcam_add_module(timing
    SOURCES
        src/clock.cpp
        src/realtime.cpp
        src/scheduler.cpp
    PUBLIC_DEPS
        vcam::core)

vcam_add_module_test(timing
    SOURCES
        tests/test_scheduler.cpp)
