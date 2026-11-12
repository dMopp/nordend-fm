#include "nordend/fs/places.hpp"

#include <algorithm>
#include <array>
#include <cctype>
#include <cerrno>
#include <charconv>
#include <cstdlib>
#include <cstring>
#include <fcntl.h>
#include <format>
#include <optional>
#include <poll.h>
#include <string_view>
#include <system_error>
#include <unistd.h>
#include <utility>

#include "nordend/core/file.hpp"
#include "nordend/core/log.hpp"
#include "nordend/fs/trash.hpp"

namespace nordend::fs {
namespace {

struct UserDirKind {
    std::string_view key;
    std::string_view name;  // its usual name in the home, without user-dirs.dirs
    std::string_view icon;
};

// The user dirs the sidebar shows, in its order (templates and public share are left out).
constexpr std::array<UserDirKind, 6> kUserDirs{{
    {.key = "DESKTOP", .name = "Desktop", .icon = "device-desktop"},
    {.key = "DOCUMENTS", .name = "Documents", .icon = "file-text"},
    {.key = "DOWNLOAD", .name = "Downloads", .icon = "download"},
    {.key = "MUSIC", .name = "Music", .icon = "music"},
    {.key = "PICTURES", .name = "Pictures", .icon = "photo"},
    {.key = "VIDEOS", .name = "Videos", .icon = "movie"},
}};

constexpr std::array<std::string_view, 3> kMountRoots{"/run/media/", "/media/", "/mnt/"};

// The remote bookmarks the sidebar lists: the schemes gvfs mounts that Nordend maps to a mount.
constexpr std::array<std::string_view, 5> kRemoteSchemes{"smb", "sftp", "ftp", "dav", "davs"};

std::string_view trim(std::string_view text) {
    const std::size_t first = text.find_first_not_of(" \t\r");
    if (first == std::string_view::npos) {
        return {};
    }
    return text.substr(first, text.find_last_not_of(" \t\r") - first + 1);
}

// Calls line(text) for every line of text.
template <typename Fn>
void for_each_line(std::string_view text, const Fn& line) {
    while (!text.empty()) {
        const std::size_t end = std::min(text.find('\n'), text.size());
        line(text.substr(0, end));
        text.remove_prefix(std::min(end + 1, text.size()));
    }
}

int hex_digit(char c) {
    if (c >= '0' && c <= '9') {
        return c - '0';
    }
    if (c >= 'a' && c <= 'f') {
        return c - 'a' + 10;
    }
    if (c >= 'A' && c <= 'F') {
        return c - 'A' + 10;
    }
    return -1;
}

// "%20" -> " "; a malformed escape is kept as it is.
std::string percent_decode(std::string_view text) {
    std::string out;
    out.reserve(text.size());
    for (std::size_t i = 0; i < text.size(); ++i) {
        if (text[i] == '%' && i + 2 < text.size()) {
            const int high = hex_digit(text[i + 1]);
            const int low = hex_digit(text[i + 2]);
            if (high >= 0 && low >= 0) {
                out += static_cast<char>((high * 16) + low);
                i += 2;
                continue;
            }
        }
        out += text[i];
    }
    return out;
}

// "\040" -> " ", as the kernel escapes spaces, tabs, newlines and backslashes in mountinfo.
std::string octal_decode(std::string_view text) {
    std::string out;
    out.reserve(text.size());
    for (std::size_t i = 0; i < text.size(); ++i) {
        if (text[i] == '\\' && i + 3 < text.size()) {
            const std::string_view digits = text.substr(i + 1, 3);
            if (std::ranges::all_of(digits, [](char c) { return c >= '0' && c <= '7'; })) {
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

std::string label_of(const std::filesystem::path& path) {
    const std::filesystem::path name = path.filename();
    return name.empty() ? path.string() : name.string();
}

bool is_folder(const std::filesystem::path& path) {
    std::error_code ignored;
    return std::filesystem::is_directory(path, ignored);
}

}  // namespace

PlaceSources PlaceSources::from_environment() {
    PlaceSources sources;
    const char* const home = std::getenv("HOME");  // NOLINT(concurrency-mt-unsafe): at start
    sources.home = home != nullptr && *home != '\0' ? home : "/";
    const char* const config = std::getenv("XDG_CONFIG_HOME");  // NOLINT(concurrency-mt-unsafe)
    sources.config_home = config != nullptr && *config == '/' ? std::filesystem::path{config}
                                                              : sources.home / ".config";
    return sources;
}

std::vector<UserDir> parse_user_dirs(std::string_view text, const std::filesystem::path& home) {
    std::vector<UserDir> dirs;
    for_each_line(text, [&](std::string_view line) {
        line = trim(line);
        if (line.empty() || line.front() == '#' || !line.starts_with("XDG_")) {
            return;
        }
        const std::size_t equals = line.find('=');
        if (equals == std::string_view::npos) {
            return;
        }
        std::string_view key = trim(line.substr(4, equals - 4));
        if (!key.ends_with("_DIR")) {
            return;
        }
        key.remove_suffix(4);
        std::string_view value = trim(line.substr(equals + 1));
        if (value.size() < 2 || value.front() != '"' || value.back() != '"') {
            return;
        }
        value = value.substr(1, value.size() - 2);
        std::filesystem::path path;
        if (value == "$HOME" || value == "$HOME/") {
            return;  // the home itself: disabled
        }
        if (value.starts_with("$HOME/")) {
            path = home / std::string{value.substr(6)};
        } else if (value.starts_with('/')) {
            path = std::string{value};
        } else {
            return;  // the spec allows only these two forms
        }
        if (path.lexically_normal() == home.lexically_normal()) {
            return;
        }
        dirs.push_back({.key = std::string{key}, .path = std::move(path)});
    });
    return dirs;
}

std::optional<RemoteUri> parse_remote_uri(std::string_view uri) {
    const std::size_t separator = uri.find("://");
    if (separator == std::string_view::npos) {
        return std::nullopt;
    }
    RemoteUri out;
    for (const char c : uri.substr(0, separator)) {
        out.scheme += static_cast<char>(std::tolower(static_cast<unsigned char>(c)));
    }
    if (std::ranges::find(kRemoteSchemes, out.scheme) == kRemoteSchemes.end()) {
        return std::nullopt;
    }
    std::string_view rest = uri.substr(separator + 3);
    rest = rest.substr(0, rest.find_first_of("?#"));
    const std::size_t slash = rest.find('/');
    std::string_view authority = rest.substr(0, slash);
    const std::string_view path =
        slash == std::string_view::npos ? std::string_view{} : rest.substr(slash);
    if (const std::size_t at = authority.rfind('@'); at != std::string_view::npos) {
        out.user = percent_decode(authority.substr(0, at));
        authority.remove_prefix(at + 1);
    }
    std::string_view host = authority;
    std::string_view port;
    if (authority.starts_with('[')) {  // an IPv6 literal: [::1]:22
        const std::size_t close = authority.find(']');
        if (close == std::string_view::npos) {
            return std::nullopt;
        }
        host = authority.substr(1, close - 1);
        const std::string_view after = authority.substr(close + 1);
        if (!after.empty()) {
            if (after.front() != ':') {
                return std::nullopt;
            }
            port = after.substr(1);
        }
    } else if (const std::size_t colon = authority.rfind(':'); colon != std::string_view::npos) {
        host = authority.substr(0, colon);
        port = authority.substr(colon + 1);
    }
    out.host = percent_decode(host);
    if (out.host.empty()) {
        return std::nullopt;
    }
    if (!port.empty()) {
        int number = 0;
        const auto [end, error] = std::from_chars(port.data(), port.data() + port.size(), number);
        if (error != std::errc{} || end != port.data() + port.size() || number < 1 ||
            number > 65535) {
            return std::nullopt;
        }
        out.port = number;
    }
    out.path = path.empty() ? std::string{"/"} : percent_decode(path);
    return out;
}

std::string remote_label(const RemoteUri& uri) {
    std::string_view path = uri.path;
    while (path.size() > 1 && path.back() == '/') {
        path.remove_suffix(1);
    }
    const std::string_view name = path.substr(path.rfind('/') + 1);
    return name.empty() ? uri.host : std::format("{} on {}", name, uri.host);
}

std::vector<Place> parse_bookmarks(std::string_view text) {
    std::vector<Place> places;
    for_each_line(text, [&](std::string_view line) {
        line = trim(line);
        const std::size_t space = line.find(' ');
        const std::string_view uri = line.substr(0, space);
        const std::string_view label =
            space == std::string_view::npos ? std::string_view{} : trim(line.substr(space + 1));
        if (!uri.starts_with("file://")) {
            if (const std::optional<RemoteUri> remote = parse_remote_uri(uri)) {
                // Not mounted yet: gvfs mounts it when chosen, and gives it a path then.
                places.push_back(
                    {.section = Place::Section::bookmarks,
                     .label = label.empty() ? remote_label(*remote) : std::string{label},
                     .path = {},
                     .icon = "server",
                     .disk = {.volume = {}, .mounted = false, .ejectable = false},
                     .uri = std::string{uri}});
            }
            return;
        }
        const std::string path = percent_decode(uri.substr(7));
        if (path.empty() || path.front() != '/') {
            return;  // file://host/path: not ours
        }
        places.push_back({.section = Place::Section::bookmarks,
                          .label = label.empty() ? label_of(path) : std::string{label},
                          .path = path,
                          .icon = "bookmark",
                          .disk = {},
                          .uri = {}});
    });
    return places;
}

namespace {

// The local folder a bookmarks line names; nullopt for anything else.
std::optional<std::filesystem::path> bookmark_folder(std::string_view line) {
    line = trim(line);
    if (!line.starts_with("file://")) {
        return std::nullopt;
    }
    const std::string path = percent_decode(line.substr(7, line.find(' ') - 7));
    if (path.empty() || path.front() != '/') {
        return std::nullopt;
    }
    return std::filesystem::path{path}.lexically_normal();
}

// path normalized, without a trailing slash ("/a/b/" is "/a/b"; "/" stays).
std::filesystem::path plain_folder(const std::filesystem::path& path) {
    std::filesystem::path normal = path.lexically_normal();
    return normal.has_filename() || normal == normal.root_path() ? normal : normal.parent_path();
}

bool same_folder(const std::filesystem::path& a, const std::filesystem::path& b) {
    return plain_folder(a) == plain_folder(b);
}

}  // namespace

std::filesystem::path bookmarks_file(const PlaceSources& sources) {
    return sources.config_home / "gtk-3.0" / "bookmarks";
}

std::optional<std::string> add_bookmark(std::string_view text,
                                        const std::filesystem::path& folder) {
    bool present = false;
    for_each_line(text, [&](std::string_view line) {
        if (const std::optional<std::filesystem::path> path = bookmark_folder(line)) {
            present = present || same_folder(*path, folder);
        }
    });
    if (present) {
        return std::nullopt;
    }
    std::string out{text};
    if (!out.empty() && out.back() != '\n') {
        out += '\n';
    }
    out += core::file_uri(plain_folder(folder));
    out += '\n';
    return out;
}

std::optional<std::string> remove_bookmark(std::string_view text,
                                           const std::filesystem::path& folder) {
    std::string out;
    bool removed = false;
    while (!text.empty()) {
        const std::size_t end = std::min(text.find('\n'), text.size());
        const std::string_view line = text.substr(0, std::min(end + 1, text.size()));
        const std::optional<std::filesystem::path> path = bookmark_folder(text.substr(0, end));
        if (path && same_folder(*path, folder)) {
            removed = true;
        } else {
            out += line;
        }
        text.remove_prefix(line.size());
    }
    if (!removed) {
        return std::nullopt;
    }
    return out;
}

std::optional<std::string> remove_remote_bookmark(std::string_view text, std::string_view uri) {
    std::string out;
    bool removed = false;
    while (!text.empty()) {
        const std::size_t end = std::min(text.find('\n'), text.size());
        const std::string_view line = text.substr(0, std::min(end + 1, text.size()));
        const std::string_view entry = trim(text.substr(0, end));
        if (!uri.empty() && entry.substr(0, entry.find(' ')) == uri) {
            removed = true;
        } else {
            out += line;
        }
        text.remove_prefix(line.size());
    }
    if (!removed) {
        return std::nullopt;
    }
    return out;
}

std::vector<Place> parse_mounts(std::string_view text) {
    std::vector<Place> places;
    for_each_line(text, [&](std::string_view line) {
        // "36 25 8:1 / /run/media/me/DATA rw,noatime - ext4 /dev/sda1 rw": the fifth field.
        std::size_t start = 0;
        for (int field = 0; field < 4; ++field) {
            start = line.find(' ', start);
            if (start == std::string_view::npos) {
                return;
            }
            ++start;
        }
        const std::size_t end = std::min(line.find(' ', start), line.size());
        const std::string point = octal_decode(line.substr(start, end - start));
        const bool under = std::ranges::any_of(kMountRoots, [&](std::string_view root) {
            return point.size() > root.size() && point.starts_with(root);
        });
        if (!under) {
            return;
        }
        Place place{.section = Place::Section::devices,
                    .label = label_of(point),
                    .path = point,
                    .icon = "database",
                    .disk = {}};
        if (std::ranges::find(places, place) == places.end()) {  // mounted twice: listed once
            places.push_back(std::move(place));
        }
    });
    return places;
}

std::string read_proc_file(const std::filesystem::path& path) {
    std::string text;
    const int fd = open(path.c_str(), O_RDONLY | O_CLOEXEC);
    if (fd < 0) {
        return text;
    }
    std::array<char, 4096> buffer{};
    for (;;) {
        const ssize_t length = read(fd, buffer.data(), buffer.size());
        if (length < 0 && errno == EINTR) {
            continue;
        }
        if (length <= 0) {
            break;
        }
        text.append(buffer.data(), static_cast<std::size_t>(length));
    }
    close(fd);
    return text;
}

std::filesystem::path templates_dir(const PlaceSources& sources) {
    const core::TextFile user_dirs = core::read_text_file(sources.config_home / "user-dirs.dirs");
    std::filesystem::path path = sources.home / "Templates";
    if (user_dirs.exists) {
        const std::vector<UserDir> dirs = parse_user_dirs(user_dirs.text, sources.home);
        const auto it = std::ranges::find(dirs, std::string_view{"TEMPLATES"}, &UserDir::key);
        if (it == dirs.end()) {
            return {};
        }
        path = it->path;
    }
    return is_folder(path) ? path : std::filesystem::path{};
}

std::vector<Place> load_places(const PlaceSources& sources) {
    std::vector<Place> places;
    places.push_back({.section = Place::Section::places,
                      .label = "Home",
                      .path = sources.home,
                      .icon = "home",
                      .disk = {}});

    const core::TextFile user_dirs = core::read_text_file(sources.config_home / "user-dirs.dirs");
    const std::vector<UserDir> dirs = parse_user_dirs(user_dirs.text, sources.home);
    for (const UserDirKind& kind : kUserDirs) {
        std::filesystem::path path;
        if (user_dirs.exists) {
            const auto it = std::ranges::find(dirs, kind.key, &UserDir::key);
            if (it == dirs.end()) {
                continue;
            }
            path = it->path;
        } else {
            path = sources.home / kind.name;
        }
        if (is_folder(path)) {
            places.push_back({.section = Place::Section::places,
                              .label = label_of(path),
                              .path = path,
                              .icon = kind.icon,
                              .disk = {}});
        }
    }
    places.push_back({.section = Place::Section::places,
                      .label = "File system",
                      .path = "/",
                      .icon = "server",
                      .disk = {}});
    places.push_back({.section = Place::Section::places,
                      .label = "Trash",
                      .path = trash_path(),
                      .icon = "trash",
                      .disk = {}});

    std::ranges::move(
        parse_bookmarks(core::read_text_file(sources.config_home / "gtk-3.0" / "bookmarks").text),
        std::back_inserter(places));
    std::ranges::move(parse_mounts(read_proc_file(sources.mountinfo)), std::back_inserter(places));
    return places;
}

PlacesWatcher::PlacesWatcher(core::EventLoop& loop, PlaceSources sources,
                             std::function<void()> changed)
    : loop_{loop},
      sources_{std::move(sources)},
      changed_{std::move(changed)},
      places_{load_places(sources_)},
      // The mount table signals a mount or an unmount as a priority event (proc(5)).
      mountinfo_fd_{open(sources_.mountinfo.c_str(), O_RDONLY | O_CLOEXEC)} {
    if (mountinfo_fd_ >= 0) {
        mountinfo_source_ =
            loop_.add_fd(mountinfo_fd_, POLLPRI, [this](short /*revents*/) { reload(); });
    } else {
        log::warn("places: cannot open {}: {}", sources_.mountinfo.string(),
                  std::strerror(errno));  // NOLINT(concurrency-mt-unsafe)
    }
    watch_files();
}

void PlacesWatcher::watch_files() {
    // Only folders that exist: the file watcher would create them.
    for (const std::filesystem::path& file :
         {sources_.config_home / "user-dirs.dirs", bookmarks_file(sources_)}) {
        if (!watched_.contains(file) && is_folder(file.parent_path())) {
            watched_.insert(file);
            files_.push_back(
                std::make_unique<core::FileWatcher>(loop_, file, [this] { reload(); }));
        }
    }
}

PlacesWatcher::~PlacesWatcher() {
    loop_.remove(mountinfo_source_);
    if (mountinfo_fd_ >= 0) {
        close(mountinfo_fd_);
    }
}

void PlacesWatcher::reload() {
    watch_files();
    std::vector<Place> places = load_places(sources_);
    if (places == places_) {
        return;  // a mount elsewhere (a container, a bind mount)
    }
    places_ = std::move(places);
    if (changed_) {
        changed_();
    }
}

}  // namespace nordend::fs
