#include "nordend/platform/rclone.hpp"

#include <algorithm>
#include <array>
#include <cctype>
#include <cerrno>
#include <csignal>
#include <cstdint>
#include <cstdlib>
#include <cstring>
#include <fcntl.h>
#include <format>
#include <fstream>
#include <poll.h>
#include <spawn.h>
#include <sys/mman.h>
#include <sys/syscall.h>
#include <sys/wait.h>
#include <system_error>
#include <unistd.h>

#include "nordend/core/log.hpp"

extern char** environ;  // NOLINT(readability-redundant-declaration)

namespace nordend::platform {
namespace {

namespace stdfs = std::filesystem;
using namespace std::chrono_literals;

constexpr int kMaxLogins = 3;                  // asked again after a wrong password, then given up
constexpr std::size_t kMaxOutput = 64 * 1024;  // what is read of a run's output
constexpr std::size_t kMaxNamePart = 40;       // of each part of a mount point's name

std::string lower(std::string_view text) {
    std::string out{text};
    std::ranges::transform(out, out.begin(),
                           [](unsigned char c) { return static_cast<char>(std::tolower(c)); });
    return out;
}

bool has(std::string_view text, std::string_view needle) {
    return text.find(needle) != std::string_view::npos;
}

std::string_view trim(std::string_view text) {
    const std::size_t first = text.find_first_not_of(" \t\r\n");
    if (first == std::string_view::npos) {
        return {};
    }
    return text.substr(first, text.find_last_not_of(" \t\r\n") - first + 1);
}

// The option key set to value, replacing it when it is there.
void set_option(RcloneRemote& remote, std::string_view key, std::string value) {
    const auto it =
        std::ranges::find(remote.options, key, &std::pair<std::string, std::string>::first);
    if (it != remote.options.end()) {
        it->second = std::move(value);
    } else {
        remote.options.emplace_back(key, std::move(value));
    }
}

void drop_option(RcloneRemote& remote, std::string_view key) {
    std::erase_if(remote.options, [key](const auto& option) { return option.first == key; });
}

// A file name made of text: letters, digits, '.', '_' and '-'; the rest '_'.
std::string name_part(std::string_view text) {
    std::string out;
    for (const char c : text.substr(0, kMaxNamePart)) {
        const bool plain =
            std::isalnum(static_cast<unsigned char>(c)) != 0 || c == '.' || c == '_' || c == '-';
        out += plain ? c : '_';
    }
    return out;
}

// FNV-1a: stable across runs and builds, unlike std::hash.
std::uint32_t fnv1a(std::string_view text) {
    std::uint32_t hash = 2166136261U;
    for (const char c : text) {
        hash ^= static_cast<unsigned char>(c);
        hash *= 16777619U;
    }
    return hash;
}

// "\040" -> " ", as the kernel escapes mountinfo's fields.
std::string decode_octal(std::string_view text) {
    std::string out;
    for (std::size_t i = 0; i < text.size(); ++i) {
        if (text[i] == '\\' && i + 3 < text.size()) {
            const std::string_view digits = text.substr(i + 1, 3);
            if (std::ranges::all_of(digits, [](char d) { return d >= '0' && d <= '7'; })) {
                out += static_cast<char>(((digits[0] - '0') * 64) + ((digits[1] - '0') * 8) +
                                         (digits[2] - '0'));
                i += 3;
                continue;
            }
        }
        out += text[i];
    }
    return out;
}

// A file descriptor, closed when it goes.
class Fd {
public:
    explicit Fd(int fd) noexcept : fd_{fd} {}

    ~Fd() {
        if (fd_ >= 0) {
            close(fd_);
        }
    }

    Fd(const Fd&) = delete;
    Fd& operator=(const Fd&) = delete;
    Fd(Fd&&) = delete;
    Fd& operator=(Fd&&) = delete;

