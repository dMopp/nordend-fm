#include "nordend/fs/places.hpp"

#include <chrono>
#include <filesystem>
#include <fstream>
#include <functional>
#include <optional>
#include <string>
#include <vector>
#include <xkbcommon/xkbcommon-keysyms.h>

#include <doctest/doctest.h>

#include "nordend/core/event_loop.hpp"
#include "nordend/core/file.hpp"
#include "nordend/core/worker_pool.hpp"
#include "nordend/fs/directory_watcher.hpp"
#include "nordend/fs/mime.hpp"
#include "nordend/fs/trash.hpp"
#include "nordend/ui/icons.hpp"
#include "nordend/ui/text.hpp"
#include "nordend/views/browser.hpp"
#include "nordend/views/sidebar.hpp"
#include "temp_dir.hpp"

using nordend::core::EventLoop;
using nordend::fs::Place;
using nordend::fs::PlaceSources;
using nordend::fs::UserDir;
using nordend::test::TempDir;
using nordend::views::Browser;
using nordend::views::place_holding;
using namespace std::chrono_literals;
namespace fs = std::filesystem;

namespace {

bool run_until(EventLoop& loop, const std::function<bool()>& done) {
    bool ok = false;
    const auto poll = loop.add_timer(1ms, 1ms, [&] {
        if (done()) {
            ok = true;
            loop.quit(0);
        }
    });
    const auto watchdog = loop.add_timer(5s, 0ms, [&loop] { loop.quit(1); });
    loop.run();
    loop.remove(poll);
    loop.remove(watchdog);
    return ok;
}

std::vector<std::string> labels(const std::vector<Place>& places) {
    std::vector<std::string> out;
    out.reserve(places.size());
    for (const Place& place : places) {
        out.push_back(place.label);
    }
    return out;
}

nordend::ui::KeyEvent key(xkb_keysym_t keysym, bool ctrl = false) {
    nordend::ui::KeyEvent event;
    event.keysym = keysym;
    event.modifiers.ctrl = ctrl;
    return event;
}

// A home with Pictures and Downloads, a config dir, an empty mount table.
struct Home {
    TempDir dir;
    PlaceSources sources{.home = dir.path / "home",
                         .config_home = dir.path / "home" / ".config",
                         .mountinfo = dir.path / "mountinfo"};

    Home() {
        fs::create_directories(sources.home / "Pictures");
        fs::create_directories(sources.home / "Downloads");
        fs::create_directories(sources.config_home);
        std::ofstream{sources.mountinfo} << "";
    }
};

}  // namespace

TEST_CASE("places: user-dirs.dirs") {
    const fs::path home{"/home/me"};
    const std::vector<UserDir> dirs = nordend::fs::parse_user_dirs(
        "# written by xdg-user-dirs-update\n"
        "XDG_DESKTOP_DIR=\"$HOME/Scrivania\"\n"
        "XDG_DOWNLOAD_DIR=\"$HOME/Scaricati\"\n"
        "XDG_TEMPLATES_DIR=\"$HOME/\"\n"
        "XDG_PICTURES_DIR=\"/data/photos\"\n"
        "XDG_MUSIC_DIR=\"relative/not/allowed\"\n"
        "  XDG_VIDEOS_DIR = \"$HOME/Video\"  \n"
        "garbage\n",
        home);
    CHECK(dirs == std::vector<UserDir>{{.key = "DESKTOP", .path = "/home/me/Scrivania"},
                                       {.key = "DOWNLOAD", .path = "/home/me/Scaricati"},
                                       {.key = "PICTURES", .path = "/data/photos"},
                                       {.key = "VIDEOS", .path = "/home/me/Video"}});
}

