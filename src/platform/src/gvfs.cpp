#include "nordend/platform/gvfs.hpp"

#include <algorithm>
#include <cctype>
#include <chrono>
#include <cstdint>
#include <format>
#include <map>
#include <memory>
#include <optional>
#include <sdbus-c++/sdbus-c++.h>
#include <string_view>
#include <utility>

#include "bus_loop.hpp"
#include "nordend/core/log.hpp"

namespace nordend::platform {
namespace {

constexpr std::string_view kService = "org.gtk.vfs.Daemon";
constexpr std::string_view kTrackerPath = "/org/gtk/vfs/mounttracker";
constexpr std::string_view kTracker = "org.gtk.vfs.MountTracker";
constexpr std::string_view kMount = "org.gtk.vfs.Mount";
constexpr std::string_view kOperation = "org.gtk.vfs.MountOperation";
constexpr int kSmbPort = 445;
constexpr int kSshPort = 22;
constexpr int kFtpPort = 21;
constexpr int kHttpPort = 80;
constexpr int kHttpsPort = 443;
// A mount waits on the person typing a password: as long as that takes.
constexpr std::chrono::hours kMountTimeout{1};
constexpr std::chrono::minutes kUnmountTimeout{2};
// GPasswordSave
constexpr std::uint32_t kSaveNever = 0;
constexpr std::uint32_t kSavePermanently = 2;

using Bytes = std::vector<std::uint8_t>;
using DbusSpec = sdbus::Struct<Bytes, std::map<std::string, sdbus::Variant>>;
using DbusMount =
    sdbus::Struct<std::string, sdbus::ObjectPath, std::string, std::string, std::string,
                  std::string, std::string, std::string, bool, Bytes, DbusSpec, Bytes>;

std::string fold(std::string_view text) {
    std::string out;
    out.reserve(text.size());
    for (const char c : text) {
        out += static_cast<char>(std::tolower(static_cast<unsigned char>(c)));
    }
    return out;
}

// gvfs's "ay" byte strings end with a NUL, as g_variant_new_bytestring makes them.
Bytes bytestring(std::string_view text) {
    Bytes bytes(text.begin(), text.end());
    bytes.push_back(0);
    return bytes;
}

std::string bytes_text(const Bytes& bytes) {
    std::string text(bytes.begin(), bytes.end());
    while (!text.empty() && text.back() == '\0') {
        text.pop_back();
    }
    return text;
}

DbusSpec to_dbus(const GvfsMountSpec& spec) {
    std::map<std::string, sdbus::Variant> items;
    items.emplace("type", sdbus::Variant{bytestring(spec.type)});
    for (const auto& [key, value] : spec.items) {
        items.emplace(key, sdbus::Variant{bytestring(value)});
    }
    return DbusSpec{bytestring(spec.prefix), std::move(items)};
}

GvfsMountSpec from_dbus(const DbusSpec& dbus) {
    GvfsMountSpec spec;
    spec.prefix = bytes_text(std::get<0>(dbus));
    for (const auto& [key, value] : std::get<1>(dbus)) {
        std::string text;
        try {
            text = bytes_text(value.get<Bytes>());
        } catch (const sdbus::Error&) {
            continue;  // another type than gvfs writes
        }
        if (key == "type") {
            spec.type = std::move(text);
        } else {
            spec.items.emplace(key, std::move(text));
        }
    }
    return spec;
}

GvfsMount from_dbus(const DbusMount& dbus) {
    return {.dbus_id = std::get<0>(dbus),
            .object_path = std::get<1>(dbus),
            .name = std::get<2>(dbus),
            .fuse = bytes_text(std::get<9>(dbus)),
            .spec = from_dbus(std::get<10>(dbus))};
}

// gvfs's errors as a person reads them.
std::string readable(const sdbus::Error& error) {
    const std::string& name = error.getName();
    // G_IO_ERROR_FAILED_HANDLED: the person said no to a question, or closed the dialog.
    if (name.ends_with("Code30")) {
        return std::string{Gvfs::kCancelled};
    }
    if (name == "org.freedesktop.DBus.Error.ServiceUnknown" ||
        name.starts_with("org.freedesktop.DBus.Error.Spawn")) {
        return "gvfs is not running (it mounts network folders)";
    }
    const std::string& message = error.getMessage();
    return message.empty() ? name : message;
}

}  // namespace

std::optional<GvfsLocation> gvfs_location(const fs::RemoteUri& uri) {
    GvfsLocation location;
    GvfsMountSpec& spec = location.spec;
    if (uri.scheme == "smb") {
        std::string_view path = uri.path;
        while (path.starts_with('/')) {
            path.remove_prefix(1);
        }
        const std::string_view share = path.substr(0, path.find('/'));
        path.remove_prefix(share.size());
        while (path.starts_with('/')) {
            path.remove_prefix(1);
        }
        spec.items.emplace("server", fold(uri.host));
        if (share.empty()) {
            spec.type = "smb-server";
            location.path = "/";
        } else {
            spec.type = "smb-share";
            spec.items.emplace("share", fold(share));
            location.path = "/" + std::string{path};
        }
        if (uri.port != -1 && uri.port != kSmbPort) {
            spec.items.emplace("port", std::to_string(uri.port));
        }
        // "DOMAIN;user", either part may be missing.
        const std::size_t semicolon = uri.user.find(';');
        const std::string_view user = semicolon == std::string::npos
                                          ? std::string_view{uri.user}
                                          : std::string_view{uri.user}.substr(semicolon + 1);
        if (semicolon != std::string::npos && semicolon > 0) {
            spec.items.emplace("domain", uri.user.substr(0, semicolon));
        }
        if (!user.empty()) {
            spec.items.emplace("user", std::string{user});
        }
        return location;
    }
    // ssh and ftp: the generic mapper, for a host that is an internet address (case-folded),
    // without the scheme's default port.
    if (uri.scheme == "sftp" || uri.scheme == "ftp") {
        spec.type = uri.scheme;
        spec.items.emplace("host", fold(uri.host));
        if (!uri.user.empty()) {
            spec.items.emplace("user", uri.user);
        }
        const int default_port = uri.scheme == "sftp" ? kSshPort : kFtpPort;
        if (uri.port != -1 && uri.port != default_port) {
            spec.items.emplace("port", std::to_string(uri.port));
        }
        location.path = uri.path;
        return location;
    }
    // WebDAV: one "dav" type, ssl telling davs from dav, the host as written.
    if (uri.scheme == "dav" || uri.scheme == "davs") {
        const bool ssl = uri.scheme == "davs";
        spec.type = "dav";
        spec.items.emplace("ssl", ssl ? "true" : "false");
        spec.items.emplace("host", uri.host);
        if (!uri.user.empty()) {
            spec.items.emplace("user", uri.user);
        }
        if (uri.port != -1 && uri.port != (ssl ? kHttpsPort : kHttpPort)) {
            spec.items.emplace("port", std::to_string(uri.port));
        }
        location.path = uri.path;
        return location;
    }
    return std::nullopt;
}

bool spec_matches(const GvfsMountSpec& wanted, const GvfsMountSpec& mounted) {
    if (wanted.type != mounted.type) {
        return false;
    }
    return std::ranges::all_of(wanted.items, [&](const auto& item) {
        const auto it = mounted.items.find(item.first);
        return it != mounted.items.end() && it->second == item.second;
    });
}

const GvfsMount* mount_holding(const std::vector<GvfsMount>& mounts, const GvfsLocation& location) {
    for (const GvfsMount& mount : mounts) {
        if (spec_matches(location.spec, mount.spec) &&
            location.path.starts_with(mount.spec.prefix)) {
            return &mount;
        }
    }
    return nullptr;
}

std::filesystem::path local_path(const GvfsMount& mount, const GvfsLocation& location) {
    if (mount.fuse.empty()) {
        return {};
    }
    std::string_view inside = location.path;
    if (inside.starts_with(mount.spec.prefix)) {
        inside.remove_prefix(mount.spec.prefix.size());
    }
    while (inside.starts_with('/')) {
        inside.remove_prefix(1);
    }
    while (inside.ends_with('/')) {
        inside.remove_suffix(1);
    }
    return inside.empty() ? mount.fuse : mount.fuse / inside;
}

std::vector<fs::Place> with_remote_mounts(std::vector<fs::Place> places,
                                          const std::vector<GvfsMount>& mounts) {
    for (fs::Place& place : places) {
        if (place.uri.empty()) {
            continue;
        }
        const std::optional<fs::RemoteUri> uri = fs::parse_remote_uri(place.uri);
        const std::optional<GvfsLocation> location =
            uri ? gvfs_location(*uri) : std::optional<GvfsLocation>{};
        const GvfsMount* const mount = location ? mount_holding(mounts, *location) : nullptr;
        std::filesystem::path where = mount ? local_path(*mount, *location) : "";
        const bool mounted = !where.empty();
        place.path = std::move(where);
        place.disk.mounted = mounted;
        place.disk.ejectable = mounted;
    }
    return places;
}

// One mount in progress: the MountOperation object gvfs asks its questions through.
struct Operation {
    std::unique_ptr<sdbus::IObject> object;
    Gvfs::Prompts prompts;
};

struct Gvfs::Impl {
    core::EventLoop& loop;
    std::function<void()> changed;
    std::unique_ptr<sdbus::IConnection> connection;
    std::unique_ptr<sdbus::IProxy> tracker;
    std::unique_ptr<sdbus::IProxy> bus_daemon;
    std::map<std::string, std::unique_ptr<sdbus::IProxy>> mount_proxies;  // by dbus id + path
    sdbus::Slot signals;
    sdbus::Slot owner;
    std::vector<GvfsMount> mounts;
    std::map<std::uint64_t, std::unique_ptr<Operation>> operations;
    std::uint64_t next_operation{1};
    std::optional<bool> installed;  // gvfsd runs or can be started
    std::shared_ptr<bool> alive{std::make_shared<bool>(true)};
    std::unique_ptr<BusLoop> bus;

