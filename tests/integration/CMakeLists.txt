# Cross-module / tool-level tests. Run only these with:
#   ctest --test-dir build -L integration --output-on-failure

# Phase 1 acceptance: frame count and every timestamp equal ffprobe's.
if(VCAM_BUILD_TOOLS)
    add_test(NAME integration.probe_source_matches_ffprobe
             COMMAND bash ${CMAKE_CURRENT_SOURCE_DIR}/compare_with_ffprobe.sh
                     $<TARGET_FILE:probe_source> ${VCAM_TEST_MEDIA_DIR})
    set_tests_properties(integration.probe_source_matches_ffprobe PROPERTIES
        LABELS integration
        FIXTURES_REQUIRED test_media)

    # Error handling: these must fail with a clean error (non-zero exit), not crash.
    foreach(bad_input not_a_video.txt audio_only.m4a does_not_exist.mp4)
        add_test(NAME integration.probe_source_rejects_${bad_input}
                 COMMAND probe_source ${VCAM_TEST_MEDIA_DIR}/${bad_input})
        set_tests_properties(integration.probe_source_rejects_${bad_input} PROPERTIES
            LABELS integration
            FIXTURES_REQUIRED test_media
            WILL_FAIL TRUE)
    endforeach()

    # Odd frame size with YUYV must be refused with an explanation.
    add_test(NAME integration.probe_source_rejects_odd_size_yuyv
             COMMAND probe_source --pixel-format yuyv ${VCAM_TEST_MEDIA_DIR}/odd.mkv)
    set_tests_properties(integration.probe_source_rejects_odd_size_yuyv PROPERTIES
        LABELS integration
        FIXTURES_REQUIRED test_media
        WILL_FAIL TRUE)
endif()

# The complete application, driven like a user would (no device needed).
add_test(NAME integration.virtual_camera_cli
         COMMAND bash ${CMAKE_CURRENT_SOURCE_DIR}/test_cli.sh
                 $<TARGET_FILE:virtual-camera> ${VCAM_TEST_MEDIA_DIR} ${PROJECT_SOURCE_DIR})
set_tests_properties(integration.virtual_camera_cli PROPERTIES
    LABELS integration
    FIXTURES_REQUIRED test_media
    TIMEOUT 120)

# Phase 8: a Python producer pushes frames through the Unix socket.
add_test(NAME integration.push_input_python
         COMMAND bash ${CMAKE_CURRENT_SOURCE_DIR}/test_push.sh $<TARGET_FILE:virtual-camera> ${PROJECT_SOURCE_DIR})
set_tests_properties(integration.push_input_python PROPERTIES LABELS integration TIMEOUT 60)
