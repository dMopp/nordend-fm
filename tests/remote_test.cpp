#include <chrono>
#include <filesystem>
#include <functional>
#include <optional>
#include <string>
#include <utility>
#include <vector>
#include <xkbcommon/xkbcommon-keysyms.h>

#include <doctest/doctest.h>

#include "nordend/core/event_loop.hpp"
#include "nordend/core/worker_pool.hpp"
#include "nordend/fs/directory_watcher.hpp"
#include "nordend/fs/mime.hpp"
#include "nordend/fs/places.hpp"
#include "nordend/platform/gvfs.hpp"
#include "nordend/ui/seat.hpp"
#include "nordend/ui/text_field.hpp"
#include "nordend/views/browser.hpp"
#include "nordend/views/password_dialog.hpp"
#include "temp_dir.hpp"

using nordend::core::EventLoop;
using nordend::fs::parse_remote_uri;
using nordend::fs::Place;
using nordend::fs::RemoteUri;
using nordend::platform::gvfs_location;
using nordend::platform::GvfsLocation;
using nordend::platform::GvfsMount;
using nordend::platform::GvfsMountSpec;
using nordend::test::TempDir;
using nordend::views::Browser;
using nordend::views::Login;
using nordend::views::LoginRequest;
using nordend::views::Remotes;
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

nordend::ui::KeyEvent key(xkb_keysym_t keysym, std::string text = {}, bool shift = false) {
    nordend::ui::KeyEvent event;
    event.keysym = keysym;
    event.text = std::move(text);
    event.modifiers.shift = shift;
    return event;
}

void type(nordend::views::PasswordDialog& dialog, std::string_view text) {
    for (const char c : text) {
        dialog.key(key(static_cast<xkb_keysym_t>(c), std::string(1, c)));
    }
}

// gvfs_location of a URI that parses.
GvfsLocation location_of(std::string_view uri) {
    const std::optional<RemoteUri> remote = parse_remote_uri(uri);
    REQUIRE(remote);
    const std::optional<GvfsLocation> location = gvfs_location(remote.value_or(RemoteUri{}));
    REQUIRE(location);
    return location.value_or(GvfsLocation{});
}

GvfsMount share_mount(std::string server, std::string share, fs::path fuse) {
    return {.dbus_id = ":1.42",
            .object_path = "/org/gtk/vfs/mount/1",
            .name = share + " on " + server,
            .fuse = std::move(fuse),
            .spec = {.type = "smb-share",
                     .items = {{"server", std::move(server)}, {"share", std::move(share)}},
                     .prefix = "/"}};
}

}  // namespace

TEST_CASE("remote: URIs a bookmark names, decoded, with user, port and IPv6 hosts") {
    CHECK(parse_remote_uri("smb://fileshare.example/Public") ==
          RemoteUri{.scheme = "smb",
                    .user = "",
                    .host = "fileshare.example",
                    .port = -1,
                    .path = "/Public"});
    CHECK(
        parse_remote_uri("SMB://WORK;me@nas:4450/My%20Files/") ==
        RemoteUri{
            .scheme = "smb", .user = "WORK;me", .host = "nas", .port = 4450, .path = "/My Files/"});
    CHECK(parse_remote_uri("sftp://me@[fd00::1]:2222") ==
          RemoteUri{.scheme = "sftp", .user = "me", .host = "fd00::1", .port = 2222, .path = "/"});
    CHECK(parse_remote_uri("smb://nas/share?query#fragment")->path == "/share");
    CHECK_FALSE(parse_remote_uri("file:///home/me"));
    CHECK(parse_remote_uri("davs://me@cloud.example/remote.php/dav") ==
          RemoteUri{.scheme = "davs",
                    .user = "me",
                    .host = "cloud.example",
                    .port = -1,
                    .path = "/remote.php/dav"});
    CHECK_FALSE(parse_remote_uri("afp://host/pub"));  // not one Nordend maps to a mount
    CHECK_FALSE(parse_remote_uri("smb:///share"));    // no host
    CHECK_FALSE(parse_remote_uri("smb://nas:99999/share"));
    CHECK_FALSE(parse_remote_uri("smb://nas:port/share"));
    CHECK_FALSE(parse_remote_uri("smb://[fd00::1/share"));
    CHECK_FALSE(parse_remote_uri("not a uri"));
}

