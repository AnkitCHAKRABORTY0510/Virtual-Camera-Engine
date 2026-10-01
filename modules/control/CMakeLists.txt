# control — state machine, commands, the Engine (threads that connect all
# modules) and the terminal ControlLoop.
vcam_add_module(control
    SOURCES
        src/commands.cpp
        src/control_loop.cpp
        src/engine.cpp
        src/state_machine.cpp
    PUBLIC_DEPS
        vcam::buffer
        vcam::config
        vcam::core
        vcam::metrics
        vcam::output
        vcam::resample
        vcam::source
        vcam::timing)

vcam_add_module_test(control
    NEEDS_MEDIA
    SOURCES
        tests/test_state_and_commands.cpp
        tests/test_engine.cpp)