TEST_CASE("places: GTK bookmarks") {
    const std::vector<Place> places = nordend::fs::parse_bookmarks(
        "file:///run/media/me/DATA/fotografia Photos\n"
        "file:///tmp/with%20space\n"
        "sftp://host/home/me Remote\n"
        "file:///bad%zzescape\n"
        "\n");
    REQUIRE(places.size() == 4);
    CHECK(places[0].label == "Photos");
    CHECK(places[0].path == "/run/media/me/DATA/fotografia");
    CHECK(places[0].section == Place::Section::bookmarks);
    CHECK(places[0].uri.empty());
    CHECK(places[1].label == "with space");
    CHECK(places[1].path == "/tmp/with space");
    // A remote one has no path until gvfs mounts it.
    CHECK(places[2].label == "Remote");
    CHECK(places[2].uri == "sftp://host/home/me");
    CHECK(places[2].path.empty());
    CHECK_FALSE(places[2].disk.mounted);
    CHECK(places[3].path == "/bad%zzescape");
}

TEST_CASE("bookmarks: added at the end as GLib writes them, removed, the rest kept") {
    const std::string text =
        "sftp://host/home/me Remote\n"
        "file:///run/media/me/DATA/fotografia Photos\n"
        "file:///tmp/with%20space";  // no newline at the end
    const std::optional<std::string> added =
        nordend::fs::add_bookmark(text, "/home/me/Viaggi/naïve 2026/");
    REQUIRE(added);
    CHECK(added.value_or("") == text + "\nfile:///home/me/Viaggi/na%C3%AFve%202026\n");
    // Already there, with or without the slash, as written or decoded.
    CHECK_FALSE(nordend::fs::add_bookmark(text, "/tmp/with space"));
    CHECK_FALSE(nordend::fs::add_bookmark(text, "/run/media/me/DATA/fotografia/"));
    CHECK(nordend::fs::parse_bookmarks(added.value_or("")).back().path ==
          "/home/me/Viaggi/naïve 2026");

    const std::optional<std::string> removed =
        nordend::fs::remove_bookmark(added.value_or(""), "/run/media/me/DATA/fotografia");
    CHECK(removed.value_or("") ==
          "sftp://host/home/me Remote\n"
          "file:///tmp/with%20space\n"
          "file:///home/me/Viaggi/na%C3%AFve%202026\n");
    CHECK_FALSE(nordend::fs::remove_bookmark(text, "/elsewhere"));
    CHECK(nordend::fs::add_bookmark("", "/a") == std::optional<std::string>{"file:///a\n"});
}

TEST_CASE("bookmarks: written all at once, the folder made, the permissions kept") {
    const TempDir dir;
    const std::filesystem::path file = dir.path / "config" / "gtk-3.0" / "bookmarks";
    CHECK_FALSE(nordend::core::write_file_atomically(file, "file:///a\n"));
    CHECK(nordend::core::read_text_file(file).text == "file:///a\n");
    CHECK((std::filesystem::status(file).permissions() & std::filesystem::perms::all) ==
          (std::filesystem::perms::owner_read | std::filesystem::perms::owner_write |
           std::filesystem::perms::group_read | std::filesystem::perms::others_read));
    std::filesystem::permissions(
        file, std::filesystem::perms::owner_read | std::filesystem::perms::owner_write);
    CHECK_FALSE(nordend::core::write_file_atomically(file, "file:///b\n"));
    CHECK(nordend::core::read_text_file(file).text == "file:///b\n");
    CHECK((std::filesystem::status(file).permissions() & std::filesystem::perms::all) ==
          (std::filesystem::perms::owner_read | std::filesystem::perms::owner_write));
    // Nothing left beside it.
    std::size_t files = 0;
    for ([[maybe_unused]] const auto& entry :
         std::filesystem::directory_iterator{file.parent_path()}) {
        ++files;
    }
    CHECK(files == 1);
    // A folder where the file should be: an error, nothing else.
    CHECK(nordend::core::write_file_atomically(dir.path / "config", "x"));
}