    [[nodiscard]] int get() const noexcept { return fd_; }

private:
    int fd_;
};

// How a program ran: whether it started, its exit status (-1 when killed), what it printed.
struct Ran {
    bool started{false};
    int status{-1};
    std::string output;
};

// Nordend's environment with extra ("NAME=value") replacing variables of the same name.
std::vector<std::string> environment_with(const std::vector<std::string>& extra) {
    std::vector<std::string> out;
    for (char** entry = environ; entry != nullptr && *entry != nullptr; ++entry) {
        const std::string_view variable{*entry};
        const std::string_view name = variable.substr(0, variable.find('=') + 1);
        if (std::ranges::none_of(extra, [name](const std::string& set) {
                return std::string_view{set}.starts_with(name);
            })) {
            out.emplace_back(variable);
        }
    }
    std::ranges::copy(extra, std::back_inserter(out));
    return out;
}

std::vector<char*> c_array(std::vector<std::string>& strings) {
    std::vector<char*> out;
    out.reserve(strings.size() + 1);
    for (std::string& string : strings) {
        out.push_back(string.data());
    }
    out.push_back(nullptr);
    return out;
}

// Runs argv (argv[0] looked up in the PATH) with env added, input on its stdin, and waits at most
// wait for it; its stdout and stderr together are the output. In a session of its own with the
// default signal mask, and no descriptor of ours: rclone's mount daemon outlives it. Memory files
// instead of pipes, so a daemon that keeps them open cannot make this wait.
Ran run(std::vector<std::string> argv, const std::vector<std::string>& env, std::string_view input,
        std::chrono::milliseconds wait) {
    Ran ran;
    const Fd in{memfd_create("nordend-rclone-in", MFD_CLOEXEC)};
    const Fd out{memfd_create("nordend-rclone-out", MFD_CLOEXEC)};
    if (in.get() < 0 || out.get() < 0) {
        ran.output = std::strerror(errno);
        return ran;
    }
    for (std::string_view left = input; !left.empty();) {
        const ssize_t wrote = write(in.get(), left.data(), left.size());
        if (wrote < 0 && errno == EINTR) {
            continue;
        }
        if (wrote <= 0) {
            ran.output = std::strerror(errno);
            return ran;
        }
        left.remove_prefix(static_cast<std::size_t>(wrote));
    }
    if (lseek(in.get(), 0, SEEK_SET) != 0) {
        ran.output = std::strerror(errno);
        return ran;
    }

    posix_spawn_file_actions_t actions{};
    posix_spawn_file_actions_init(&actions);
    posix_spawn_file_actions_adddup2(&actions, in.get(), STDIN_FILENO);
    posix_spawn_file_actions_adddup2(&actions, out.get(), STDOUT_FILENO);
    posix_spawn_file_actions_adddup2(&actions, out.get(), STDERR_FILENO);
    posix_spawn_file_actions_addclosefrom_np(&actions, STDERR_FILENO + 1);
    posix_spawnattr_t attr{};
    posix_spawnattr_init(&attr);
    sigset_t none{};
    sigemptyset(&none);
    sigset_t all{};
    sigfillset(&all);
    posix_spawnattr_setsigmask(&attr, &none);
    posix_spawnattr_setsigdefault(&attr, &all);
    posix_spawnattr_setflags(&attr,
                             POSIX_SPAWN_SETSID | POSIX_SPAWN_SETSIGMASK | POSIX_SPAWN_SETSIGDEF);
    std::vector<std::string> environment = environment_with(env);
    const std::vector<char*> c_args = c_array(argv);
    const std::vector<char*> c_env = c_array(environment);
    int pidfd = -1;
    const int error =
        pidfd_spawnp(&pidfd, c_args.front(), &actions, &attr, c_args.data(), c_env.data());
    posix_spawn_file_actions_destroy(&actions);
    posix_spawnattr_destroy(&attr);
    if (error != 0) {
        ran.output = std::strerror(error);
        return ran;
    }
    const Fd child{pidfd};
    ran.started = true;

    pollfd watched{.fd = child.get(), .events = POLLIN, .revents = 0};
    int ready = 0;
    do {
        ready = poll(&watched, 1, static_cast<int>(wait.count()));
    } while (ready < 0 && errno == EINTR);
    const bool timed_out = ready == 0;
    if (timed_out) {
        syscall(SYS_pidfd_send_signal, child.get(), SIGKILL, nullptr, 0);  // not in every libc
    }
    siginfo_t info{};
    if (waitid(P_PIDFD, static_cast<id_t>(child.get()), &info, WEXITED) == 0 &&
        info.si_code == CLD_EXITED && !timed_out) {
        ran.status = info.si_status;
    }

    std::array<char, 4096> buffer{};
    for (off_t at = 0; ran.output.size() < kMaxOutput;) {
        const ssize_t got = pread(out.get(), buffer.data(), buffer.size(), at);
        if (got < 0 && errno == EINTR) {
            continue;
        }
        if (got <= 0) {
            break;
        }
        ran.output.append(buffer.data(), static_cast<std::size_t>(got));
        at += got;
    }
    if (timed_out) {
        ran.output += "\nERROR : the server did not answer in time\n";
    }
    return ran;
}

std::string read_file(const stdfs::path& path) {
    std::ifstream file{path};
    std::string text;
    text.resize(kMaxOutput);
    file.read(text.data(), static_cast<std::streamsize>(text.size()));
    text.resize(static_cast<std::size_t>(file.gcount()));
    return text;
}

// sftp checks the server's key against the person's known_hosts, as ssh does, when there is one;
// without it rclone would take any key.
RcloneRemote with_known_hosts(RcloneRemote remote) {
    if (remote.backend != "sftp") {
        return remote;
    }
    const char* const home = std::getenv("HOME");
    if (home == nullptr || *home == '\0') {
        return remote;
    }
    const stdfs::path known = stdfs::path{home} / ".ssh" / "known_hosts";
    std::error_code ignored;
    if (stdfs::is_regular_file(known, ignored)) {
        set_option(remote, "known_hosts_file", known.string());
    }
    return remote;
}

// What a mount came to: an error, or mounted.
struct Outcome {
    std::string error;
    bool mounted{false};
};

// On a worker: the share listed with the login (so a wrong password says so before anything is
// mounted), then mounted on where.
Outcome mount_now(const stdfs::path& program, const RcloneRemote& remote, const stdfs::path& where,
                  const std::string& password) {
    std::vector<std::string> env;
    if (!password.empty()) {
        const Ran obscured = run({program.string(), "obscure", "-"}, {}, password, 10s);
        if (obscured.status != 0) {
            return {.error = "rclone could not take the password"};
        }
        env.push_back(std::format("{}={}", password_variable(remote), trim(obscured.output)));
    }
    const std::string remote_name = connection_string(remote);
    const Ran listed =
        run({program.string(), "--config=", "lsf", "--max-depth=1", "--retries=1",
             "--low-level-retries=1", "--contimeout=15s", "--timeout=30s", remote_name},
            env, {}, 60s);
    if (!listed.started) {
        return {.error = std::format("rclone cannot be started: {}", trim(listed.output))};
    }
    if (listed.status != 0) {
        return {.error = rclone_error(listed.output)};
    }
    std::error_code error;
    stdfs::create_directories(where, error);
    if (error) {
        return {.error = std::format("{} cannot be made: {}", where.string(), error.message())};
    }
    const stdfs::path log = where.parent_path() / ("." + where.filename().string() + ".log");
    stdfs::remove(log, error);
    const Ran mounted = run({program.string(), "--config=", "mount", remote_name, where.string(),
                             "--daemon", "--daemon-wait=30s", "--vfs-cache-mode=writes",
                             "--contimeout=15s", "--log-file=" + log.string()},
                            env, {}, 60s);
    if (mounted.status != 0) {
        stdfs::remove(where, error);  // only when empty: never a folder with files in it
        return {.error = rclone_error(mounted.output + "\n" + read_file(log))};
    }
    return {.error = {}, .mounted = true};
}

// On a worker: where unmounted with fusermount3 (or fusermount), its folder and log removed.
std::string unmount_now(const stdfs::path& where) {
    // Its messages in English: "busy" is looked for below.
    const std::vector<std::string> english{"LC_ALL=C"};
    Ran ran = run({"fusermount3", "-u", where.string()}, english, {}, 30s);
    if (!ran.started) {
        ran = run({"fusermount", "-u", where.string()}, english, {}, 30s);
    }
    if (!ran.started) {
        return "fusermount is not installed";
    }
    if (ran.status != 0) {
        const std::string said{trim(ran.output)};
        if (has(lower(said), "busy")) {
            return "in use";
        }
        return said.empty() ? std::string{"fusermount failed"} : said;
    }
    std::error_code ignored;
    stdfs::remove(where, ignored);
    stdfs::remove(where.parent_path() / ("." + where.filename().string() + ".log"), ignored);
    return {};
}

}  // namespace

std::optional<RcloneRemote> rclone_remote(const fs::RemoteUri& uri) {
    RcloneRemote remote;
    const auto port = [&uri](int default_port) -> std::optional<std::string> {
        if (uri.port == -1 || uri.port == default_port) {
            return std::nullopt;
        }
        return std::to_string(uri.port);
    };
    if (uri.scheme == "smb") {
        remote.backend = "smb";
        remote.options.emplace_back("host", uri.host);
        if (const auto given = port(445)) {
            remote.options.emplace_back("port", *given);
        }
        // "DOMAIN;user", either part may be missing.
        const std::size_t semicolon = uri.user.find(';');
        const std::string user =
            semicolon == std::string::npos ? uri.user : uri.user.substr(semicolon + 1);
        if (!user.empty()) {
            remote.options.emplace_back("user", user);
        }
        if (semicolon != std::string::npos && semicolon > 0) {
            remote.options.emplace_back("domain", uri.user.substr(0, semicolon));
        }
        std::string_view path = uri.path;
        while (path.starts_with('/')) {
            path.remove_prefix(1);
        }
        while (path.ends_with('/')) {
            path.remove_suffix(1);
        }
        remote.path = path;
        return remote;
    }
    if (uri.scheme == "sftp" || uri.scheme == "ftp") {
        remote.backend = uri.scheme;
        remote.options.emplace_back("host", uri.host);
        if (const auto given = port(uri.scheme == "sftp" ? 22 : 21)) {
            remote.options.emplace_back("port", *given);
        }
        if (!uri.user.empty()) {
            remote.options.emplace_back("user", uri.user);
        }
        remote.path = uri.path;
        return remote;
    }
    if (uri.scheme == "dav" || uri.scheme == "davs") {
        const bool ssl = uri.scheme == "davs";
        remote.backend = "webdav";
        const bool ipv6 = has(uri.host, ":");
        const std::optional<std::string> given = port(ssl ? 443 : 80);
        remote.options.emplace_back(
            "url", std::format("{}://{}{}{}{}", ssl ? "https" : "http", ipv6 ? "[" : "", uri.host,
                               ipv6 ? "]" : "", given ? ":" + *given : std::string{}) +
                       uri.path);
        if (!uri.user.empty()) {
            remote.options.emplace_back("user", uri.user);
        }
        return remote;
    }
    return std::nullopt;
}

RcloneRemote with_login(RcloneRemote remote, const std::string& user, const std::string& domain) {
    if (user.empty()) {
        drop_option(remote, "user");
    } else {
        set_option(remote, "user", user);
    }
    if (remote.backend == "smb") {
        if (domain.empty()) {
            drop_option(remote, "domain");
        } else {
            set_option(remote, "domain", domain);
        }
    }
    return remote;
}

std::string connection_string(const RcloneRemote& remote) {
    std::string out = ":" + remote.backend;
    for (const auto& [key, value] : remote.options) {
        out += std::format(",{}=\"", key);
        for (const char c : value) {
            out += c;
            if (c == '"') {
                out += '"';
            }
        }
        out += '"';
    }
    return out + ":" + remote.path;
}

std::string password_variable(const RcloneRemote& remote) {
    std::string name = remote.backend;
    std::ranges::transform(name, name.begin(),
                           [](unsigned char c) { return static_cast<char>(std::toupper(c)); });
    return std::format("RCLONE_{}_PASS", name);
}

stdfs::path rclone_mount_point(const stdfs::path& root, std::string_view uri) {
    std::string name;
    if (const std::optional<fs::RemoteUri> remote = fs::parse_remote_uri(uri)) {
        std::string_view path = remote->path;
        while (path.ends_with('/')) {
            path.remove_suffix(1);
        }
        const std::string_view last = path.substr(path.rfind('/') + 1);
        name = name_part(remote->scheme) + "-" + name_part(remote->host);
        if (!last.empty()) {
            name += "-" + name_part(last);
        }
    } else {
        name = "remote";
    }
    return root / std::format("{}-{:08x}", name, fnv1a(uri));
}

std::set<stdfs::path> rclone_mounts(std::string_view mountinfo, const stdfs::path& root) {
    std::set<stdfs::path> out;
    const stdfs::path base = root.lexically_normal();
    while (!mountinfo.empty()) {
        const std::size_t end = std::min(mountinfo.find('\n'), mountinfo.size());
        std::string_view line = mountinfo.substr(0, end);
        mountinfo.remove_prefix(std::min(end + 1, mountinfo.size()));
        // id parent major:minor root mount-point options [optional...] - type source ...
        std::vector<std::string_view> fields;
        while (!line.empty()) {
            const std::size_t space = std::min(line.find(' '), line.size());
            fields.push_back(line.substr(0, space));
            line.remove_prefix(std::min(space + 1, line.size()));
        }
        const auto separator = std::ranges::find(fields, std::string_view{"-"});
        if (fields.size() < 5 || separator == fields.end() || separator + 1 == fields.end() ||
            *(separator + 1) != "fuse.rclone") {
            continue;
        }
        const stdfs::path point = stdfs::path{decode_octal(fields[4])}.lexically_normal();
        if (point.parent_path() == base) {
            out.insert(point);
        }
    }
    return out;
}

std::vector<fs::Place> with_rclone_mounts(std::vector<fs::Place> places, const stdfs::path& root,
                                          const std::set<stdfs::path>& mounted) {
    for (fs::Place& place : places) {
        if (place.uri.empty()) {
            continue;
        }
        const stdfs::path point = rclone_mount_point(root, place.uri).lexically_normal();
        if (mounted.contains(point)) {
            place.path = point;
            place.disk.mounted = true;
            place.disk.ejectable = true;
        }
    }
    return places;
}

std::string rclone_error(std::string_view output) {
    std::vector<std::string_view> lines;
    while (!output.empty()) {
        const std::size_t end = std::min(output.find('\n'), output.size());
        if (const std::string_view line = trim(output.substr(0, end)); !line.empty()) {
            lines.push_back(line);
        }
        output.remove_prefix(std::min(end + 1, output.size()));
    }
    std::string_view found;
    for (auto it = lines.rbegin(); it != lines.rend() && found.empty(); ++it) {
        for (const std::string_view marker : {"CRITICAL: ", "ERROR : "}) {
            if (const std::size_t at = it->find(marker); at != std::string_view::npos) {
                found = it->substr(at + marker.size());
                break;
            }
        }
    }
    if (found.empty()) {
        if (lines.empty()) {
            return "rclone failed";
        }
        found = lines.back();
        // "2026/11/12 22:29:44 NOTICE: ...": without the time stamp and level.
        if (const std::size_t colon = found.find(": ");
            colon != std::string_view::npos && found.size() > 20 && found[4] == '/') {
            found.remove_prefix(colon + 2);
        }
    }
    // What rclone puts before the server's own words.
    bool cut = true;
    while (cut) {
        cut = false;
        for (const std::string_view prefix :
             {"Fatal error: ", "error listing: ", "error in ListJSON: ",
              "Failed to create file system for "}) {
            if (!found.starts_with(prefix)) {
                continue;
            }
            found.remove_prefix(prefix.size());
            if (prefix == "Failed to create file system for ") {
                // the remote's name, then ": "
                const std::size_t after = found.find("\": ");
                found.remove_prefix(after == std::string_view::npos ? 0 : after + 3);
            }
            cut = true;
        }
    }
    return std::string{found};
}

bool wants_login(std::string_view error) {
    // What the backends say then: WebDAV's 401, smb's logon and access errors, ssh's and ftp's.
    static constexpr std::array<std::string_view, 9> kSigns{
        "401",         "unauthorized", "logon",           "permission denied", "access denied",
        "authenticat", "530 ",         "login incorrect", "password"};
    const std::string text = lower(error);
    return std::ranges::any_of(kSigns, [&text](std::string_view sign) { return has(text, sign); });
}

struct Rclone::Impl {
    core::EventLoop& loop;
    core::WorkerPool& pool;
    stdfs::path program;
    stdfs::path root;
    std::function<void()> changed;

