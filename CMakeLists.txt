# =============================================================================
# Virtual Camera Engine — top-level build
#
# Quick start:
#   cmake -S . -B build -DCMAKE_BUILD_TYPE=Release
#   cmake --build build -j
#   ctest --test-dir build --output-on-failure
#
# Or simply use the Makefile wrapper:  make help
#
# Every module (modules/<name>) is its own static library with its own include
# directory. A module can only #include headers of modules it links against,
# so the architecture's separation rules are enforced by the compiler.
# =============================================================================
cmake_minimum_required(VERSION 3.20)

project(VirtualCameraEngine
    VERSION 1.1.0
    DESCRIPTION "General-purpose Linux virtual camera engine"
    LANGUAGES CXX)

# ---- Options -----------------------------------------------------------------
option(VCAM_BUILD_TESTS          "Build unit and integration tests (needs GoogleTest)" ON)
option(VCAM_BUILD_TOOLS          "Build debugging tools in tools/"                       ON)
option(VCAM_WARNINGS_AS_ERRORS   "Treat compiler warnings as errors"                     OFF)
option(VCAM_ENABLE_ASAN          "AddressSanitizer + LeakSanitizer"                      OFF)
option(VCAM_ENABLE_UBSAN         "UndefinedBehaviorSanitizer"                            OFF)
option(VCAM_ENABLE_TSAN          "ThreadSanitizer (cannot be combined with ASan)"        OFF)

# Default to Release when the user did not choose a build type.
if(NOT CMAKE_BUILD_TYPE AND NOT CMAKE_CONFIGURATION_TYPES)
    set(CMAKE_BUILD_TYPE Release CACHE STRING "Build type" FORCE)
    set_property(CACHE CMAKE_BUILD_TYPE PROPERTY STRINGS Debug Release RelWithDebInfo MinSizeRel)
endif()

# ---- Language settings ---------------------------------------------------------
set(CMAKE_CXX_STANDARD 20)
set(CMAKE_CXX_STANDARD_REQUIRED ON)
set(CMAKE_CXX_EXTENSIONS OFF)           # -std=c++20, not gnu++20
set(CMAKE_EXPORT_COMPILE_COMMANDS ON)   # build/compile_commands.json for editors / clangd

# Put all executables in build/bin so they are easy to find.
set(CMAKE_RUNTIME_OUTPUT_DIRECTORY ${CMAKE_BINARY_DIR}/bin)

list(APPEND CMAKE_MODULE_PATH ${CMAKE_CURRENT_SOURCE_DIR}/cmake)
include(PlatformChecks)     # Linux, C++20 compiler, 64-bit, kernel headers: clear errors early
include(CompilerWarnings)
include(Sanitizers)
include(VcamHelpers)

# ---- Dependencies -------------------------------------------------------------
find_package(Threads REQUIRED)
find_package(PkgConfig REQUIRED)

# FFmpeg libraries, found through pkg-config. IMPORTED_TARGET creates the
# target PkgConfig::FFMPEG that carries include paths and link flags.
# Minimum versions = FFmpeg 4.4 (Ubuntu 22.04, Debian 12 ships 5.1, Fedora/Arch 6.x-7.x).
pkg_check_modules(FFMPEG IMPORTED_TARGET
    libavformat>=58.76
    libavcodec>=58.134
    libavutil>=56.70
    libswscale>=5.9)
if(NOT FFMPEG_FOUND)
    message(FATAL_ERROR
        "FFmpeg development libraries (FFmpeg 4.4 or newer) were not found by pkg-config.\n"
        "Needed: libavformat, libavcodec, libavutil, libswscale.\n"
        "${VCAM_INSTALL_HINT}")
endif()
message(STATUS "FFmpeg: libavformat ${FFMPEG_libavformat_VERSION}, libavcodec ${FFMPEG_libavcodec_VERSION}, "
               "libavutil ${FFMPEG_libavutil_VERSION}, libswscale ${FFMPEG_libswscale_VERSION}")

if(VCAM_BUILD_TESTS)
    find_package(GTest QUIET)
    if(GTest_FOUND)
        enable_testing()
        include(GoogleTest)   # provides gtest_discover_tests()
    else()
        message(WARNING "GoogleTest not found: tests are disabled (the program itself builds fine). "
                        "${VCAM_INSTALL_HINT}")
        set(VCAM_BUILD_TESTS OFF)
    endif()
endif()

# ---- Test media fixture -------------------------------------------------------
# Test videos are generated (not committed) by tools/gen_test_media.sh. CTest
# runs the generator once as a "fixture" before any test that needs media.
set(VCAM_TEST_MEDIA_DIR ${CMAKE_BINARY_DIR}/test_media)
if(VCAM_BUILD_TESTS)
    add_test(NAME generate_test_media
             COMMAND bash ${CMAKE_CURRENT_SOURCE_DIR}/tools/gen_test_media.sh ${VCAM_TEST_MEDIA_DIR})
    set_tests_properties(generate_test_media PROPERTIES FIXTURES_SETUP test_media LABELS media)
endif()

# ---- Modules (order = dependency order) ---------------------------------------
add_subdirectory(modules/core)
add_subdirectory(modules/convert)
add_subdirectory(modules/source)
add_subdirectory(modules/buffer)
add_subdirectory(modules/timing)
add_subdirectory(modules/metrics)
add_subdirectory(modules/resample)
add_subdirectory(modules/output)
add_subdirectory(modules/config)
add_subdirectory(modules/control)
add_subdirectory(apps/virtual-camera)

if(VCAM_BUILD_TOOLS)
    add_subdirectory(tools)
endif()

if(VCAM_BUILD_TESTS)
    add_subdirectory(tests/integration)
endif()

# ---- Installation (make install) and packages (make package) ---------------------
# GNUInstallDirs defines the standard folders: CMAKE_INSTALL_BINDIR = bin,
# CMAKE_INSTALL_DOCDIR = share/doc/VirtualCameraEngine, ...
include(GNUInstallDirs)
install(PROGRAMS tools/setup_loopback.sh DESTINATION ${CMAKE_INSTALL_BINDIR} RENAME vcam-setup-loopback)
install(FILES README.md LICENSE CHANGELOG.md DESTINATION ${CMAKE_INSTALL_DOCDIR})
install(DIRECTORY docs/ DESTINATION ${CMAKE_INSTALL_DOCDIR}/docs)
install(DIRECTORY config/examples/ DESTINATION ${CMAKE_INSTALL_DOCDIR}/config-examples)
include(Packaging)

# ---- Summary ------------------------------------------------------------------
message(STATUS "Build type        : ${CMAKE_BUILD_TYPE}")
message(STATUS "Tests             : ${VCAM_BUILD_TESTS}")
message(STATUS "Tools             : ${VCAM_BUILD_TOOLS}")
message(STATUS "Sanitizers        : ASan=${VCAM_ENABLE_ASAN} UBSan=${VCAM_ENABLE_UBSAN} TSan=${VCAM_ENABLE_TSAN}")