    Impl(core::EventLoop& event_loop, std::function<void()> on_change)
        : loop{event_loop}, changed{std::move(on_change)} {}

    ~Impl() { *alive = false; }

    Impl(const Impl&) = delete;
    Impl& operator=(const Impl&) = delete;
    Impl(Impl&&) = delete;
    Impl& operator=(Impl&&) = delete;

    void pump() const {
        if (bus) {
            bus->pump();
        }
    }

    // Reads the mounts; then runs after it, with or without an error.
    void read_mounts(std::function<void()> then = nullptr) {
        tracker->callMethodAsync("ListMounts")
            .onInterface(std::string{kTracker})
            .uponReplyInvoke([this, then = std::move(then)](std::optional<sdbus::Error> error,
                                                            const std::vector<DbusMount>& all) {
                if (error) {
                    log::warn("gvfs: {}", error->getMessage());
                } else {
                    std::vector<GvfsMount> fresh;
                    fresh.reserve(all.size());
                    for (const DbusMount& mount : all) {
                        fresh.push_back(from_dbus(mount));
                    }
                    const auto same = [](const GvfsMount& a, const GvfsMount& b) {
                        return a.dbus_id == b.dbus_id && a.object_path == b.object_path &&
                               a.fuse == b.fuse && a.spec == b.spec;
                    };
                    if (!std::ranges::equal(fresh, mounts, same)) {
                        mounts = std::move(fresh);
                        if (changed) {
                            changed();
                        }
                    }
                }
                if (then) {
                    then();
                }
            });
        pump();
    }

