# The complete application.
add_executable(virtual-camera main.cpp)
target_link_libraries(virtual-camera PRIVATE vcam::control vcam::config vcam::core)
target_compile_definitions(virtual-camera PRIVATE VCAM_VERSION="${PROJECT_VERSION}")
vcam_set_warnings(virtual-camera)
install(TARGETS virtual-camera RUNTIME DESTINATION ${CMAKE_INSTALL_BINDIR})
