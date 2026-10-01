# config — settings: defaults < config file (YAML subset) < command line.
vcam_add_module(config
    SOURCES
        src/command_line.cpp
        src/config.cpp
        src/config_file.cpp
    PUBLIC_DEPS
        vcam::core
        vcam::resample
        vcam::source)

vcam_add_module_test(config
    SOURCES
        tests/test_config.cpp)
