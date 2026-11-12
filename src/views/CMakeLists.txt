# fzy (third_party/fzy): the fuzzy matcher of the command palette. Compiled on its own, as C,
# without our warnings.
add_library(nordend_fzy STATIC ${PROJECT_SOURCE_DIR}/third_party/fzy/src/match.c)
target_include_directories(nordend_fzy SYSTEM PUBLIC ${PROJECT_SOURCE_DIR}/third_party)
target_compile_options(nordend_fzy PRIVATE -w)
if(NORDEND_SANITIZER)
    target_compile_options(nordend_fzy PRIVATE -fsanitize=${NORDEND_SANITIZER})
endif()

add_library(nordend_views
    src/breadcrumb.cpp
    src/browser.cpp
    src/browser_palette.cpp
    src/column.cpp
    src/column_view.cpp
    src/command_palette.cpp
    src/confirm_dialog.cpp
    src/conflict_dialog.cpp
    src/context_menu.cpp
    src/details_view.cpp
    src/file_clipboard.cpp
    src/draw_helpers.cpp
    src/folder.cpp
    src/format.cpp
    src/file_finder.cpp
    src/fuzzy.cpp
    src/grid_view.cpp
    src/info_pane.cpp
    src/name_editor.cpp
    src/pane.cpp
    src/password_dialog.cpp
    src/shortcuts.cpp
    src/shortcuts_overlay.cpp
    src/sidebar.cpp
    src/status_bar.cpp
    src/state.cpp
    src/tab_bar.cpp
    src/title_bar.cpp
    src/quick_look.cpp
    src/text_selection.cpp
    src/thumbnails.cpp
    src/undo.cpp)
add_library(nordend::views ALIAS nordend_views)

target_include_directories(nordend_views PUBLIC include)
target_link_libraries(nordend_views
    PUBLIC nordend::core nordend::ui nordend::fs nordend::theme nordend::thumbs nordend::preview
    PRIVATE nordend::warnings PkgConfig::TOMLPLUSPLUS nordend_fzy)