TEST_CASE("remote: the label of a remote bookmark without one") {
    CHECK(nordend::fs::remote_label(*parse_remote_uri("smb://fileshare.example/Public")) ==
          "Public on fileshare.example");
    CHECK(nordend::fs::remote_label(*parse_remote_uri("smb://nas/Media/Photos/")) ==
          "Photos on nas");
    CHECK(nordend::fs::remote_label(*parse_remote_uri("sftp://me@host")) == "host");
}

TEST_CASE("remote: bookmarks listed without a path, removed by their URI, the rest kept") {
    const std::string text =
        "smb://fileshare.example/Dokumente\n"
        "file:///tmp/a A\n"
        "smb://fileshare.example/DMS-Input Scanner\n"
        "afp://host/pub\n"
        "sftp://me@host/srv\n";
    const std::vector<Place> places = nordend::fs::parse_bookmarks(text);
    REQUIRE(places.size() == 4);
    CHECK(places[0].label == "Dokumente on fileshare.example");
    CHECK(places[0].uri == "smb://fileshare.example/Dokumente");
    CHECK(places[0].icon == "server");
    CHECK(places[0].path.empty());
    CHECK_FALSE(places[0].disk.mounted);
    CHECK(places[1].path == "/tmp/a");
    CHECK(places[2].label == "Scanner");
    CHECK(places[3].label == "srv on host");

    CHECK(nordend::fs::remove_remote_bookmark(text, "smb://fileshare.example/DMS-Input") ==
          std::optional<std::string>{"smb://fileshare.example/Dokumente\n"
                                     "file:///tmp/a A\n"
                                     "afp://host/pub\n"
                                     "sftp://me@host/srv\n"});
    CHECK_FALSE(nordend::fs::remove_remote_bookmark(text, "smb://fileshare.example/DMS"));
    CHECK_FALSE(nordend::fs::remove_remote_bookmark(text, ""));
    // A local folder's removal leaves remote lines alone.
    CHECK(nordend::fs::remove_bookmark(text, "/tmp/a")
              .value_or("")
              .starts_with("smb://fileshare.example/Dokumente\nsmb://"));
}

TEST_CASE("gvfs: the mount of an smb URI, as gvfs's own mapper makes it") {
    // A share: server and share case-folded, the rest the path inside.
    CHECK(location_of("smb://FileShare.Example/Public/Scans/2026/") ==
          GvfsLocation{.spec = {.type = "smb-share",
                                .items = {{"server", "fileshare.example"}, {"share", "public"}},
                                .prefix = "/"},
                       .path = "/Scans/2026/"});
    CHECK(location_of("smb://nas/Public").path == "/");
    // "DOMAIN;user" split; the default port left out, another kept.
    CHECK(location_of("smb://WORK;me@nas:445/share").spec.items ==
          std::map<std::string, std::string>{
              {"server", "nas"}, {"share", "share"}, {"domain", "WORK"}, {"user", "me"}});
    CHECK(location_of("smb://;me@nas:4450/share").spec.items ==
          std::map<std::string, std::string>{
              {"server", "nas"}, {"share", "share"}, {"user", "me"}, {"port", "4450"}});
    // The server alone lists its shares.
    CHECK(location_of("smb://nas/").spec ==
          GvfsMountSpec{.type = "smb-server", .items = {{"server", "nas"}}, .prefix = "/"});
}

TEST_CASE("gvfs: the mount of an sftp or ftp URI: host case-folded, the default port left out") {
    CHECK(location_of("sftp://me@Host:2222/srv/www") ==
          GvfsLocation{.spec = {.type = "sftp",
                                .items = {{"host", "host"}, {"user", "me"}, {"port", "2222"}},
                                .prefix = "/"},
                       .path = "/srv/www"});
    CHECK(location_of("sftp://host:22").spec.items ==
          std::map<std::string, std::string>{{"host", "host"}});
    CHECK(location_of("ftp://anonymous@FTP.example:21/pub") ==
          GvfsLocation{.spec = {.type = "ftp",
                                .items = {{"host", "ftp.example"}, {"user", "anonymous"}},
                                .prefix = "/"},
                       .path = "/pub"});
    CHECK(location_of("ftp://ftp.example:2121").spec.items ==
          std::map<std::string, std::string>{{"host", "ftp.example"}, {"port", "2121"}});
}

