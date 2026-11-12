// Network folders without gvfs: how a bookmark's URI becomes an rclone remote, where it is
// mounted, what rclone's output says, and the mount itself against a fake rclone.

#include "nordend/platform/rclone.hpp"

#include <chrono>
#include <cstdlib>
#include <filesystem>
#include <fstream>
#include <functional>
#include <optional>
#include <set>
#include <string>
#include <vector>

#include <doctest/doctest.h>

#include "nordend/core/event_loop.hpp"
#include "nordend/core/worker_pool.hpp"
#include "nordend/fs/places.hpp"
#include "nordend/platform/gvfs.hpp"
#include "temp_dir.hpp"

using nordend::core::EventLoop;
using nordend::fs::parse_remote_uri;
using nordend::fs::Place;
using nordend::platform::PasswordAnswer;
using nordend::platform::PasswordRequest;
using nordend::platform::Rclone;
using nordend::platform::RcloneRemote;
using nordend::test::TempDir;
using namespace std::chrono_literals;

namespace {

namespace fs = std::filesystem;

// The rclone remote of a URI that parses.
RcloneRemote remote_of(std::string_view uri) {
    const auto parsed = parse_remote_uri(uri);
    REQUIRE(parsed);
    const auto remote = nordend::platform::rclone_remote(parsed.value_or(nordend::fs::RemoteUri{}));
    REQUIRE(remote);
    return remote.value_or(RcloneRemote{});
}

// Runs the loop until done() holds or 10 s pass.
bool run_until(EventLoop& loop, const std::function<bool()>& done) {
    bool ok = false;
    const auto poll = loop.add_timer(1ms, 1ms, [&] {
        if (done()) {
            ok = true;
            loop.quit(0);
        }
    });
    const auto watchdog = loop.add_timer(10s, 0ms, [&loop] { loop.quit(1); });
    loop.run();
    loop.remove(poll);
    loop.remove(watchdog);
    return ok;
}

// A fake rclone: "obscure -" marks its input, "lsf" lets in the password "geheim" and a guest on
// the share "open", and says a host named "down" refuses connections; "mount" writes down the
// remote it got and its folder.
fs::path fake_rclone(const TempDir& dir) {
    setenv("NORDEND_FAKE_RCLONE_LOG", (dir.path / "mounts.log").c_str(), 1);
    // clang-format off
    const std::string script = R"SH(#!/bin/sh
if [ "$1" = obscure ]; then printf 'OBS:%s\n' "$(cat)"; exit 0; fi
case "$2" in
lsf)
    case "$*" in *down*)
        echo "2026/11/12 22:29:14 ERROR : error listing: couldn't connect: dial tcp: connect: connection refused"
        exit 1;;
    esac
    case "$*" in *'user="guest":open'*) echo g.txt; exit 0;; esac
    if [ "$RCLONE_WEBDAV_PASS" = "OBS:geheim" ]; then echo a.txt; exit 0; fi
    echo "2026/11/12 22:29:14 ERROR : error listing: couldn't list files: 401 Unauthorized: 401 Unauthorized"
    echo "2026/11/12 22:29:14 NOTICE: Failed to lsf with 2 errors: last error was: 401 Unauthorized"
    exit 1;;
mount)
    echo "$3 $4" >> "$NORDEND_FAKE_RCLONE_LOG"
    exit 0;;
esac
exit 2
)SH";
    // clang-format on
    const fs::path program = dir.write("rclone", script);
    fs::permissions(program, fs::perms::owner_all);
    return program;
}

}  // namespace

TEST_CASE("rclone: the remote of each scheme, as rclone's backends take it") {
    CHECK(remote_of("smb://WORK;me@NAS:4450/Public/Scans/") ==
          RcloneRemote{
              .backend = "smb",
              .options = {{"host", "NAS"}, {"port", "4450"}, {"user", "me"}, {"domain", "WORK"}},
              .path = "Public/Scans"});
    // The server alone: rclone lists its shares. The default port left out.
    CHECK(remote_of("smb://nas:445") ==
          RcloneRemote{.backend = "smb", .options = {{"host", "nas"}}, .path = ""});
    CHECK(remote_of("sftp://me@host:2222/srv/www") ==
          RcloneRemote{.backend = "sftp",
                       .options = {{"host", "host"}, {"port", "2222"}, {"user", "me"}},
                       .path = "/srv/www"});
    CHECK(remote_of("ftp://ftp.example:21/pub") ==
          RcloneRemote{.backend = "ftp", .options = {{"host", "ftp.example"}}, .path = "/pub"});
    CHECK(remote_of("davs://me@cloud.example/remote.php/dav/files/me") ==
          RcloneRemote{
              .backend = "webdav",
              .options = {{"url", "https://cloud.example/remote.php/dav/files/me"}, {"user", "me"}},
              .path = ""});
    CHECK(remote_of("dav://[fd00::1]:8080/share").options.front().second ==
          "http://[fd00::1]:8080/share");
}

