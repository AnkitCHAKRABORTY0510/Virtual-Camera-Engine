# Helper functions so each module's CMakeLists.txt stays a few lines long.

# vcam_add_module(<name> SOURCES <files...> [PUBLIC_DEPS <targets...>] [PRIVATE_DEPS <targets...>])
#
# Creates the static library `vcam_<name>` (alias `vcam::<name>`) from
# modules/<name>/src, exposing modules/<name>/include as its public include
# directory. Modules that do not link this library cannot include its headers.
function(vcam_add_module name)
    cmake_parse_arguments(ARG "" "" "SOURCES;PUBLIC_DEPS;PRIVATE_DEPS" ${ARGN})
    set(target vcam_${name})
    add_library(${target} STATIC ${ARG_SOURCES})
    add_library(vcam::${name} ALIAS ${target})
    target_include_directories(${target} PUBLIC ${CMAKE_CURRENT_SOURCE_DIR}/include)
    target_link_libraries(${target} PUBLIC ${ARG_PUBLIC_DEPS} PRIVATE ${ARG_PRIVATE_DEPS})
    vcam_set_warnings(${target})
endfunction()

# vcam_add_module_test(<module> SOURCES <files...> [LINK <targets...>] [NEEDS_MEDIA])
#
# Creates the executable `test_<module>` (in build/bin) and registers every
# GoogleTest case with CTest under the label <module>, so that
#     ctest --test-dir build -L <module>
# runs only this module's tests. NEEDS_MEDIA makes the tests depend on the
# generated test videos and passes their location in VCAM_TEST_MEDIA_DIR.
function(vcam_add_module_test module)
    if(NOT VCAM_BUILD_TESTS)
        return()
    endif()
    cmake_parse_arguments(ARG "NEEDS_MEDIA" "" "SOURCES;LINK" ${ARGN})
    set(target test_${module})
    add_executable(${target} ${ARG_SOURCES})
    target_link_libraries(${target} PRIVATE vcam_${module} ${ARG_LINK} GTest::gtest GTest::gtest_main)
    vcam_set_warnings(${target})

    set(test_properties LABELS ${module})
    if(ARG_NEEDS_MEDIA)
        list(APPEND test_properties
            FIXTURES_REQUIRED test_media
            ENVIRONMENT VCAM_TEST_MEDIA_DIR=${VCAM_TEST_MEDIA_DIR})
    endif()

    # PRE_TEST: list the test cases when ctest runs, not at build time
    # (keeps builds fast and works with sanitizers).
    gtest_discover_tests(${target}
        DISCOVERY_MODE PRE_TEST
        TEST_PREFIX "${module}."
        PROPERTIES ${test_properties})
endfunction()
