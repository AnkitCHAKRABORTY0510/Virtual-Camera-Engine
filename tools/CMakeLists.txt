# Debugging tools. Each tool exercises one module on its own, so a component
# can be tested without building or running the whole engine.

# Phase 1: decode a file through the source module and report what it finds.
add_executable(probe_source probe_source.cpp)
target_link_libraries(probe_source PRIVATE vcam::source vcam::core)
vcam_set_warnings(probe_source)

# Phase 3: measure scheduler timing accuracy without any video device.
add_executable(bench_scheduler bench_scheduler.cpp)
target_link_libraries(bench_scheduler PRIVATE vcam::timing vcam::metrics vcam::core)
vcam_set_warnings(bench_scheduler)

# Phase 4: print the output-slot -> source-frame mapping for any pair of rates.
add_executable(resample_table resample_table.cpp)
target_link_libraries(resample_table PRIVATE vcam::resample)
vcam_set_warnings(resample_table)

# Phase 5: consumer-side check of the virtual camera (opens /dev/videoN like an app).
add_executable(vcam_probe vcam_probe.cpp)
target_link_libraries(vcam_probe PRIVATE vcam::output vcam::source vcam::metrics vcam::core)
vcam_set_warnings(vcam_probe)

# Installed with the program (make install / packages): checks what applications
# receive from the camera. The other tools are for development only.
install(TARGETS vcam_probe RUNTIME DESTINATION ${CMAKE_INSTALL_BINDIR})