TEST_CASE("gvfs: the mount of a WebDAV URI: one type, ssl for davs, the path kept") {
    CHECK(
        location_of("davs://me@cloud.example/remote.php/dav/files/me") ==
        GvfsLocation{.spec = {.type = "dav",
                              .items = {{"ssl", "true"}, {"host", "cloud.example"}, {"user", "me"}},
                              .prefix = "/"},
                     .path = "/remote.php/dav/files/me"});
    CHECK(location_of("davs://cloud.example:443/").spec.items ==
          std::map<std::string, std::string>{{"ssl", "true"}, {"host", "cloud.example"}});
    CHECK(location_of("dav://nas:8080/share").spec.items ==
          std::map<std::string, std::string>{{"ssl", "false"}, {"host", "nas"}, {"port", "8080"}});

    // Mounted under a prefix (where the share starts on the server): the folder inside it.
    const GvfsLocation wanted = location_of("davs://cloud.example/remote.php/dav/files/me/Photos");
    GvfsMount mount{.dbus_id = ":1.43",
                    .object_path = "/org/gtk/vfs/mount/2",
                    .name = "WebDAV on cloud.example",
                    .fuse = "/run/user/1000/gvfs/dav:host=cloud.example,ssl=true",
                    .spec = location_of("davs://cloud.example/").spec};
    mount.spec.prefix = "/remote.php/dav/files/me";
    const std::vector<GvfsMount> mounts{mount};
    const GvfsMount* const holding = nordend::platform::mount_holding(mounts, wanted);
    REQUIRE(holding != nullptr);
    CHECK(nordend::platform::local_path(*holding, wanted) ==
          "/run/user/1000/gvfs/dav:host=cloud.example,ssl=true/Photos");
    CHECK(nordend::platform::mount_holding(mounts, location_of("davs://cloud.example/other")) ==
          nullptr);
}

TEST_CASE("gvfs: a mount matches when it has every key wanted, and gives the local folder") {
    const GvfsLocation wanted = location_of("smb://fileshare.example/Public/Scans");
    GvfsMount mount = share_mount("fileshare.example", "public",
                                  "/run/user/1000/gvfs/smb-share:server=fileshare.example,"
                                  "share=public");
    mount.spec.items.emplace("user", "me");  // who logged in: more keys still match
    CHECK(nordend::platform::spec_matches(wanted.spec, mount.spec));
    CHECK_FALSE(nordend::platform::spec_matches(mount.spec, wanted.spec));
    CHECK_FALSE(nordend::platform::spec_matches(
        location_of("smb://fileshare.example/Dokumente").spec, mount.spec));
    CHECK_FALSE(
        nordend::platform::spec_matches(location_of("sftp://fileshare.example").spec, mount.spec));

    const std::vector<GvfsMount> mounts{share_mount("other", "public", "/x"), mount};
    const GvfsMount* const holding = nordend::platform::mount_holding(mounts, wanted);
    REQUIRE(holding != nullptr);
    CHECK(nordend::platform::local_path(*holding, wanted) ==
          "/run/user/1000/gvfs/smb-share:server=fileshare.example,share=public/Scans");
    CHECK(nordend::platform::local_path(*holding, location_of("smb://fileshare.example/public/")) ==
          "/run/user/1000/gvfs/smb-share:server=fileshare.example,share=public");
    // Without gvfsd-fuse there is no local folder.
    mount.fuse.clear();
    CHECK(nordend::platform::local_path(mount, wanted).empty());
    CHECK(nordend::platform::mount_holding({}, wanted) == nullptr);
}

