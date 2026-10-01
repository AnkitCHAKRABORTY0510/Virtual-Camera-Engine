# =============================================================================
# PlatformChecks.cmake — stop early, with a clear message, on systems where the
# engine cannot work, instead of failing later with confusing compiler errors.
#
# Checked:
#   * operating system   Linux only (v4l2loopback is a Linux kernel driver)
#   * compiler           C++20 with <span> (GCC >= 10, Clang >= 12)
#   * 64-bit integers    __int128 for exact timing arithmetic (all 64-bit CPUs:
#                        x86_64, aarch64/arm64, riscv64, ppc64le, s390x)
#   * kernel header      <linux/videodev2.h> (package linux-libc-dev / kernel-headers)
#
# check_cxx_source_compiles(code RESULT) compiles a tiny test program with the
# project's compiler and sets RESULT to TRUE/FALSE; nothing is executed.
# =============================================================================
include(CheckCXXSourceCompiles)
include(CheckIncludeFileCXX)

set(VCAM_INSTALL_HINT "Install all dependencies with:  bash tools/install_deps.sh   (or: make install-deps)")

# ---- Operating system -------------------------------------------------------
if(NOT CMAKE_SYSTEM_NAME STREQUAL "Linux")
    message(FATAL_ERROR
        "Virtual Camera Engine runs on Linux only (found: ${CMAKE_SYSTEM_NAME}).\n"
        "It creates the camera through the Linux kernel driver v4l2loopback, which does not exist on "
        "macOS or Windows. Use a Linux machine or a Linux virtual machine with USB/camera pass-through.")
endif()

# ---- Compiler -----------------------------------------------------------------
if(CMAKE_CXX_COMPILER_ID STREQUAL "GNU" AND CMAKE_CXX_COMPILER_VERSION VERSION_LESS 10)
    message(FATAL_ERROR
        "GCC ${CMAKE_CXX_COMPILER_VERSION} is too old: C++20 needs GCC 10 or newer.\n"
        "Ubuntu 20.04: sudo apt install g++-10, then: make CMAKE_FLAGS=\"-DCMAKE_CXX_COMPILER=g++-10\"")
endif()
if(CMAKE_CXX_COMPILER_ID STREQUAL "Clang" AND CMAKE_CXX_COMPILER_VERSION VERSION_LESS 12)
    message(FATAL_ERROR "Clang ${CMAKE_CXX_COMPILER_VERSION} is too old: C++20 needs Clang 12 or newer.")
endif()

set(CMAKE_REQUIRED_FLAGS "-std=c++20")
check_cxx_source_compiles("
    #include <span>
    int main() { int values[2] = {1, 2}; std::span<int> view(values); return view.size() == 2 ? 0 : 1; }"
    VCAM_HAVE_STD_SPAN)
if(NOT VCAM_HAVE_STD_SPAN)
    message(FATAL_ERROR "The C++ standard library has no <span>: install a newer compiler (GCC >= 10, Clang >= 12).")
endif()

# ---- 128-bit integers (exact timestamp arithmetic) ------------------------------
check_cxx_source_compiles("
    int main() { __extension__ __int128 big = 1; big <<= 100; return big > 0 ? 0 : 1; }"
    VCAM_HAVE_INT128)
unset(CMAKE_REQUIRED_FLAGS)
if(NOT VCAM_HAVE_INT128)
    message(FATAL_ERROR
        "This compiler/CPU has no 128-bit integer type (__int128), which the engine uses for exact, "
        "drift-free timing arithmetic. All 64-bit systems have it (x86_64, arm64/aarch64, riscv64, ...); "
        "32-bit systems (e.g. 32-bit Raspberry Pi OS) are not supported. Use a 64-bit OS.")
endif()

# ---- Kernel video header ------------------------------------------------------------
check_include_file_cxx("linux/videodev2.h" VCAM_HAVE_VIDEODEV2)
if(NOT VCAM_HAVE_VIDEODEV2)
    message(FATAL_ERROR
        "<linux/videodev2.h> not found (Linux kernel user-space headers).\n"
        "Debian/Ubuntu: linux-libc-dev · Fedora: kernel-headers · Arch: linux-api-headers\n"
        "${VCAM_INSTALL_HINT}")
endif()

message(STATUS "Platform          : ${CMAKE_SYSTEM_NAME} ${CMAKE_SYSTEM_PROCESSOR}, "
               "${CMAKE_CXX_COMPILER_ID} ${CMAKE_CXX_COMPILER_VERSION}")
