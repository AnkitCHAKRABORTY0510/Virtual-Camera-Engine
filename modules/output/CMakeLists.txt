# output — HOW frames reach Linux: v4l2loopback device, null sink, raw file.
# Uses only the kernel's V4L2 header (linux/videodev2.h); no extra library.
vcam_add_module(output
    SOURCES
        src/v4l2_loopback_camera.cpp
        src/virtual_camera.cpp
    PUBLIC_DEPS
        vcam::core)

vcam_add_module_test(output
    SOURCES
        tests/test_output.cpp)