TEST_CASE("gvfs: remote bookmarks get their folder while mounted, and an eject icon") {
    std::vector<Place> places = nordend::fs::parse_bookmarks(
        "file:///tmp/a\n"
        "smb://fileshare.example/Public\n"
        "smb://fileshare.example/Dokumente/Steuer Steuer\n");
    const std::vector<GvfsMount> mounts{share_mount("fileshare.example", "dokumente", "/fuse/d")};
    places = nordend::platform::with_remote_mounts(std::move(places), mounts);
    REQUIRE(places.size() == 3);
    CHECK(places[0].path == "/tmp/a");
    CHECK(places[0].disk.mounted);  // a local one stays as it is
    CHECK(places[1].path.empty());
    CHECK_FALSE(places[1].disk.mounted);
    CHECK_FALSE(places[1].disk.ejectable);
    CHECK(places[2].path == "/fuse/d/Steuer");
    CHECK(places[2].disk.mounted);
    CHECK(places[2].disk.ejectable);
    // Unmounted elsewhere: back without a path.
    places = nordend::platform::with_remote_mounts(std::move(places), {});
    CHECK(places[2].path.empty());
    CHECK_FALSE(places[2].disk.mounted);
}

TEST_CASE("text field: a masked one draws dots and does not move by words") {
    nordend::ui::TextField field{"pass word"};
    field.set_masked(true);
    CHECK(field.masked());
    nordend::ui::KeyEvent left = key(XKB_KEY_Left);
    left.modifiers.ctrl = true;
    field.key(left);
    CHECK(field.caret() == 0);  // to the start, not to "word"
    nordend::ui::KeyEvent back = key(XKB_KEY_BackSpace);
    back.modifiers.ctrl = true;
    field.key(key(XKB_KEY_End));
    field.key(back);
    CHECK(field.text().empty());
}

TEST_CASE("password dialog: the fields asked, Tab between them, Enter answers") {
    nordend::views::PasswordDialog dialog{[] {}};
    std::optional<std::optional<Login>> answered;
    dialog.open("Connect to “Public on nas”",
                {.message = "Password required for share public on nas",
                 .user = "",
                 .domain = "WORKGROUP",
                 .ask_user = true,
                 .ask_domain = true,
                 .ask_password = true,
                 .anonymous = true,
                 .saving = true},
                [&](std::optional<Login> login) { answered = std::move(login); });
    REQUIRE(dialog.is_open());
    CHECK(dialog.focused() == "user");  // no name yet: it comes first
    type(dialog, "me");
    dialog.key(key(XKB_KEY_Tab));
    CHECK(dialog.focused() == "domain");
    dialog.key(key(XKB_KEY_Tab));
    CHECK(dialog.focused() == "password");
    dialog.paste("s3cret\nnot this line");
    dialog.key(key(XKB_KEY_Tab));
    CHECK(dialog.focused() == "remember");
    dialog.key(key(XKB_KEY_space));
    dialog.key(key(XKB_KEY_ISO_Left_Tab, {}, true));
    CHECK(dialog.focused() == "password");
    dialog.key(key(XKB_KEY_Return));
    CHECK_FALSE(dialog.is_open());
    REQUIRE(answered);
    REQUIRE(answered->has_value());
    const Login& login = answered->value();
    CHECK(login.user == "me");
    CHECK(login.domain == "WORKGROUP");
    CHECK(login.password == "s3cret");
    CHECK(login.remember);
    CHECK_FALSE(login.anonymous);
}

TEST_CASE("password dialog: anonymous, Escape and a close without an answer") {
    nordend::views::PasswordDialog dialog{[] {}};
    std::optional<std::optional<Login>> answered;
    const LoginRequest request{.message = "Password required",
                               .user = "me",
                               .domain = "",
                               .ask_user = true,
                               .ask_domain = false,
                               .ask_password = true,
                               .anonymous = true,
                               .saving = false};
    const auto answer = [&](std::optional<Login> login) { answered = std::move(login); };
    dialog.open("Connect", request, answer);
    CHECK(dialog.focused() == "password");  // the name is there already
    dialog.key(key(XKB_KEY_ISO_Left_Tab, {}, true));
    dialog.key(key(XKB_KEY_ISO_Left_Tab, {}, true));
    CHECK(dialog.focused() == "mode");
    dialog.key(key(XKB_KEY_Right));
    CHECK(dialog.anonymous());
    dialog.key(key(XKB_KEY_Tab));
    CHECK(dialog.focused() == "mode");  // the fields are gone
    dialog.key(key(XKB_KEY_KP_Enter));
    REQUIRE(answered);
    CHECK(answered->value_or(Login{}).anonymous);
    CHECK(answered->value_or(Login{}).user.empty());

    answered.reset();
    dialog.open("Connect", request, answer);
    CHECK_FALSE(dialog.anonymous());  // each time as a registered user first
    dialog.key(key(XKB_KEY_Escape));
    REQUIRE(answered);
    CHECK_FALSE(answered->has_value());

    answered.reset();
    dialog.open("Connect", request, answer);
    dialog.close();
    CHECK_FALSE(dialog.is_open());
    CHECK_FALSE(answered);
}

