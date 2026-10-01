# metrics — timing statistics (histograms, jitter, drift) and reports.
vcam_add_module(metrics
    SOURCES
        src/histogram.cpp
        src/timing_metrics.cpp
    PUBLIC_DEPS
        vcam::core)

vcam_add_module_test(metrics
    SOURCES
        tests/test_metrics.cpp)