TEST_CASE("rclone: a login replaces the URI's user, and the domain for smb") {
    const RcloneRemote smb =
        nordend::platform::with_login(remote_of("smb://WORK;me@nas/public"), "you", "");
    CHECK(smb.options ==
          std::vector<std::pair<std::string, std::string>>{{"host", "nas"}, {"user", "you"}});
    const RcloneRemote dav =
        nordend::platform::with_login(remote_of("davs://cloud.example/dav"), "me", "WORK");
    CHECK(dav.options.back() == std::pair<std::string, std::string>{"user", "me"});
    CHECK(dav.options.size() == 2);  // no domain outside smb
}

TEST_CASE("rclone: the command line's remote quotes every value") {
    CHECK(nordend::platform::connection_string(remote_of("smb://me@nas/Public/Scans")) ==
          R"(:smb,host="nas",user="me":Public/Scans)");
    CHECK(nordend::platform::connection_string(
              RcloneRemote{.backend = "sftp",
                           .options = {{"host", "fd00::1"}, {"user", R"(a"b,c)"}},
                           .path = "/srv"}) == R"(:sftp,host="fd00::1",user="a""b,c":/srv)");
    CHECK(nordend::platform::password_variable(remote_of("davs://h/x")) == "RCLONE_WEBDAV_PASS");
    CHECK(nordend::platform::password_variable(remote_of("smb://h/x")) == "RCLONE_SMB_PASS");
}

TEST_CASE("rclone: every bookmark has a folder of its own, the same in every run") {
    const fs::path root = "/run/user/1000/nordend/remotes";
    const fs::path a = nordend::platform::rclone_mount_point(root, "smb://nas/Public Files/");
    CHECK(a.parent_path() == root);
    CHECK(a.filename().string().starts_with("smb-nas-Public_Files-"));
    CHECK(a == nordend::platform::rclone_mount_point(root, "smb://nas/Public Files/"));
    CHECK(a != nordend::platform::rclone_mount_point(root, "smb://nas/Public%20Files/"));
    CHECK(nordend::platform::rclone_mount_point(root, "sftp://me@host")
              .filename()
              .string()
              .starts_with("sftp-host-"));
}

TEST_CASE("rclone: its mounts under the root in mountinfo, and the places they fill in") {
    const fs::path root = "/run/user/1000/nordend/remotes";
    const fs::path share = nordend::platform::rclone_mount_point(root, "smb://nas/Public");
    // The kernel escapes a space as \040.
    std::string escaped = share.string();
    const std::string mountinfo = std::format(
        "36 35 98:0 / /mnt/data rw,noatime master:1 - ext4 /dev/sda1 rw\n"
        "90 30 0:77 / {} rw,nosuid,nodev - fuse.rclone :smb{{x}}:Public rw,user_id=1000\n"
        "91 30 0:78 / /run/user/1000/gvfs rw - fuse.gvfsd-fuse gvfsd-fuse rw\n"
        "92 30 0:79 / /home/me/My\\040Cloud rw - fuse.rclone cloud: rw\n",
        escaped);
    const std::set<fs::path> mounted = nordend::platform::rclone_mounts(mountinfo, root);
    CHECK(mounted == std::set<fs::path>{share});
    CHECK(nordend::platform::rclone_mounts(mountinfo, "/home/me") ==
          std::set<fs::path>{"/home/me/My Cloud"});

    std::vector<Place> places = nordend::fs::parse_bookmarks(
        "file:///tmp/a\n"
        "smb://nas/Public\n"
        "sftp://me@host/srv\n");
    places = nordend::platform::with_rclone_mounts(std::move(places), root, mounted);
    REQUIRE(places.size() == 3);
    CHECK(places[0].path == "/tmp/a");
    CHECK(places[1].path == share);
    CHECK(places[1].disk.mounted);
    CHECK(places[1].disk.ejectable);
    CHECK(places[2].path.empty());  // left for gvfs, or not mounted
    CHECK_FALSE(places[2].disk.mounted);
}

TEST_CASE("rclone: what went wrong, from its output; a login wanted or not") {
    using nordend::platform::rclone_error;
    using nordend::platform::wants_login;
    const std::string unauthorized =
        "2026/11/12 22:29:14 ERROR : error listing: couldn't list files: 401 Unauthorized: 401 "
        "Unauthorized\n"
        "2026/11/12 22:29:14 NOTICE: Failed to lsf with 2 errors: last error was: error in "
        "ListJSON: couldn't list files: 401 Unauthorized: 401 Unauthorized\n";
    CHECK(rclone_error(unauthorized) == "couldn't list files: 401 Unauthorized: 401 Unauthorized");
    CHECK(wants_login(rclone_error(unauthorized)));
    const std::string logon =
        "2026/11/12 22:30:24 ERROR : error listing: couldn't connect SMB: response error: The "
        "attempted logon is invalid. This is either due to a bad username or authentication "
        "information.\n";
    CHECK(rclone_error(logon).starts_with("couldn't connect SMB: response error:"));
    CHECK(wants_login(rclone_error(logon)));
    CHECK(wants_login("couldn't initialize SMB: permission denied"));  // a guest turned away
    CHECK(wants_login("ssh: handshake failed: ssh: unable to authenticate"));

    const std::string refused =
        "2026/11/12 22:29:44 ERROR : error listing: couldn't connect SMB: dial tcp "
        "127.0.0.1:4450: connect: connection refused\n";
    CHECK(rclone_error(refused) ==
          "couldn't connect SMB: dial tcp 127.0.0.1:4450: connect: connection refused");
    CHECK_FALSE(wants_login(rclone_error(refused)));
    CHECK(
        rclone_error(
            "2026/11/12 22:00:00 CRITICAL: Failed to create file system for \":smb,host=\"x\":\": "
            "didn't find section in config file\n") == "didn't find section in config file");
    CHECK(rclone_error("") == "rclone failed");
    CHECK(rclone_error("something odd\n") == "something odd");
}

TEST_CASE("rclone: a mount asks for a password, again when it was wrong, then mounts") {
    const TempDir dir;
    const fs::path program = fake_rclone(dir);
    const fs::path root = dir.path / "remotes";
    EventLoop loop;
    nordend::core::WorkerPool pool{loop, 1};
    Rclone rclone{loop, pool, program, root, [] {}};

    const std::string uri = "davs://u@cloud.example/dav";
    std::vector<PasswordRequest> asked;
    Rclone::Prompts prompts{
        .password =
            [&](const PasswordRequest& request,
                const std::function<void(const PasswordAnswer&)>& answer) {
                asked.push_back(request);
                PasswordAnswer login;
                login.aborted = false;
                login.user = "u";
                login.password = asked.size() == 1 ? "falsch" : "geheim";
                answer(login);
            },
        .question = {},
        .cancel = {},
    };
    bool finished = false;
    fs::path where;
    std::string error;
    rclone.mount(uri, prompts, [&](const fs::path& at, const std::string& failure) {
        finished = true;
        where = at;
        error = failure;
    });
    REQUIRE(run_until(loop, [&] { return finished; }));
    CHECK_MESSAGE(error.empty(), error);
    CHECK(where == nordend::platform::rclone_mount_point(root, uri));
    REQUIRE(asked.size() == 2);
    CHECK(asked[0].message == "Password required for dav on cloud.example");
    CHECK(asked[0].user == "u");
    CHECK((asked[0].flags & PasswordRequest::kNeedPassword) != 0);
    CHECK((asked[0].flags & PasswordRequest::kAnonymousSupported) == 0);  // not for WebDAV
    CHECK(asked[1].message.find("did not accept the login") != std::string::npos);
    std::ifstream log{dir.path / "mounts.log"};
    std::string line;
    std::getline(log, line);
    CHECK(line ==
          std::format(R"(:webdav,url="https://cloud.example/dav",user="u": {})", where.string()));
}

TEST_CASE("rclone: giving up, and a server that cannot be reached, without a question") {
    const TempDir dir;
    const fs::path program = fake_rclone(dir);
    EventLoop loop;
    nordend::core::WorkerPool pool{loop, 1};
    Rclone rclone{loop, pool, program, dir.path / "remotes", [] {}};

    int asked = 0;
    Rclone::Prompts prompts{
        .password =
            [&](const PasswordRequest& request,
                const std::function<void(const PasswordAnswer&)>& answer) {
                ++asked;
                CHECK((request.flags & PasswordRequest::kNeedDomain) != 0);  // smb
                CHECK((request.flags & PasswordRequest::kAnonymousSupported) != 0);
                answer({});  // aborted
            },
        .question = {},
        .cancel = {},
    };
    std::optional<std::string> error;
    rclone.mount("smb://nas/public", prompts,
                 [&](const fs::path& /*at*/, const std::string& failure) { error = failure; });
    REQUIRE(run_until(loop, [&] { return error.has_value(); }));
    CHECK(error == std::string{nordend::platform::Gvfs::kCancelled});
    CHECK(asked == 1);

    error.reset();
    rclone.mount("smb://down/public", prompts,
                 [&](const fs::path& /*at*/, const std::string& failure) { error = failure; });
    REQUIRE(run_until(loop, [&] { return error.has_value(); }));
    CHECK(error == "couldn't connect: dial tcp: connect: connection refused");
    CHECK(asked == 1);
    CHECK_FALSE(fs::exists(
        nordend::platform::rclone_mount_point(dir.path / "remotes", "smb://down/public")));

    // A public share: as guest, without a question.
    error.reset();
    rclone.mount("smb://nas/open", prompts,
                 [&](const fs::path& /*at*/, const std::string& failure) { error = failure; });
    REQUIRE(run_until(loop, [&] { return error.has_value(); }));
    CHECK(error == "");
    CHECK(asked == 1);

    error.reset();
    rclone.mount("afp://nas/x", prompts,
                 [&](const fs::path& /*at*/, const std::string& failure) { error = failure; });
    CHECK(error == "it is not a network folder Nordend can mount");
    CHECK_FALSE(rclone.holds("smb://nas/public"));
}
