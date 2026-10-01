# buffer — where decoded frames are stored between loading and output.
#   StreamBuffer (decode-ahead ring, default), RamBuffer + DiskBackedBuffer (full
#   preload), LiveFrameBuffer (newest frames only, for live input)
vcam_add_module(buffer
    SOURCES
        src/live_frame_buffer.cpp
        src/memory_budget.cpp
        src/slab_buffer.cpp
        src/stream_buffer.cpp
        src/timeline.cpp
    PUBLIC_DEPS
        vcam::core)

vcam_add_module_test(buffer
    SOURCES
        tests/test_timeline.cpp
        tests/test_slab_buffer.cpp
        tests/test_live_frame_buffer.cpp
        tests/test_stream_buffer.cpp)
