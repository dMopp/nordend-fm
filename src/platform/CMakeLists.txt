add_library(nordend_platform
    src/about.cpp
    src/applications.cpp
    src/bus_loop.cpp
    src/desktop_entry.cpp
    src/exec.cpp
    src/gvfs.cpp
    src/launcher.cpp
    src/opener.cpp
    src/portal.cpp
    src/udisks.cpp
    src/volumes.cpp)
add_library(nordend::platform ALIAS nordend_platform)

target_include_directories(nordend_platform PUBLIC include)
# thumbs: file_uri(), the file:// URIs %u and %U take, escaped as GLib does.
target_link_libraries(nordend_platform
    PUBLIC nordend::core nordend::fs
    PRIVATE nordend::thumbs nordend::warnings PkgConfig::SDBUSCPP)
