#pragma once

#include <cstdint>
#include <filesystem>
#include <functional>
#include <map>
#include <memory>
#include <optional>
#include <string>
#include <string_view>
#include <vector>

#include "nordend/core/event_loop.hpp"
#include "nordend/fs/places.hpp"

// Remote bookmarks (smb://, sftp://, ftp://, dav://, davs://) mounted through gvfs on the session
// bus, without GLib in the process: gvfsd mounts them and its FUSE daemon shows them as local
// folders under $XDG_RUNTIME_DIR/gvfs, which Nordend then opens like any other.
namespace nordend::platform {

// What gvfs calls a mount: a type and its keys ("smb-share", server=..., share=...), as
// GMountSpec has them.
struct GvfsMountSpec {
    std::string type;
    std::map<std::string, std::string> items;  // without "type"
    std::string prefix{"/"};                   // the mount prefix

    bool operator==(const GvfsMountSpec&) const = default;
};

// A remote URI as gvfs mounts it: the mount, and the folder inside it the URI names.
struct GvfsLocation {
    GvfsMountSpec spec;
    std::string path;  // "/" for the mount's root

    bool operator==(const GvfsLocation&) const = default;
};

// The mount gvfs makes of uri, as its own URI mappers do: smb://server/share/dir is an
// "smb-share" (server and share case-folded, "DOMAIN;user" split, port 445 left out), the path
// /dir; smb://server alone an "smb-server"; sftp:// and ftp:// an "sftp" or "ftp" with host
// (case-folded), user and a port that is not the default; dav:// and davs:// a "dav" with ssl,
// host, user and port. nullopt for anything else.
[[nodiscard]] std::optional<GvfsLocation> gvfs_location(const fs::RemoteUri& uri);

// mounted is the mount wanted names: the same type, and every key wanted has with the same
// value. A mount gvfs made may have more (the user who logged in), never fewer.
[[nodiscard]] bool spec_matches(const GvfsMountSpec& wanted, const GvfsMountSpec& mounted);

// One mount gvfs has (ListMounts).
struct GvfsMount {
    std::string dbus_id;  // the backend that serves it, for Unmount
    std::string object_path;
    std::string name;            // what gvfs calls it ("Public on fileshare")
    std::filesystem::path fuse;  // its local folder; empty without gvfsd-fuse
    GvfsMountSpec spec;
};

// The mount among mounts holding location; nullptr when it is not mounted.
[[nodiscard]] const GvfsMount* mount_holding(const std::vector<GvfsMount>& mounts,
                                             const GvfsLocation& location);

// Where location is on the local disk: its folder inside the mount's FUSE folder; empty when
// the mount has none (gvfsd-fuse not running).
[[nodiscard]] std::filesystem::path local_path(const GvfsMount& mount,
                                               const GvfsLocation& location);

// The places with the gvfs mounts in: a remote bookmark that is mounted gets its local folder,
// mounted and ejectable (the eject icon unmounts it); one that is not stays without a path.
[[nodiscard]] std::vector<fs::Place> with_remote_mounts(std::vector<fs::Place> places,
                                                        const std::vector<GvfsMount>& mounts);

// What gvfs asks before mounting: the backend's question for a name and a password
// (MountOperation.AskPassword).
struct PasswordRequest {
    // GAskPasswordFlags
    static constexpr std::uint32_t kNeedPassword = 1;
    static constexpr std::uint32_t kNeedUsername = 2;
    static constexpr std::uint32_t kNeedDomain = 4;
    static constexpr std::uint32_t kSavingSupported = 8;
    static constexpr std::uint32_t kAnonymousSupported = 16;

    std::string message;  // "Password required for share public on fileshare"
    std::string user;     // the defaults to show
    std::string domain;
    std::uint32_t flags{0};
};

// The answer; aborted: the person gave up.
struct PasswordAnswer {
    bool aborted{true};
    bool anonymous{false};
    std::string user;
    std::string domain;
    std::string password;
    bool remember{false};  // kept in the keyring by gvfs
};

// gvfsd on the session bus. Every call is asynchronous: the bus's socket is watched by the loop,
// and answers come back on the loop thread. Without a session bus available() is false; when
// gvfsd cannot be started, mounting says so.
//
// Loop thread only. Must not outlive the loop.
class Gvfs {
public:
    // changed runs when mounts() changed.
    Gvfs(core::EventLoop& loop, std::function<void()> changed);
    ~Gvfs();

    Gvfs(const Gvfs&) = delete;
    Gvfs& operator=(const Gvfs&) = delete;
    Gvfs(Gvfs&&) = delete;
    Gvfs& operator=(Gvfs&&) = delete;

    [[nodiscard]] bool available() const noexcept;

    [[nodiscard]] const std::vector<GvfsMount>& mounts() const noexcept;

    // What a mount asks the person, answered later on the loop thread: a name and a password,
    // a question (choices[0] is the one that goes on: "Log In Anyway"; nullopt gives up), and
    // cancel when gvfs gave up waiting (the dialog closes then).
    struct Prompts {
        std::function<void(const PasswordRequest& request,
                           std::function<void(const PasswordAnswer& answer)> answer)>
            password;
        std::function<void(const std::string& message, const std::vector<std::string>& choices,
                           std::function<void(std::optional<std::size_t> choice)> answer)>
            question;
        std::function<void()> cancel;
    };

    // An error is gvfs's message, kCancelled when the person gave up; empty when it worked.
    using Mounted =
        std::function<void(const std::filesystem::path& where, const std::string& error)>;
    using Done = std::function<void(const std::string& error)>;

    static constexpr std::string_view kCancelled = "cancelled";

    // Mounts uri (a remote bookmark) unless it is mounted; done gets its local folder.
    void mount(const std::string& uri, Prompts prompts, Mounted done);

    // Unmounts the mount holding uri.
    void unmount(const std::string& uri, Done done);

private:
    struct Impl;
    std::unique_ptr<Impl> impl_;
};

}  // namespace nordend::platform
