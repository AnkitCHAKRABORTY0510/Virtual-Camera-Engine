# Sanitizers are runtime checkers built into GCC/Clang:
#   ASan  — buffer overflows, use-after-free, memory leaks (LeakSanitizer)
#   UBSan — undefined behaviour (signed overflow, misaligned access, ...)
#   TSan  — data races between threads
# They slow the program down (2-5x) and are meant for Debug/test builds.
# Flags are applied globally so every module and test is instrumented.

if(VCAM_ENABLE_ASAN AND VCAM_ENABLE_TSAN)
    message(FATAL_ERROR "ASan and TSan cannot be enabled together")
endif()

set(_vcam_sanitize_flags "")
if(VCAM_ENABLE_ASAN)
    list(APPEND _vcam_sanitize_flags -fsanitize=address)
endif()
if(VCAM_ENABLE_UBSAN)
    list(APPEND _vcam_sanitize_flags -fsanitize=undefined -fno-sanitize-recover=undefined)
endif()
if(VCAM_ENABLE_TSAN)
    list(APPEND _vcam_sanitize_flags -fsanitize=thread)
endif()

if(_vcam_sanitize_flags)
    # -fno-omit-frame-pointer gives readable stack traces in sanitizer reports.
    add_compile_options(${_vcam_sanitize_flags} -fno-omit-frame-pointer -g)
    add_link_options(${_vcam_sanitize_flags})
endif()