    // One mount: what it asks with, and the login the person gave so far.
    struct Attempt {
        std::string uri;
        std::string label;
        RcloneRemote remote;
        stdfs::path where;
        Prompts prompts;
        Mounted done;
        std::string user;
        std::string domain;
        std::string password;
        int asked{0};
        bool tried_guest{false};
    };

    [[nodiscard]] std::set<stdfs::path> mounted() const {
        return rclone_mounts(fs::read_proc_file("/proc/self/mountinfo"), root);
    }

    static void attempt(const std::shared_ptr<Impl>& impl, const std::shared_ptr<Attempt>& job) {
        const RcloneRemote remote =
            with_known_hosts(with_login(job->remote, job->user, job->domain));
        impl->pool.submit(
            core::Priority::visible,
            [program = impl->program, remote, where = job->where,
             password = job->password](const core::CancelToken& /*token*/) {
                return mount_now(program, remote, where, password);
            },
            [weak = std::weak_ptr<Impl>{impl}, job](const Outcome& outcome) {
                const std::shared_ptr<Impl> self = weak.lock();
                if (!self) {
                    return;
                }
                if (outcome.mounted) {
                    log::info("rclone: mounted {} on {}", job->uri, job->where.string());
                    if (self->changed) {
                        self->changed();
                    }
                    job->done(job->where, {});
                    return;
                }
                if (!wants_login(outcome.error)) {
                    job->done({}, outcome.error);
                    return;
                }
                // An smb bookmark without a user: once more as guest before asking, so a public
                // share opens without a question (rclone logs in as $USER otherwise).
                if (job->remote.backend == "smb" && job->asked == 0 && !job->tried_guest &&
                    job->user.empty()) {
                    job->tried_guest = true;
                    job->user = "guest";
                    attempt(self, job);
                    return;
                }
                if (job->tried_guest && job->asked == 0) {
                    job->user.clear();  // the card starts empty, not with "guest"
                }
                if (job->prompts.password && job->asked < kMaxLogins) {
                    ask(self, job, outcome.error);
                    return;
                }
                job->done({}, outcome.error);
            });
    }