TEST_CASE("places: mounts") {
    const std::vector<Place> places = nordend::fs::parse_mounts(
        "22 1 0:21 / / rw,relatime - btrfs /dev/nvme0n1p2 rw\n"
        "38 27 8:0 / /run/media/me/DATA rw,noatime - ext4 /dev/sda rw\n"
        "39 27 8:17 / /run/media/me/MY\\040STICK rw - vfat /dev/sdb1 rw\n"
        "40 27 8:18 / /mnt/backup rw - ext4 /dev/sdc1 rw\n"
        "41 27 8:18 / /mnt/backup rw - ext4 /dev/sdc1 rw\n"
        "42 27 0:5 / /media rw - tmpfs tmpfs rw\n"
        "43 27 0:6 / /home/me/mnt/x rw - tmpfs tmpfs rw\n"
        "short line\n");
    CHECK(labels(places) == std::vector<std::string>{"DATA", "MY STICK", "backup"});
    CHECK(places[1].path == "/run/media/me/MY STICK");
    CHECK(places[0].section == Place::Section::devices);
}

TEST_CASE("places: load, without user-dirs.dirs") {
    Home home;
    fs::create_directories(home.sources.config_home / "gtk-3.0");
    std::ofstream{home.sources.config_home / "gtk-3.0" / "bookmarks"} << "file:///srv Served\n";
    std::ofstream{home.sources.mountinfo}
        << "38 27 8:0 / /run/media/me/DATA rw - ext4 /dev/sda rw\n";
    const std::vector<Place> places = nordend::fs::load_places(home.sources);
    // The usual names that exist, in the sidebar's order.
    CHECK(labels(places) == std::vector<std::string>{"Home", "Downloads", "Pictures", "File system",
                                                     "Trash", "Served", "DATA"});
    CHECK(places[0].path == home.sources.home);
    CHECK(places[3].path == "/");
    CHECK(places[4].path == nordend::fs::trash_path());
}

TEST_CASE("places: load, with user-dirs.dirs") {
    Home home;
    fs::create_directories(home.sources.home / "Immagini");
    std::ofstream{home.sources.config_home / "user-dirs.dirs"}
        << "XDG_PICTURES_DIR=\"$HOME/Immagini\"\nXDG_MUSIC_DIR=\"$HOME/Musica\"\n";
    // Downloads exists but the file does not name it; Musica is named but missing.
    CHECK(labels(nordend::fs::load_places(home.sources)) ==
          std::vector<std::string>{"Home", "Immagini", "File system", "Trash"});
}

TEST_CASE("places: the watcher follows the bookmarks") {
    Home home;
    fs::create_directories(home.sources.config_home / "gtk-3.0");
    EventLoop loop;
    int changes = 0;
    nordend::fs::PlacesWatcher watcher{loop, home.sources, [&] { ++changes; }};
    CHECK(watcher.places().size() == 5);
    std::ofstream{home.sources.config_home / "gtk-3.0" / "bookmarks"} << "file:///srv\n";
    CHECK(run_until(loop, [&] { return changes > 0; }));
    CHECK(labels(watcher.places()).back() == "srv");
}

TEST_CASE("places: icons exist in the icon font") {
    nordend::ui::FontSystem fonts;
    nordend::ui::IconSet icons{fonts};
    REQUIRE(icons.available());
    Home home;
    std::ofstream{home.sources.mountinfo} << "38 27 8:0 / /mnt/x rw - ext4 /dev/sda rw\n";
    std::ofstream{home.sources.config_home / "user-dirs.dirs"}
        << "XDG_DESKTOP_DIR=\"$HOME/a\"\nXDG_DOCUMENTS_DIR=\"$HOME/b\"\n"
           "XDG_DOWNLOAD_DIR=\"$HOME/c\"\nXDG_MUSIC_DIR=\"$HOME/d\"\n"
           "XDG_PICTURES_DIR=\"$HOME/e\"\nXDG_VIDEOS_DIR=\"$HOME/f\"\n";
    for (const char* name : {"a", "b", "c", "d", "e", "f"}) {
        fs::create_directories(home.sources.home / name);
    }
    fs::create_directories(home.sources.config_home / "gtk-3.0");
    std::ofstream{home.sources.config_home / "gtk-3.0" / "bookmarks"} << "file:///srv\n";
    const std::vector<Place> places = nordend::fs::load_places(home.sources);
    CHECK(places.size() == 11);
    for (const Place& place : places) {
        CHECK_MESSAGE(icons.has(place.icon), place.icon);
    }
    CHECK(icons.has("trash-filled"));  // the trash's when it is not empty
}