namespace {

// A browser with a fake gvfs: a share that mounts into a folder of the test.
struct Fixture {
    TempDir dir;
    fs::path share = dir.path / "gvfs" / "smb-share:server=nas,share=public";
    EventLoop loop;
    nordend::core::WorkerPool pool{loop, 2};
    nordend::fs::DirectoryWatcher watcher{loop};
    Browser browser{loop,    pool,
                    watcher, nordend::fs::MimeDatabase::load(),
                    [] {},   [](nordend::ui::CursorShape /*shape*/) {}};
    std::vector<std::string> calls;
    bool ask{true};      // the next mount asks for a password
    std::string answer;  // what the next call answers: empty, "cancelled", another error
    std::optional<Login> login;

    Fixture() {
        fs::create_directories(share / "Scans");
        browser.set_home_trash(dir.path / "Trash");
        browser.set_remotes({
            .mount =
                [this](const std::string& uri, Remotes::Prompts prompts, Remotes::Mounted done) {
                    calls.push_back("mount " + uri);
                    const auto finish = [this, done](const std::string& error) {
                        loop.post([this, done, error] {
                            done(error.empty() ? share / "Scans" : fs::path{}, error);
                        });
                    };
                    if (!ask) {
                        finish(answer);
                        return;
                    }
                    loop.post([this, prompts = std::move(prompts), finish] {
                        prompts.login({.message = "Password required for share public on nas",
                                       .user = "",
                                       .domain = "",
                                       .ask_user = true,
                                       .ask_domain = false,
                                       .ask_password = true,
                                       .anonymous = false,
                                       .saving = false},
                                      [this, finish](std::optional<Login> given) {
                                          login = std::move(given);
                                          finish(login ? answer : std::string{"cancelled"});
                                      });
                    });
                },
            .unmount =
                [this](const std::string& uri, Remotes::Done done) {
                    calls.push_back("unmount " + uri);
                    loop.post([this, done = std::move(done)] { done(answer); });
                },
        });
    }

    [[nodiscard]] Place place(bool mounted) const {
        return {.section = Place::Section::bookmarks,
                .label = "Scans",
                .path = mounted ? share / "Scans" : fs::path{},
                .icon = "server",
                .disk = {.volume = {}, .mounted = mounted, .ejectable = mounted},
                .uri = "smb://nas/public/Scans"};
    }

    bool settle() {
        return run_until(loop, [this] { return !browser.loading(); });
    }

    [[nodiscard]] std::string message() { return browser.status_bar().message(); }

    [[nodiscard]] fs::path shown() { return browser.columns().focused_folder()->path(); }
};

}  // namespace

TEST_CASE("remote: a bookmark not mounted asks for a password on a card, then opens") {
    Fixture f;
    f.browser.open(f.dir.path);
    REQUIRE(f.settle());
    f.browser.open_place(f.place(false));
    REQUIRE(run_until(f.loop, [&f] { return f.browser.password_dialog().is_open(); }));
    CHECK(f.calls == std::vector<std::string>{"mount smb://nas/public/Scans"});
    CHECK(f.browser.password_dialog().title() == "Connect to “Scans”");
    // The card takes the keys meanwhile.
    for (const char c : std::string_view{"me"}) {
        CHECK(f.browser.key(key(static_cast<xkb_keysym_t>(c), std::string(1, c)), {}));
    }
    f.browser.key(key(XKB_KEY_Tab), {});
    for (const char c : std::string_view{"pw"}) {
        f.browser.key(key(static_cast<xkb_keysym_t>(c), std::string(1, c)), {});
    }
    f.browser.key(key(XKB_KEY_Return), {});
    REQUIRE(f.settle());
    CHECK_FALSE(f.browser.password_dialog().is_open());
    REQUIRE(f.login);
    CHECK(f.login->user == "me");
    CHECK(f.login->password == "pw");
    CHECK(f.shown() == f.share / "Scans");
    CHECK(f.message() == "“Scans” mounted");
}