    static void ask(const std::shared_ptr<Impl>& impl, const std::shared_ptr<Attempt>& job,
                    const std::string& error) {
        const bool smb = job->remote.backend == "smb";
        const bool ftp = job->remote.backend == "ftp";
        PasswordRequest request{
            .message = job->asked == 0
                           ? std::format("Password required for {}", job->label)
                           : std::format("{} did not accept the login ({})", job->label, error),
            .user = job->user,
            .domain = job->domain,
            .flags = PasswordRequest::kNeedPassword | PasswordRequest::kNeedUsername |
                     (smb ? PasswordRequest::kNeedDomain : 0U) |
                     (smb || ftp ? PasswordRequest::kAnonymousSupported : 0U),
        };
        ++job->asked;
        job->prompts.password(request, [weak = std::weak_ptr<Impl>{impl}, job, smb,
                                        ftp](const PasswordAnswer& answer) {
            const std::shared_ptr<Impl> self = weak.lock();
            if (!self) {
                return;
            }
            if (answer.aborted) {
                job->done({}, std::string{Gvfs::kCancelled});
                return;
            }
            if (answer.anonymous) {
                // Samba maps an unknown user to its guest; ftp's anonymous login wants any
                // password.
                job->user = smb ? "guest" : "anonymous";
                job->domain.clear();
                job->password = ftp ? "anonymous" : "";
            } else {
                job->user = answer.user;
                job->domain = answer.domain;
                job->password = answer.password;
            }
            attempt(self, job);
        });
    }
};

Rclone::Rclone(core::EventLoop& loop, core::WorkerPool& pool, stdfs::path program, stdfs::path root,
               std::function<void()> changed)
    : impl_{std::make_shared<Impl>(Impl{.loop = loop,
                                        .pool = pool,
                                        .program = std::move(program),
                                        .root = std::move(root),
                                        .changed = std::move(changed)})} {}

Rclone::~Rclone() = default;

std::optional<stdfs::path> Rclone::find_program() {
    const char* const path = std::getenv("PATH");
    std::string_view dirs = path != nullptr ? path : "";
    while (!dirs.empty()) {
        const std::size_t end = std::min(dirs.find(':'), dirs.size());
        const std::string_view dir = dirs.substr(0, end);
        dirs.remove_prefix(std::min(end + 1, dirs.size()));
        if (dir.empty()) {
            continue;
        }
        const stdfs::path program = stdfs::path{dir} / "rclone";
        if (access(program.c_str(), X_OK) == 0) {
            return program;
        }
    }
    return std::nullopt;
}

stdfs::path Rclone::default_root() {
    const char* const runtime = std::getenv("XDG_RUNTIME_DIR");
    if (runtime != nullptr && *runtime == '/') {
        return stdfs::path{runtime} / "nordend" / "remotes";
    }
    return stdfs::temp_directory_path() / std::format("nordend-{}", getuid()) / "remotes";
}

const stdfs::path& Rclone::root() const noexcept { return impl_->root; }

bool Rclone::holds(std::string_view uri) const {
    return impl_->mounted().contains(rclone_mount_point(impl_->root, uri).lexically_normal());
}

std::vector<fs::Place> Rclone::places(std::vector<fs::Place> places) const {
    return with_rclone_mounts(std::move(places), impl_->root, impl_->mounted());
}

void Rclone::mount(const std::string& uri, Prompts prompts, Mounted done) {
    const std::optional<fs::RemoteUri> parsed = fs::parse_remote_uri(uri);
    const std::optional<RcloneRemote> remote =
        parsed ? rclone_remote(*parsed) : std::optional<RcloneRemote>{};
    if (!remote) {
        done({}, "it is not a network folder Nordend can mount");
        return;
    }
    const stdfs::path where = rclone_mount_point(impl_->root, uri);
    if (impl_->mounted().contains(where.lexically_normal())) {
        done(where, {});
        return;
    }
    // The root is the person's alone, as $XDG_RUNTIME_DIR is.
    std::error_code error;
    stdfs::create_directories(impl_->root, error);
    stdfs::permissions(impl_->root, stdfs::perms::owner_all, error);

    auto job = std::make_shared<Impl::Attempt>();
    job->uri = uri;
    job->label = fs::remote_label(*parsed);
    job->remote = *remote;
    job->where = where;
    job->prompts = std::move(prompts);
    job->done = std::move(done);
    for (const auto& [key, value] : remote->options) {
        if (key == "user") {
            job->user = value;
        } else if (key == "domain") {
            job->domain = value;
        }
    }
    Impl::attempt(impl_, job);
}

void Rclone::unmount(const std::string& uri, Done done) {
    const stdfs::path where = rclone_mount_point(impl_->root, uri);
    if (!impl_->mounted().contains(where.lexically_normal())) {
        done("it is not mounted");
        return;
    }
    impl_->pool.submit(
        core::Priority::visible,
        [where](const core::CancelToken& /*token*/) { return unmount_now(where); },
        [weak = std::weak_ptr<Impl>{impl_}, done = std::move(done)](const std::string& error) {
            const std::shared_ptr<Impl> self = weak.lock();
            if (!self) {
                return;
            }
            if (error.empty() && self->changed) {
                self->changed();
            }
            done(error);
        });
}

}  // namespace nordend::platform
