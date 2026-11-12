add_executable(nordend-tests
    main.cpp
    about_test.cpp
    app_icon_test.cpp
    breadcrumb_test.cpp
    clipboard_test.cpp
    column_view_test.cpp
    command_palette_test.cpp
    config_test.cpp
    desktop_palette_test.cpp
    context_menu_test.cpp
    details_view_test.cpp
    devices_test.cpp
    engines_test.cpp
    drag_drop_test.cpp
    directory_model_test.cpp
    directory_test.cpp
    dng_test.cpp
    event_loop_test.cpp
    file_watcher_test.cpp
    format_test.cpp
    icon_test.cpp
    helpers_test.cpp
    sandbox_test.cpp
    tiff_test.cpp
    image_test.cpp
    load_test.cpp
    memory_test.cpp
    mime_test.cpp
    navigation_test.cpp
    palette_test.cpp
    paste_test.cpp
    places_test.cpp
    rclone_test.cpp
    remote_test.cpp
    pointer_shapes_test.cpp
    tabs_test.cpp
    text_test.cpp
    text_selection_test.cpp
    platform_test.cpp
    preview_test.cpp
    quick_look_test.cpp
    rename_test.cpp
    renderer_runs_test.cpp
    scroller_test.cpp
    shortcuts_test.cpp
    sort_test.cpp
    syntax_test.cpp
    syntax_colors_test.cpp
    state_test.cpp
    thumbs_test.cpp
    title_bar_test.cpp
    erase_test.cpp
    erase_view_test.cpp
    file_finder_test.cpp
    transfer_test.cpp
    trash_test.cpp
    trash_view_test.cpp
    undo_test.cpp
    worker_pool_test.cpp
    xcursor_test.cpp)
target_link_libraries(nordend-tests PRIVATE nordend::core nordend::theme nordend::fs nordend::views nordend::picture nordend::formats nordend::formats::image nordend::formats::text nordend::text nordend::thumbs nordend::preview nordend::engines nordend::isolate nordend::platform doctest::doctest nordend::warnings)

# The helper protocol's private header, for its tests; the image family's private readers (TIFF,
# DNG, lossless JPEG), for theirs.
target_include_directories(nordend-tests PRIVATE ${PROJECT_SOURCE_DIR}/src/isolate/src
                                                 ${PROJECT_SOURCE_DIR}/src/formats/image/src)

# LibRaw, when installed, as the reference for the DNG reader's values (tests only: never linked
# into Nordend or its helper).
pkg_check_modules(LIBRAW QUIET IMPORTED_TARGET libraw_r)
if(LIBRAW_FOUND)
    target_sources(nordend-tests PRIVATE dng_libraw_test.cpp)
    target_link_libraries(nordend-tests PRIVATE PkgConfig::LIBRAW)
endif()

# exiv2, when installed, as the reference for the metadata readers that replace it (ROADMAP 2e;
# tests only: never linked into Nordend or its helper).
pkg_check_modules(EXIV2_REFERENCE QUIET IMPORTED_TARGET exiv2)
if(EXIV2_REFERENCE_FOUND)
    target_sources(nordend-tests PRIVATE exiv2_reference.cpp exiv2_reference_test.cpp)
    target_link_libraries(nordend-tests PRIVATE PkgConfig::EXIV2_REFERENCE)
    # Nordend's reader against exiv2's, timed (tools, not tests).
    add_executable(nordend-bench-previews bench_previews.cpp exiv2_reference.cpp)
    target_include_directories(nordend-bench-previews PRIVATE ${PROJECT_SOURCE_DIR}/src/formats/image/src)
    target_link_libraries(nordend-bench-previews
        PRIVATE nordend::formats::image PkgConfig::EXIV2_REFERENCE nordend::warnings)
endif()

# A preview helper that misbehaves on purpose (crash, hang, nonsense), and the real one, for
# tests/helpers_test.cpp.
add_executable(nordend-fake-preview fake_preview_helper.cpp
                                    ${PROJECT_SOURCE_DIR}/src/isolate/src/protocol.cpp)
target_include_directories(nordend-fake-preview PRIVATE ${PROJECT_SOURCE_DIR}/src/isolate/src)
target_link_libraries(nordend-fake-preview PRIVATE nordend::picture nordend::text nordend::warnings)

# The sandbox probe: the preview helper's cage, then one forbidden (or allowed) thing.
add_executable(nordend-sandbox-probe sandbox_probe.cpp
                                     ${PROJECT_SOURCE_DIR}/src/isolate/src/sandbox.cpp)
target_include_directories(nordend-sandbox-probe PRIVATE ${PROJECT_SOURCE_DIR}/src/isolate/src)
target_link_libraries(nordend-sandbox-probe PRIVATE nordend::warnings)
add_dependencies(nordend-tests nordend-preview nordend-fake-preview nordend-sandbox-probe)
# The MIME database the tests read: shared-mime-info as pkg-config finds it at build time, not
# whatever $XDG_DATA_DIRS says when they run (NixOS keeps it outside /usr/share, Flatpak adds
# its own). tests/main.cpp points $XDG_DATA_DIRS at it.
pkg_get_variable(NORDEND_MIME_PREFIX shared-mime-info prefix)
if(NOT NORDEND_MIME_PREFIX OR NOT EXISTS "${NORDEND_MIME_PREFIX}/share/mime/globs2")
    message(FATAL_ERROR "The tests need shared-mime-info (its pkg-config file and "
                        "share/mime/globs2); install it, or configure with "
                        "-DNORDEND_BUILD_TESTS=OFF")
endif()
# The shipped example config must parse clean and match the built-in defaults.
target_compile_definitions(nordend-tests
    PRIVATE NORDEND_EXAMPLE_CONFIG="${PROJECT_SOURCE_DIR}/docs/config.example.toml"
            NORDEND_TEST_DATA="${PROJECT_SOURCE_DIR}/tests/data"
            NORDEND_APP_ICON="${PROJECT_SOURCE_DIR}/data/nordend.svg"
            NORDEND_ENGINES="${PROJECT_SOURCE_DIR}/data/engines"
            NORDEND_MIME_DATA="${NORDEND_MIME_PREFIX}/share"
            NORDEND_PREVIEW_HELPER="$<TARGET_FILE:nordend-preview>"
            NORDEND_FAKE_HELPER="$<TARGET_FILE:nordend-fake-preview>"
            NORDEND_SANDBOX_PROBE="$<TARGET_FILE:nordend-sandbox-probe>")
if(NORDEND_SANITIZER)
    target_sources(nordend-tests PRIVATE ${PROJECT_SOURCE_DIR}/src/core/src/lsan_suppressions.cpp)
endif()

add_test(NAME nordend-tests COMMAND nordend-tests)
