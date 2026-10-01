# vcam_set_warnings(<target>)
# Enables a strict, but practical, warning set on one of our own targets.
# Third-party headers (FFmpeg, GoogleTest) come in as SYSTEM includes through
# their imported targets, so their warnings are not shown.
function(vcam_set_warnings target)
    target_compile_options(${target} PRIVATE
        -Wall                 # the common set
        -Wextra               # more common mistakes
        -Wpedantic            # non-standard C++
        -Wshadow              # a variable hides another one
        -Wconversion          # implicit narrowing (e.g. int64 -> int)
        -Wsign-conversion     # implicit signed <-> unsigned
        -Wnon-virtual-dtor    # polymorphic class without virtual destructor
        -Wold-style-cast      # (int)x instead of static_cast<int>(x)
        -Woverloaded-virtual  # a function hides a base-class virtual
        -Wnull-dereference
        -Wdouble-promotion    # float silently promoted to double
        -Wformat=2            # printf format checks
        -Wimplicit-fallthrough)
    if(VCAM_WARNINGS_AS_ERRORS)
        target_compile_options(${target} PRIVATE -Werror)
    endif()
endfunction()
