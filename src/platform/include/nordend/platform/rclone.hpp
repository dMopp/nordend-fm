#pragma once

#include <chrono>
#include <filesystem>
#include <functional>
#include <memory>
#include <optional>
#include <set>
#include <string>
#include <string_view>
#include <utility>
#include <vector>

#include "nordend/core/event_loop.hpp"
#include "nordend/core/worker_pool.hpp"
#include "nordend/fs/places.hpp"
#include "nordend/platform/gvfs.hpp"

// Remote bookmarks mounted without gvfs: rclone mounts each one with FUSE in the person's own
// session (no daemon of ours, no root), for systems that do not run gvfs. rclone must be in the
// PATH; it is run, never linked. Each share gets a folder of its own under a root
// ($XDG_RUNTIME_DIR/nordend/remotes), named after its URI, so one mounted by an earlier Nordend is
// found again; rclone keeps serving it after Nordend closes, as gvfs does.
namespace nordend::platform {

// A remote as rclone reaches it: one of its backends, the options of an on-the-fly remote, and the
// path inside it.
struct RcloneRemote {
    std::string backend;                                       // "smb", "sftp", "ftp", "webdav"
    std::vector<std::pair<std::string, std::string>> options;  // in this order
    std::string path;  // "share/dir" for smb, absolute for sftp and ftp, empty for webdav

    bool operator==(const RcloneRemote&) const = default;
};

// uri as rclone reaches it: smb://DOMAIN;user@host:port/share/dir an "smb" remote with host, port,
// user and domain, the path share/dir (empty: the server, which lists its shares); sftp:// and
// ftp:// with host, port and user, the path as it is; dav:// and davs:// a "webdav" remote whose
// url is the http:// or https:// one, with user. nullopt for anything else.
[[nodiscard]] std::optional<RcloneRemote> rclone_remote(const fs::RemoteUri& uri);

// remote with the user (and for smb the domain) a login gave, replacing the URI's.
[[nodiscard]] RcloneRemote with_login(RcloneRemote remote, const std::string& user,
                                      const std::string& domain);

// remote as rclone's command line names it: :smb,host="nas",user="me":share/dir. Every value is
// quoted, a quote in it doubled, so commas and colons (IPv6 hosts) stay inside.
[[nodiscard]] std::string connection_string(const RcloneRemote& remote);

// The environment variable rclone reads remote's password from, obscured: RCLONE_SMB_PASS.
[[nodiscard]] std::string password_variable(const RcloneRemote& remote);

// The folder uri is mounted on under root: its scheme, host and last folder, and a hash of the
// whole URI ("smb-nas-Public-1a2b3c4d"), the same in every run.
[[nodiscard]] std::filesystem::path rclone_mount_point(const std::filesystem::path& root,
                                                       std::string_view uri);

// The rclone mounts under root in /proc/self/mountinfo's text (octal escapes decoded).
[[nodiscard]] std::set<std::filesystem::path> rclone_mounts(std::string_view mountinfo,
                                                            const std::filesystem::path& root);

// The places with the rclone mounts in: a remote bookmark whose folder is mounted gets it, mounted
// and ejectable; the others stay as they are (not mounted, or mounted by gvfs).
[[nodiscard]] std::vector<fs::Place> with_rclone_mounts(
    std::vector<fs::Place> places, const std::filesystem::path& root,
    const std::set<std::filesystem::path>& mounted);

// What went wrong, from rclone's output: the last error it logged, without its time stamp, level
// and the operation's own prefix ("couldn't connect SMB: ..."); else its last line.
[[nodiscard]] std::string rclone_error(std::string_view output);

// Whether an error says the server wants a login: a name or password missing or not accepted.
[[nodiscard]] bool wants_login(std::string_view error);

// rclone, run on the worker pool so the loop never waits for a server. A mount first lists the
// share's folder, as rclone mount itself does not check the login: when that fails for a login (an
// smb bookmark without a user is tried as guest first), the person is asked, up to three times, and
// only a share that answered is mounted.
//
// Loop thread only. Must not outlive the loop or the pool.
class Rclone {
public:
    using Prompts = Gvfs::Prompts;  // only password and cancel are used
    using Mounted = Gvfs::Mounted;
    using Done = Gvfs::Done;

    // program: rclone; root: where shares are mounted; changed: runs when a mount came or went.
    Rclone(core::EventLoop& loop, core::WorkerPool& pool, std::filesystem::path program,
           std::filesystem::path root, std::function<void()> changed);
    ~Rclone();

    Rclone(const Rclone&) = delete;
    Rclone& operator=(const Rclone&) = delete;
    Rclone(Rclone&&) = delete;
    Rclone& operator=(Rclone&&) = delete;

    // rclone in the PATH; nullopt when it is not installed.
    [[nodiscard]] static std::optional<std::filesystem::path> find_program();

    // $XDG_RUNTIME_DIR/nordend/remotes, else one in the temporary folder, per user.
    [[nodiscard]] static std::filesystem::path default_root();

    [[nodiscard]] const std::filesystem::path& root() const noexcept;

    // Whether uri's share is mounted by rclone now.
    [[nodiscard]] bool holds(std::string_view uri) const;

    // The places with what rclone has mounted (read from /proc/self/mountinfo).
    [[nodiscard]] std::vector<fs::Place> places(std::vector<fs::Place> places) const;

    // Mounts uri unless it is mounted; done gets its folder, or an error (Gvfs::kCancelled when
    // the person gave up).
    void mount(const std::string& uri, Prompts prompts, Mounted done);

    // Unmounts uri's share with fusermount; "in use" when something has a file open on it.
    void unmount(const std::string& uri, Done done);

private:
    struct Impl;
    std::shared_ptr<Impl> impl_;
};

}  // namespace nordend::platform