TEST_CASE("places: the place holding a folder") {
    const std::vector<Place> places{
        {.label = "Home", .path = "/home/me", .icon = "folder"},
        {.label = "Pictures", .path = "/home/me/Pictures", .icon = "folder"},
        {.label = "File system", .path = "/", .icon = "folder"},
        {.label = "Pic", .path = "/home/me/Pic", .icon = "folder"},
    };
    CHECK(place_holding(places, "/home/me/Pictures/2026") == 1U);
    CHECK(place_holding(places, "/home/me/Pictures/") == 1U);
    CHECK(place_holding(places, "/home/me/Pictures2") == 0U);  // whole names only
    CHECK(place_holding(places, "/home/me") == 0U);
    CHECK(place_holding(places, "/etc") == 2U);
    CHECK_FALSE(place_holding(std::vector<Place>{places[1]}, "/etc").has_value());
}

TEST_CASE("places: the sidebar opens a place from the keyboard") {
    TempDir dir;
    fs::create_directories(dir.path / "a" / "inner");
    fs::create_directories(dir.path / "b");
    EventLoop loop;
    nordend::core::WorkerPool pool{loop, 2};
    nordend::fs::DirectoryWatcher watcher{loop};
    Browser browser{loop,    pool,
                    watcher, nordend::fs::MimeDatabase::load(),
                    [] {},   [](nordend::ui::CursorShape /*shape*/) {}};
    browser.set_places({{.label = "a", .path = dir.path / "a", .icon = "folder"},
                        {.label = "b", .path = dir.path / "b", .icon = "folder"}});
    browser.open(dir.path / "a");
    browser.set_mode(Browser::Mode::grid);
    REQUIRE(run_until(loop, [&] { return !browser.loading(); }));

    const auto now = Browser::Clock::now();
    browser.key(key(XKB_KEY_F6), now);
    CHECK(browser.sidebar().focused());
    // Without a draw the sidebar does not know the current place yet: it starts on the first.
    CHECK(browser.sidebar().cursor() == 0U);
    browser.key(key(XKB_KEY_Down), now);
    CHECK(browser.sidebar().cursor() == 1U);
    CHECK_FALSE(browser.grid().folder()->cursor().has_value());  // the view did not move
    browser.key(key(XKB_KEY_Return), now);
    CHECK_FALSE(browser.sidebar().focused());
    CHECK(browser.mode() == Browser::Mode::grid);
    CHECK(browser.grid().folder()->path() == dir.path / "b");

    browser.key(key(XKB_KEY_F6), now);
    browser.key(key(XKB_KEY_Escape), now);
    CHECK_FALSE(browser.sidebar().focused());
    CHECK(browser.grid().folder()->path() == dir.path / "b");

    // Ctrl+B hides it and takes the keyboard back; F6 shows it again.
    browser.key(key(XKB_KEY_F6), now);
    browser.key(key(XKB_KEY_b, true), now);
    CHECK_FALSE(browser.sidebar_visible());
    CHECK_FALSE(browser.sidebar().focused());
    browser.key(key(XKB_KEY_F6), now);
    CHECK(browser.sidebar_visible());
    CHECK(browser.sidebar().focused());
}