TEST_CASE("remote: giving up, an error, and unmounting say how it went") {
    Fixture f;
    f.browser.open(f.dir.path);
    REQUIRE(f.settle());
    f.browser.open_place(f.place(false));
    REQUIRE(run_until(f.loop, [&f] { return f.browser.password_dialog().is_open(); }));
    f.browser.key(key(XKB_KEY_Escape), {});
    REQUIRE(f.settle());
    CHECK(f.message() == "Not connected to “Scans”");
    CHECK(f.shown() == f.dir.path);

    f.ask = false;
    f.answer = "Anonymous access denied";
    f.browser.mount_remote(f.place(false), true);
    REQUIRE(f.settle());
    CHECK(f.message() == "“Scans” could not be mounted: Anonymous access denied");

    // Unmounting leaves the folder first; the eject icon does the same.
    f.answer.clear();
    f.browser.open(f.share / "Scans");
    REQUIRE(f.settle());
    f.calls.clear();
    f.browser.eject_place(f.place(true));
    REQUIRE(f.settle());
    CHECK(f.calls == std::vector<std::string>{"unmount smb://nas/public/Scans"});
    CHECK(f.shown() != f.share / "Scans");
    CHECK(f.message() == "“Scans” unmounted");
    f.answer = "in use";
    f.browser.unmount_remote(f.place(true));
    REQUIRE(f.settle());
    CHECK(f.message() == "“Scans” is in use: close the files open from it");

    // A mounted one opens as any other place.
    f.calls.clear();
    f.browser.open_place(f.place(true));
    REQUIRE(f.settle());
    CHECK(f.calls.empty());
    CHECK(f.shown() == f.share / "Scans");
}

TEST_CASE("remote: without gvfs or rclone a remote bookmark says so") {
    TempDir dir;
    EventLoop loop;
    nordend::core::WorkerPool pool{loop, 2};
    nordend::fs::DirectoryWatcher watcher{loop};
    Browser browser{loop,    pool,
                    watcher, nordend::fs::MimeDatabase::load(),
                    [] {},   [](nordend::ui::CursorShape /*shape*/) {}};
    browser.set_home_trash(dir.path / "Trash");
    browser.open(dir.path);
    REQUIRE(run_until(loop, [&] { return !browser.loading(); }));
    browser.open_place({.section = Place::Section::bookmarks,
                        .label = "Public on nas",
                        .path = {},
                        .icon = "server",
                        .disk = {.volume = {}, .mounted = false, .ejectable = false},
                        .uri = "smb://nas/Public"});
    CHECK(browser.status_bar().message() ==
          "“Public on nas” could not be mounted: neither gvfs nor rclone is installed");
    CHECK(browser.columns().focused_folder()->path() == dir.path);
}

TEST_CASE("gvfs: the real session bus answers without starting gvfsd (reads only)") {
    EventLoop loop;
    nordend::platform::Gvfs gvfs{loop, [] {}};
    if (!gvfs.available()) {
        MESSAGE("no session bus: skipped");
        return;
    }
    // The first answers come back through the loop; most of the time nothing is mounted.
    const auto stop = loop.add_timer(300ms, 0ms, [&loop] { loop.quit(0); });
    loop.run();
    loop.remove(stop);
    for (const GvfsMount& mount : gvfs.mounts()) {
        CHECK_FALSE(mount.spec.type.empty());
        MESSAGE(mount.name << " (" << mount.spec.type << ") at " << mount.fuse.string());
    }
}