    void forget_mounts() {
        if (!mounts.empty()) {
            mounts.clear();
            if (changed) {
                changed();
            }
        }
    }

    // The MountOperation object of a new mount, at a path of its own.
    sdbus::ObjectPath add_operation(std::uint64_t id, Prompts prompts) {
        sdbus::ObjectPath path{std::format("/org/nordend/fm/MountOperation/{}", id)};
        auto operation = std::make_unique<Operation>();
        operation->prompts = std::move(prompts);
        operation->object = sdbus::createObject(*connection, path);
        Operation* const op = operation.get();
        using PasswordReply =
            sdbus::Result<bool, bool, std::string, std::string, std::string, bool, std::uint32_t>;
        using ChoiceReply = sdbus::Result<bool, bool, std::uint32_t>;
        op->object
            ->addVTable(
                sdbus::registerMethod("AskPassword")
                    .implementedAs([op](PasswordReply reply, const std::string& message,
                                        const std::string& user, const std::string& domain,
                                        std::uint32_t flags) {
                        if (!op->prompts.password) {
                            reply.returnResults(true, true, "", "", "", false, kSaveNever);
                            return;
                        }
                        op->prompts.password(
                            {.message = message, .user = user, .domain = domain, .flags = flags},
                            [reply = std::make_shared<PasswordReply>(std::move(reply))](
                                const PasswordAnswer& answer) {
                                reply->returnResults(
                                    true, answer.aborted, answer.password, answer.user,
                                    answer.domain, answer.anonymous,
                                    answer.remember ? kSavePermanently : kSaveNever);
                            });
                    }),
                sdbus::registerMethod("AskQuestion")
                    .implementedAs([op](ChoiceReply reply, const std::string& message,
                                        const std::vector<std::string>& choices) {
                        if (!op->prompts.question || choices.empty()) {
                            reply.returnResults(true, true, 0U);
                            return;
                        }
                        op->prompts.question(
                            message, choices,
                            [reply = std::make_shared<ChoiceReply>(std::move(reply))](
                                std::optional<std::size_t> choice) {
                                reply->returnResults(
                                    true, !choice.has_value(),
                                    static_cast<std::uint32_t>(choice.value_or(0)));
                            });
                    }),
                // Only an unmount asks these, and Nordend passes no operation to one.
                sdbus::registerMethod("ShowProcesses")
                    .implementedAs([](const std::string& /*message*/,
                                      const std::vector<std::string>& /*choices*/,
                                      const std::vector<std::int32_t>& /*processes*/) {
                        return std::tuple<bool, bool, std::uint32_t>{false, false, 0U};
                    }),
                sdbus::registerMethod("ShowUnmountProgress")
                    .implementedAs([](const std::string& /*message*/, std::int64_t /*time_left*/,
                                      std::int64_t /*bytes_left*/) {}),
                sdbus::registerMethod("Aborted").implementedAs([op] {
                    if (op->prompts.cancel) {
                        op->prompts.cancel();
                    }
                }))
            .forInterface(std::string{kOperation});
        operations.emplace(id, std::move(operation));
        return path;
    }

    // The mount's local folder, once the mounts are read again; an error without one.
    void finish(const GvfsLocation& location, const Mounted& done) {
        read_mounts([this, location, done] {
            const GvfsMount* const mount = mount_holding(mounts, location);
            if (mount == nullptr) {
                done({}, "gvfs does not list it as mounted");
                return;
            }
            std::filesystem::path where = local_path(*mount, location);
            if (where.empty()) {
                done({}, "gvfs has no local folder for it (gvfsd-fuse is not running)");
                return;
            }
            done(where, {});
        });
    }
};

Gvfs::Gvfs(core::EventLoop& loop, std::function<void()> changed)
    : impl_{std::make_unique<Impl>(loop, std::move(changed))} {
    try {
        impl_->connection = sdbus::createSessionBusConnection();
        impl_->bus = std::make_unique<BusLoop>(loop, *impl_->connection, "gvfs");
        impl_->tracker =
            sdbus::createProxy(*impl_->connection, sdbus::ServiceName{std::string{kService}},
                               sdbus::ObjectPath{std::string{kTrackerPath}});
        impl_->bus_daemon =
            sdbus::createProxy(*impl_->connection, sdbus::ServiceName{"org.freedesktop.DBus"},
                               sdbus::ObjectPath{"/org/freedesktop/DBus"});
        // Something mounted or unmounted, by Nordend or another program: read again.
        impl_->signals = impl_->connection->addMatch(
            std::format("type='signal',interface='{}'", kTracker),
            [this](const sdbus::Message& /*message*/) { impl_->read_mounts(); },
            sdbus::return_slot);
        // gvfsd started (by another program) or gone.
        impl_->owner = impl_->connection->addMatch(
            std::format("type='signal',sender='org.freedesktop.DBus',member='NameOwnerChanged',"
                        "arg0='{}'",
                        kService),
            [this](sdbus::Message message) {
                std::string name;
                std::string before;
                std::string after;
                message >> name >> before >> after;
                if (after.empty()) {
                    impl_->forget_mounts();
                } else {
                    impl_->installed = true;
                    impl_->read_mounts();
                }
            },
            sdbus::return_slot);
        // Only a running gvfsd is asked: reading the mounts would start it, for nothing when
        // no network folder is mounted.
        impl_->bus_daemon->callMethodAsync("NameHasOwner")
            .onInterface("org.freedesktop.DBus")
            .withArguments(std::string{kService})
            .uponReplyInvoke([this](std::optional<sdbus::Error> error, bool running) {
                if (!error && running) {
                    impl_->installed = true;
                    impl_->read_mounts();
                }
            });
        // Whether the bus can start gvfsd: without it, network folders are someone else's job.
        impl_->bus_daemon->callMethodAsync("ListActivatableNames")
            .onInterface("org.freedesktop.DBus")
            .uponReplyInvoke(
                [this](std::optional<sdbus::Error> error, const std::vector<std::string>& names) {
                    if (impl_->installed.value_or(false)) {
                        return;
                    }
                    impl_->installed = !error && std::ranges::find(names, kService) != names.end();
                });
        impl_->pump();
    } catch (const sdbus::Error& error) {
        log::warn("gvfs: no session bus ({}): network folders are not mounted through gvfs",
                  error.getMessage());
        impl_->installed = false;
        impl_->tracker.reset();
        impl_->bus_daemon.reset();
        impl_->bus.reset();
        impl_->connection.reset();
    }
}

Gvfs::~Gvfs() = default;

bool Gvfs::available() const noexcept { return impl_->connection != nullptr; }

std::optional<bool> Gvfs::installed() const noexcept { return impl_->installed; }

const std::vector<GvfsMount>& Gvfs::mounts() const noexcept { return impl_->mounts; }

void Gvfs::mount(const std::string& uri, Prompts prompts, Mounted done) {
    if (!available()) {
        done({}, "there is no session bus");
        return;
    }
    const std::optional<fs::RemoteUri> remote = fs::parse_remote_uri(uri);
    const std::optional<GvfsLocation> location =
        remote ? gvfs_location(*remote) : std::optional<GvfsLocation>{};
    if (!location) {
        done({}, "it is not a network folder Nordend can mount");
        return;
    }
    if (const GvfsMount* const mount = mount_holding(impl_->mounts, *location)) {
        if (std::filesystem::path where = local_path(*mount, *location); !where.empty()) {
            done(where, {});
            return;
        }
    }
    const std::uint64_t id = impl_->next_operation++;
    sdbus::ObjectPath source;
    try {
        source = impl_->add_operation(id, std::move(prompts));
    } catch (const sdbus::Error& error) {
        done({}, readable(error));
        return;
    }
    // As GLib asks: the whole path as the prefix, which the backend cuts down to its mount
    // (a WebDAV share mounts where the URI points, not at the server's root).
    GvfsMountSpec wanted = location->spec;
    wanted.prefix = location->path;
    impl_->tracker->callMethodAsync("MountLocation")
        .onInterface(std::string{kTracker})
        .withTimeout(kMountTimeout)
        .withArguments(to_dbus(wanted),
                       sdbus::Struct<std::string, sdbus::ObjectPath>{
                           std::string{impl_->connection->getUniqueName()}, source})
        .uponReplyInvoke([this, alive = impl_->alive, id, location = *location,
                          done = std::move(done)](std::optional<sdbus::Error> error) {
            if (!*alive) {
                return;
            }
            // Not from inside its own call: the object goes on the next turn of the loop.
            impl_->loop.post([this, alive, id] {
                if (*alive) {
                    impl_->operations.erase(id);
                }
            });
            if (error) {
                done({}, readable(*error));
                return;
            }
            impl_->finish(location, done);
        });
    impl_->pump();
}

void Gvfs::unmount(const std::string& uri, Done done) {
    if (!available()) {
        done("there is no session bus");
        return;
    }
    const std::optional<fs::RemoteUri> remote = fs::parse_remote_uri(uri);
    const std::optional<GvfsLocation> location =
        remote ? gvfs_location(*remote) : std::optional<GvfsLocation>{};
    const GvfsMount* const mount = location ? mount_holding(impl_->mounts, *location) : nullptr;
    if (mount == nullptr) {
        done("it is not mounted");
        return;
    }
    auto& proxy = impl_->mount_proxies[mount->dbus_id + mount->object_path];
    try {
        if (!proxy) {
            proxy = sdbus::createProxy(*impl_->connection, sdbus::ServiceName{mount->dbus_id},
                                       sdbus::ObjectPath{mount->object_path});
        }
    } catch (const sdbus::Error& error) {
        done(readable(error));
        return;
    }
    // No mount operation (gvfs's dummy source): an unmount asks nothing.
    proxy->callMethodAsync("Unmount")
        .onInterface(std::string{kMount})
        .withTimeout(kUnmountTimeout)
        .withArguments(std::string{}, sdbus::ObjectPath{"/"}, std::uint32_t{0})
        .uponReplyInvoke([this, alive = impl_->alive,
                          done = std::move(done)](std::optional<sdbus::Error> error) {
            if (!*alive) {
                return;
            }
            if (error) {
                const std::string message = readable(*error);
                // G_IO_ERROR_BUSY
                done(error->getName().ends_with("Code26") ? std::string{"in use"} : message);
                return;
            }
            impl_->read_mounts();
            done({});
        });
    impl_->pump();
}

}  // namespace nordend::platform
