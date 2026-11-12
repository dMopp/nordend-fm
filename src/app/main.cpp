#include <GLES3/gl3.h>
#include <algorithm>
#include <array>
#include <charconv>
#include <chrono>
#include <csignal>
#include <cstdint>
#include <cstdio>
#include <cstdlib>
#include <exception>
#include <filesystem>
#include <format>
#include <functional>
#include <iterator>
#include <memory>
#include <optional>
#include <poll.h>
#include <stdexcept>
#include <string>
#include <string_view>
#include <sys/signalfd.h>
#include <system_error>
#include <unistd.h>
#include <utility>
#include <vector>

#include "nordend/core/config.hpp"
#include "nordend/core/event_loop.hpp"
#include "nordend/core/file_watcher.hpp"
#include "nordend/core/log.hpp"
#include "nordend/core/memory.hpp"
#include "nordend/core/worker_pool.hpp"
#include "nordend/engines/engines.hpp"
#include "nordend/fs/directory_watcher.hpp"
#include "nordend/fs/mime.hpp"
#include "nordend/fs/places.hpp"
#include "nordend/isolate/helpers.hpp"
#include "nordend/platform/about.hpp"
#include "nordend/platform/desktop_entry.hpp"
#include "nordend/platform/gvfs.hpp"
#include "nordend/platform/opener.hpp"
#include "nordend/platform/portal.hpp"
#include "nordend/platform/rclone.hpp"
#include "nordend/platform/udisks.hpp"
#include "nordend/theme/app_icon.hpp"
#include "nordend/theme/theme.hpp"
#include "nordend/thumbs/cache.hpp"
#include "nordend/ui/activation.hpp"
#include "nordend/ui/clipboard.hpp"
#include "nordend/ui/display.hpp"
#include "nordend/ui/egl_context.hpp"
#include "nordend/ui/icons.hpp"
#include "nordend/ui/offscreen.hpp"
#include "nordend/ui/seat.hpp"
#include "nordend/ui/text.hpp"
#include "nordend/ui/window.hpp"
#include "nordend/views/browser.hpp"
#include "nordend/views/state.hpp"
#include "nordend/views/title_bar.hpp"
#include "test_scene.hpp"

namespace {

namespace core = nordend::core;
namespace engines = nordend::engines;
namespace isolate = nordend::isolate;
namespace fs = nordend::fs;
namespace log = nordend::log;
namespace platform = nordend::platform;
namespace theme = nordend::theme;
namespace thumbs = nordend::thumbs;
namespace ui = nordend::ui;
namespace views = nordend::views;

// SIGINT/SIGTERM delivered as a readable fd, so Ctrl+C ends the loop and runs every
// destructor instead of killing the process mid-flight; SIGUSR1 asks for the memory report.
class SignalFd {
public:
    SignalFd() : mask_{block_signals()}, fd_{signalfd(-1, &mask_, SFD_CLOEXEC | SFD_NONBLOCK)} {
        if (fd_ < 0) {
            throw std::runtime_error{"signalfd failed"};
        }
    }

    ~SignalFd() {
        if (fd_ >= 0) {
            close(fd_);
        }
    }

    SignalFd(const SignalFd&) = delete;
    SignalFd& operator=(const SignalFd&) = delete;
    SignalFd(SignalFd&&) = delete;
    SignalFd& operator=(SignalFd&&) = delete;

    [[nodiscard]] int fd() const noexcept { return fd_; }

private:
    static sigset_t block_signals() {
        sigset_t mask{};
        sigemptyset(&mask);
        sigaddset(&mask, SIGINT);
        sigaddset(&mask, SIGTERM);
        sigaddset(&mask, SIGUSR1);
        // Writing to a program that stopped reading the clipboard: EPIPE, not death. Started
        // applications get an empty mask (Launcher).
        sigaddset(&mask, SIGPIPE);
        sigprocmask(SIG_BLOCK, &mask, nullptr);
        return mask;
    }

    sigset_t mask_;
    int fd_;
};

// Start timing (ROADMAP 7.6). nordend-bench-start puts the steady-clock time of its exec, in
// nanoseconds, in NORDEND_STARTUP_NS; every step then prints "startup: STEP MS" on stderr, the
// milliseconds since that exec. With NORDEND_STARTUP_QUIT set, Nordend closes once its folder is
// shown. Without NORDEND_STARTUP_NS nothing is measured.
class StartupTrace {
public:
    StartupTrace() {
        const char* start = std::getenv("NORDEND_STARTUP_NS");  // NOLINT(concurrency-mt-unsafe)
        if (start == nullptr) {
            return;
        }
        const std::string_view text{start};
        std::int64_t nanoseconds = 0;
        const auto [end, error] =
            std::from_chars(text.data(), text.data() + text.size(), nanoseconds);
        if (error != std::errc{} || end != text.data() + text.size()) {
            return;
        }
        start_ = std::chrono::steady_clock::time_point{std::chrono::nanoseconds{nanoseconds}};
        const char* quit = std::getenv("NORDEND_STARTUP_QUIT");  // NOLINT(concurrency-mt-unsafe)
        quit_ = quit != nullptr && *quit != '\0';
    }

    [[nodiscard]] bool active() const noexcept { return start_.has_value(); }

    [[nodiscard]] bool quit_when_shown() const noexcept { return quit_; }

    void mark(std::string_view step) const {
        if (!start_) {
            return;
        }
        const double ms =
            std::chrono::duration<double, std::milli>(std::chrono::steady_clock::now() - *start_)
                .count();
        std::fprintf(stderr, "startup: %.*s %.2f ms\n", static_cast<int>(step.size()), step.data(),
                     ms);
    }

private:
    std::optional<std::chrono::steady_clock::time_point> start_;
    bool quit_{false};
};

// "Ctrl+Shift+a  text 'A'  (repeat)": what the test scene shows for the last key.
std::string describe(const ui::KeyEvent& event) {
    std::string out;
    const ui::Modifiers& mods = event.modifiers;
    for (const auto& [active, name] :
         {std::pair{mods.ctrl, "Ctrl+"}, std::pair{mods.alt, "Alt+"},
          std::pair{mods.shift, "Shift+"}, std::pair{mods.super, "Super+"}}) {
        if (active) {
            out += name;
        }
    }
    std::array<char, 64> name{};
    xkb_keysym_get_name(event.keysym, name.data(), name.size());
    out += name.data();
    if (!event.text.empty()) {
        out += std::format("   text \u201c{}\u201d", event.text);
    }
    if (event.repeat) {
        out += "   (repeat)";
    }
    return out;
}

const char* button_name(ui::PointerButton button) {
    switch (button) {
        case ui::PointerButton::left:
            return "left";
        case ui::PointerButton::right:
            return "right";
        case ui::PointerButton::middle:
            return "middle";
        case ui::PointerButton::back:
            return "back";
        case ui::PointerButton::forward:
            return "forward";
        case ui::PointerButton::other:
            break;
    }
    return "other";
}

constexpr const char* kUsage =
    "usage: nordend [--config FILE] [--check-config] [--test-scene] [--version] [--about] [DIR]\n"
    "  DIR             the folder to show (default: your home folder)\n"
    "  --config FILE   read FILE instead of ~/.config/nordend/config.toml\n"
    "  --test-scene    show the renderer test scene instead of a folder\n"
    "  --check-config  report problems in the config file and exit (status 1 if any)\n"
    "  --version       print the version and exit\n"
    "  --about         print what Nordend runs on, for a bug report, and exit\n";

// The folder to open: DIR as given (made absolute, ".." resolved), else $HOME, else "/".
std::filesystem::path start_folder(const std::filesystem::path& given) {
    std::filesystem::path folder = given;
    if (folder.empty()) {
        const char* home = std::getenv("HOME");  // NOLINT(concurrency-mt-unsafe): startup
        folder = home != nullptr && home[0] != '\0' ? home : "/";
    }
    std::error_code error;
    const std::filesystem::path absolute = std::filesystem::weakly_canonical(folder, error);
    return error ? folder : absolute;
}

// NORDEND_FORCE_CSD set and not "0": Nordend's own title bar even where the compositor draws one.
bool client_decorations_forced() {
    const char* value = std::getenv("NORDEND_FORCE_CSD");  // NOLINT(concurrency-mt-unsafe): startup
    return value != nullptr && *value != '\0' && std::string_view{value} != "0";
}

// --about: what Nordend runs on, one "Label: value" line each, to paste into a bug report.
int print_about() {
    std::vector<platform::AboutLine> lines;
    const std::string_view commit = NORDEND_COMMIT;
    lines.push_back({.label = "Nordend",
                     .value = commit.empty() ? std::string{NORDEND_VERSION}
                                             : std::format("{} ({})", NORDEND_VERSION, commit)});
    std::ranges::move(platform::system_about(), std::back_inserter(lines));
    // The engines found, and where their programs are: an installation in one line.
    const std::filesystem::path directory = engines::default_directory();
    const engines::Registry registry = engines::Registry::load(directory, engines::locate_program);
    std::string found;
    for (const engines::Engine& engine : registry.engines()) {
        found += std::format("{}{} ({})", found.empty() ? "" : ", ", engine.manifest.name,
                             engine.program.string());
    }
    if (found.empty()) {
        found = directory.empty() ? "none found" : std::format("none in {}", directory.string());
    }
    lines.push_back({.label = "Engines", .value = found});
    // The GPU as OpenGL names it, from a context without a window.
    try {
        const ui::Offscreen offscreen{1, 1};
        const auto* name = reinterpret_cast<const char*>(glGetString(GL_RENDERER));  // NOLINT
        if (name != nullptr && *name != '\0') {
            lines.push_back({.label = "Graphics", .value = name});
        }
    } catch (const std::exception& error) {
        lines.push_back({.label = "Graphics", .value = std::format("unknown ({})", error.what())});
    }
    std::size_t width = 0;
    for (const platform::AboutLine& line : lines) {
        width = std::max(width, line.label.size());
    }
    for (const platform::AboutLine& line : lines) {
        std::printf("%-*s %s\n", static_cast<int>(width + 1), (line.label + ":").c_str(),
                    line.value.c_str());
    }
    return 0;
}

// --check-config: every problem on stderr, a one-line verdict on stdout.
int check_config(const std::filesystem::path& path, const core::ConfigResult& result) {
    for (const std::string& problem : result.problems) {
        std::fprintf(stderr, "%s\n", problem.c_str());
    }
    if (!result.problems.empty()) {
        std::printf("%s: %zu problem(s)\n", path.c_str(), result.problems.size());
        return 1;
    }
    std::printf("%s: %s\n", path.c_str(), result.from_file ? "ok" : "not found, defaults in use");
    return 0;
}

// What gvfs asks before a mount, answered on the Browser's cards.
platform::Gvfs::Prompts remote_prompts(views::Remotes::Prompts prompts) {
    using platform::PasswordRequest;
    return {
        .password =
            [login = std::move(prompts.login)](
                const PasswordRequest& request,
                std::function<void(const platform::PasswordAnswer&)> answer) {
                if (!login) {
                    answer({});
                    return;
                }
                const auto asks = [&request](std::uint32_t flag) {
                    return (request.flags & flag) != 0;
                };
                login({.message = request.message,
                       .user = request.user,
                       .domain = request.domain,
                       .ask_user = asks(PasswordRequest::kNeedUsername),
                       .ask_domain = asks(PasswordRequest::kNeedDomain),
                       .ask_password = asks(PasswordRequest::kNeedPassword),
                       .anonymous = asks(PasswordRequest::kAnonymousSupported),
                       .saving = asks(PasswordRequest::kSavingSupported)},
                      [answer = std::move(answer)](const std::optional<views::Login>& given) {
                          platform::PasswordAnswer out;
                          if (given) {
                              out = {.aborted = false,
                                     .anonymous = given->anonymous,
                                     .user = given->user,
                                     .domain = given->domain,
                                     .password = given->password,
                                     .remember = given->remember};
                          }
                          answer(out);
                      });
            },
        .question = std::move(prompts.question),
        .cancel = std::move(prompts.cancel),
    };
}

}  // namespace

int main(int argc, char** argv) {
    core::give_back_large_blocks();  // before any thread: decoded photos go back to the system
    const StartupTrace startup;
    startup.mark("main");
    std::filesystem::path config_path;
    std::filesystem::path start_dir;
    bool check_only = false;
    bool test_scene = false;
    for (int i = 1; i < argc; ++i) {
        const std::string_view arg{argv[i]};
        if (arg == "--version") {
            std::puts("nordend " NORDEND_VERSION);
            return 0;
        }
        if (arg == "--about") {
            return print_about();
        }
        if (arg == "--check-config") {
            check_only = true;
        } else if (arg == "--test-scene") {
            test_scene = true;
        } else if (arg == "--config") {
            if (i + 1 == argc) {
                std::fprintf(stderr, "nordend: --config needs a file\n%s", kUsage);
                return 2;
            }
            config_path = argv[++i];
        } else if (!arg.starts_with("-") && start_dir.empty()) {
            start_dir = arg;
        } else {
            std::fprintf(stderr, "nordend: unknown option '%s'\n%s", argv[i], kUsage);
            return 2;
        }
    }
    if (config_path.empty()) {
        config_path = core::default_config_path();
    }
    const core::ConfigResult config_result = core::load_config(config_path);
    if (check_only) {
        return check_config(config_path, config_result);
    }

    log::init_from_env();
    log::info("nordend {}", NORDEND_VERSION);
    if (config_result.from_file) {
        log::info("config: {}", config_path.string());
    } else {
        log::info("config: {} not used, defaults in use", config_path.string());
    }
    for (const std::string& problem : config_result.problems) {
        log::warn("config: {}", problem);
    }
    const core::Config& config = config_result.config;

    try {
        // Before any other thread exists, so every thread inherits the blocked signals.
        const SignalFd signals;
        core::EventLoop loop;
        // Set once the browser exists: what it holds, for the memory report.
        std::function<std::vector<std::string>()> memory_lines;
        loop.add_fd(signals.fd(), POLLIN, [&](short /*revents*/) {
            signalfd_siginfo info{};
            if (read(signals.fd(), &info, sizeof info) != sizeof info) {
                return;
            }
            if (info.ssi_signo == SIGPIPE) {
                return;  // a clipboard reader went away: the write already failed with EPIPE
            }
            if (info.ssi_signo == SIGUSR1) {
                // A development report (ROADMAP 6.3b): pkill -USR1 nordend.
                for (const std::string& line : core::describe(core::read_memory_report())) {
                    log::info("memory: {}", line);
                }
                for (const std::string& line :
                     memory_lines ? memory_lines() : std::vector<std::string>{}) {
                    log::info("memory: {}", line);
                }
                return;
            }
            log::info("signal received, closing");
            loop.quit(0);
        });

        ui::Display display;
        startup.mark("display");
        ui::EglContext egl{display};
        startup.mark("egl");
        ui::FontSystem fonts{config.font.family, config.font.monospace};
        startup.mark("fonts");
        theme::Theme theme{theme::default_colors_path()};
        theme.watch(loop);
        // Without colors.toml, the desktop's light/dark and accent (ROADMAP 7.4).
        std::unique_ptr<platform::DesktopPortal> portal;
        const auto follow_desktop = [&] {
            if (!portal->available()) {
                return;
            }
            const platform::Appearance& appearance = portal->appearance();
            theme::Desktop desktop{.dark = appearance.dark, .accent = std::nullopt};
            if (appearance.accent) {
                const auto [r, g, b] = *appearance.accent;
                desktop.accent = ui::Color{static_cast<float>(r), static_cast<float>(g),
                                           static_cast<float>(b), 1.0F};
            }
            theme.set_desktop(desktop);
        };
        portal = std::make_unique<platform::DesktopPortal>(loop, follow_desktop);
        follow_desktop();
        startup.mark("portal");
        // How Nordend was left (ROADMAP 7.5a).
        const std::filesystem::path state_path = views::default_state_path();
        const views::StateResult remembered = views::load_state(state_path);
        for (const std::string& problem : remembered.problems) {
            log::warn("state: {}", problem);
        }
        ui::Window window{display,
                          egl,
                          fonts,
                          {.title = "Nordend",
                           .app_id = "nordend",
                           .width = remembered.state.window_width,
                           .height = remembered.state.window_height,
                           .maximized = remembered.state.maximized,
                           .background = theme.palette().surface,
                           .force_client_decorations = client_decorations_forced()}};
        startup.mark("window");
        ui::Seat seat{display};
        seat.set_multi_click(std::chrono::milliseconds{config.pointer.double_click_ms},
                             static_cast<float>(config.pointer.double_click_distance));
        display.attach(loop);
        seat.attach(loop);
        window.set_close_handler([&] {
            log::info("window closed");
            loop.quit(0);
        });

        core::WorkerPool pool{loop};
        // Pictures are made by the engines in their cages (ROADMAP 2b, 3a), never here.
        {
            auto registry = std::make_shared<const engines::Registry>(
                engines::Registry::load(engines::default_directory(), engines::locate_program));
            isolate::use_engines(*registry);
            engines::set_installed(std::move(registry));
        }
        // Nordend's icon in the desktop's accent (ROADMAP 9.2), written on a worker.
        bool follow_accent = config.icon.follow_accent;
        theme::AppIconKeeper app_icon{pool, theme::default_app_icon_place()};
        const auto update_icon = [&] {
            app_icon.follow(follow_accent ? theme.accent() : std::nullopt);
        };
        theme.set_change_handler([&] {
            window.set_background(theme.palette().surface);
            update_icon();
        });
        update_icon();
        fs::DirectoryWatcher watcher{loop};
        ui::IconSet icons{fonts};
        ui::Activation activation{display, loop};
        ui::Clipboard clipboard{display, loop};  // before the browser that uses it
        // The column view and the grid (ROADMAP 4.2, 4.4), unless the renderer test scene was
        // asked for.
        std::unique_ptr<views::Browser> columns;
        // config.toml is followed while Nordend runs: the double click changes at once, the
        // fonts at the next start (`config` keeps the ones in use).
        std::unique_ptr<core::FileWatcher> config_watcher;
        const auto apply_config = [&](const core::Config& fresh) {
            seat.set_multi_click(std::chrono::milliseconds{fresh.pointer.double_click_ms},
                                 static_cast<float>(fresh.pointer.double_click_distance));
            follow_accent = fresh.icon.follow_accent;
            update_icon();
        };
        // Writes state.toml as things change; the window's size is the one it has when neither
        // maximized, fullscreen nor tiled, so a maximized window comes back to it.
        views::StateKeeper keeper{loop, pool, state_path, remembered.state};
        std::int32_t restored_width = remembered.state.window_width;
        std::int32_t restored_height = remembered.state.window_height;
        const auto note_state = [&] {
            const ui::WindowState& shown = window.state();
            if (!shown.maximized && !shown.fullscreen && !shown.tiled_left && !shown.tiled_right &&
                !shown.tiled_top && !shown.tiled_bottom) {
                restored_width = window.width();
                restored_height = window.height();
            }
            views::State state = columns->state();
            state.window_width = restored_width;
            state.window_height = restored_height;
            state.maximized = shown.maximized;
            keeper.note(state);
        };
        // Files open with their default applications (ROADMAP 6.1).
        std::unique_ptr<platform::Opener> opener;
        if (!test_scene) {
            const std::shared_ptr<const fs::MimeDatabase> mime = fs::MimeDatabase::load();
            columns = std::make_unique<views::Browser>(
                loop, pool, watcher, mime, [&] { window.request_redraw(); },
                [&](ui::CursorShape shape) { seat.set_cursor(shape); },
                std::make_shared<thumbs::Cache>(thumbs::default_root(),
                                                "nordend-" NORDEND_VERSION));
            opener = std::make_unique<platform::Opener>(
                loop, pool, mime, platform::Environment::from_environment(),
                [&](std::string message, bool error) {
                    columns->show_message(std::move(message), error);
                },
                [&](std::function<void(std::string)> done) {
                    activation.request(display.seat(), seat.input_serial(), window.surface(),
                                       std::move(done));
                });
            columns->set_open_handler(
                [&](const std::filesystem::path& directory, std::vector<fs::Entry> files) {
                    opener->open(directory, std::move(files));
                });
            columns->set_open_with_handlers(
                [&](const std::filesystem::path& directory, fs::Entry entry,
                    std::function<void(std::vector<views::Browser::AppChoice>)> done) {
                    opener->choices(
                        directory, std::move(entry),
                        [done = std::move(done)](std::vector<platform::AppChoice> found) {
                            std::vector<views::Browser::AppChoice> apps;
                            apps.reserve(found.size());
                            for (platform::AppChoice& app : found) {
                                apps.push_back({.id = std::move(app.id),
                                                .name = std::move(app.name),
                                                .is_default = app.is_default});
                            }
                            done(std::move(apps));
                        });
                },
                [&](const std::filesystem::path& directory, std::vector<fs::Entry> files,
                    std::string app_id) {
                    opener->open(directory, std::move(files), std::move(app_id));
                });
            columns->set_terminal_handler(
                [&](const std::filesystem::path& directory) { opener->open_terminal(directory); });
            columns->set_quit_handler([&] {
                log::info("quit from the command palette");
                loop.quit(0);
            });
            if (clipboard.available()) {
                columns->set_system_clipboard({
                    .offer =
                        [&](std::vector<views::SystemClipboard::Format> formats) {
                            clipboard.offer(std::move(formats), seat.input_serial());
                        },
                    .clear = [&] { clipboard.clear(seat.input_serial()); },
                    .owned = [&] { return clipboard.owned(); },
                    .generation = [&] { return clipboard.generation(); },
                    .types = [&] { return clipboard.types(); },
                    .read =
                        [&](const std::string& type, views::SystemClipboard::Done done) {
                            clipboard.read(type, std::move(done));
                        },
                });
                clipboard.set_lost_handler([&] { columns->clipboard_lost(); });
                // Drag and drop (ROADMAP 7.1) on the same wl_data_device.
                columns->set_drag_and_drop({
                    .start =
                        [&](std::vector<views::DragAndDrop::Format> formats) {
                            wl_surface* const icon = window.make_drag_icon(
                                views::Browser::kDragIconWidth, views::Browser::kDragIconHeight,
                                [&](ui::Renderer& renderer, const ui::Rect& bounds) {
                                    columns->draw_drag_icon(renderer, bounds, theme.palette(),
                                                            icons);
                                });
                            clipboard.start_drag(std::move(formats), window.surface(), icon,
                                                 seat.input_serial());
                        },
                    .cancel = [&] { clipboard.cancel_drag(); },
                    .types = [&] { return clipboard.drop_types(); },
                    .source_actions = [&] { return clipboard.drop_source_actions(); },
                    .action = [&] { return clipboard.drop_action(); },
                    .accept =
                        [&](const std::optional<std::string>& type, std::uint32_t actions,
                            std::uint32_t preferred) {
                            clipboard.accept_drop(type, actions, preferred);
                        },
                    .read =
                        [&](const std::string& type, views::DragAndDrop::Done done) {
                            clipboard.read_drop(type, std::move(done));
                        },
                    .finish = [&] { clipboard.finish_drop(); },
                    .modifiers = [&] { return seat.modifiers(); },
                });
                clipboard.set_drop_handler(
                    [&](const ui::DropEvent& event) { columns->drop_event(event); });
                clipboard.set_drag_end_handler([&](bool /*dropped*/) {
                    window.drop_drag_icon();
                    columns->drag_ended();
                });
            }
            columns->apply(remembered.state);
            // Without a folder (Super+E) the tabs come back as they were left; with one
            // (xdg-open, "Show in folder") a single tab on it (ROADMAP 9.1d).
            if (!start_dir.empty() ||
                !columns->restore_tabs(remembered.state.tabs, remembered.state.shown_tab)) {
                columns->open(start_folder(start_dir));
            }
            memory_lines = [&columns] { return columns->memory_lines(); };
        }
        // The sidebar's places, kept current as things are mounted and bookmarked (ROADMAP 5.9),
        // with the disks udisks2 knows, mounted or not (ROADMAP 7.3).
        std::unique_ptr<fs::PlacesWatcher> places;
        std::unique_ptr<platform::Udisks> udisks;
        std::unique_ptr<platform::Gvfs> gvfs;
        std::unique_ptr<platform::Rclone> rclone;
        if (columns) {
            const auto show_places = [&] {
                if (places) {
                    std::vector<fs::Place> shown =
                        udisks ? platform::with_volumes(places->places(), udisks->volumes())
                               : places->places();
                    if (gvfs) {
                        shown = platform::with_remote_mounts(std::move(shown), gvfs->mounts());
                    }
                    if (rclone) {
                        shown = rclone->places(std::move(shown));
                    }
                    columns->set_places(std::move(shown));
                }
            };
            places = std::make_unique<fs::PlacesWatcher>(loop, fs::PlaceSources::from_environment(),
                                                         show_places);
            udisks = std::make_unique<platform::Udisks>(loop, show_places);
            if (udisks->available()) {
                columns->set_devices({
                    .mount =
                        [&](const std::string& volume, views::Devices::Mounted done) {
                            udisks->mount(volume, std::move(done));
                        },
                    .unmount =
                        [&](const std::string& volume, views::Devices::Done done) {
                            udisks->unmount(volume, std::move(done));
                        },
                    .eject =
                        [&](const std::string& volume, views::Devices::Done done) {
                            udisks->eject(volume, std::move(done));
                        },
                });
            }
            // Remote bookmarks (smb://, sftp://, ftp://, WebDAV): mounted through gvfs on the
            // session bus where gvfs is installed, else with rclone when that is.
            gvfs = std::make_unique<platform::Gvfs>(loop, show_places);
            if (const std::optional<std::filesystem::path> program =
                    platform::Rclone::find_program()) {
                rclone = std::make_unique<platform::Rclone>(
                    loop, pool, *program, platform::Rclone::default_root(), show_places);
            }
            // Until the bus has said whether gvfs is there, it is taken to be.
            const auto use_gvfs = [&] {
                return gvfs->available() && (!rclone || gvfs->installed().value_or(true));
            };
            if (gvfs->available() || rclone) {
                columns->set_remotes({
                    .mount =
                        [&](const std::string& uri, views::Remotes::Prompts prompts,
                            views::Remotes::Mounted done) {
                            if (use_gvfs()) {
                                gvfs->mount(uri, remote_prompts(std::move(prompts)),
                                            std::move(done));
                            } else {
                                rclone->mount(uri, remote_prompts(std::move(prompts)),
                                              std::move(done));
                            }
                        },
                    // By who mounted it: a share rclone mounted stays rclone's.
                    .unmount =
                        [&](const std::string& uri, views::Remotes::Done done) {
                            if (rclone && (rclone->holds(uri) || !use_gvfs())) {
                                rclone->unmount(uri, std::move(done));
                            } else {
                                gvfs->unmount(uri, std::move(done));
                            }
                        },
                });
            }
            show_places();
            columns->set_places_reload([&places] { places->reload(); });
        }

        // Nordend's own title bar where the compositor draws none (ROADMAP 7.4).
        views::TitleBar title_bar{
            {.move = [&] { window.move(display.seat(), seat.input_serial()); },
             .resize =
                 [&](ui::Edge edge) { window.resize(display.seat(), seat.input_serial(), edge); },
             .minimize = [&] { window.minimize(); },
             .toggle_maximized = [&] { window.toggle_maximized(); },
             .window_menu =
                 [&](float x, float y) {
                     window.show_window_menu(display.seat(), seat.input_serial(), x, y);
                 },
             .close = [&] { window.close(); }},
            [&] { window.request_redraw(); },
            [&](ui::CursorShape shape) { seat.set_cursor(shape); }};
        bool bar_has_pointer = false;  // the content heard a leave when the bar took the pointer
        bool content_pressed = false;  // a press in the content keeps the pointer there

        std::string last_key = "Press a key (Ctrl+Q quits)";
        seat.set_key_handler([&](const ui::KeyEvent& event) {
            if (event.modifiers.ctrl && (event.keysym == XKB_KEY_q || event.keysym == XKB_KEY_Q)) {
                log::info("quit requested");
                loop.quit(0);
                return;
            }
            if (columns) {
                columns->key(event, views::Browser::Clock::now());
                return;
            }
            last_key = describe(event);
            log::debug("key: {}", last_key);
            window.request_redraw();
        });
        // Pointer test (ROADMAP 2.2): a hover/click target and the last pointer event.
        const ui::Rect target{760.0F, 32.0F, 160.0F, 48.0F};
        bool hovered = false;
        int clicks = 0;
        std::string last_pointer = "Move the pointer, click the button, scroll";
        seat.set_pointer_handler([&](const ui::PointerEvent& event) {
            using Kind = ui::PointerEvent::Kind;
            if (window.client_decorations() && !content_pressed) {
                if (title_bar.pointer(event)) {
                    if (!std::exchange(bar_has_pointer, true) && columns) {
                        columns->pointer({.kind = Kind::leave}, views::Browser::Clock::now());
                    }
                    return;
                }
                bar_has_pointer = false;
            }
            if (event.kind == Kind::press) {
                content_pressed = true;
            } else if (event.kind == Kind::release || event.kind == Kind::leave) {
                content_pressed = false;
            }
            if (columns) {
                columns->pointer(event, views::Browser::Clock::now());
                return;
            }
            const bool inside = event.kind != Kind::leave && event.x >= target.x &&
                                event.x < target.right() && event.y >= target.y &&
                                event.y < target.bottom();
            if (inside != hovered) {
                hovered = inside;
                seat.set_cursor(inside ? ui::CursorShape::pointer : ui::CursorShape::standard);
                window.request_redraw();
            }
            switch (event.kind) {
                case Kind::press:
                    if (inside && event.button == ui::PointerButton::left) {
                        ++clicks;
                    }
                    last_pointer =
                        std::format("press {} \u00d7{} at {:.1f}, {:.1f}",
                                    button_name(event.button), event.click_count, event.x, event.y);
                    break;
                case Kind::scroll:
                    last_pointer =
                        std::format("scroll {:.2f}, {:.2f} px   wheel {:.2f}, {:.2f}{}{}",
                                    event.scroll_x, event.scroll_y, event.wheel_x, event.wheel_y,
                                    event.scroll_finger ? "   (touchpad)" : "",
                                    event.scroll_stop ? "   stop" : "");
                    break;
                case Kind::leave:
                    last_pointer = "pointer left the window";
                    break;
                default:
                    return;  // motion and enter only matter for hover
            }
            log::debug("pointer: {}", last_pointer);
            window.request_redraw();
        });

        // Start timing: the first frame, then the first frame drawn with the folder loaded
        // (listed, its thumbnails made), each when the compositor shows it.
        bool first_drawn = false;
        bool first_shown = false;
        bool ready_drawn = false;
        bool ready_shown = false;
        if (startup.active()) {
            window.set_frame_shown_handler([&] {
                if (first_drawn && !first_shown) {
                    first_shown = true;
                    startup.mark("first-frame-shown");
                }
                if (ready_drawn && !ready_shown) {
                    ready_shown = true;
                    startup.mark("folder-shown");
                    if (startup.quit_when_shown()) {
                        loop.quit(0);
                    }
                }
            });
        }
        window.set_draw_handler([&](ui::Renderer& renderer, const ui::Rect& bounds) {
            const bool ready = !columns || !columns->loading();
            seat.set_cursor_scale(window.scale());  // the xcursor fallback's images follow it
            const theme::Palette& palette = theme.palette();
            ui::Rect content = bounds;
            if (window.client_decorations()) {
                title_bar.set_state(window.state());
                title_bar.draw(renderer, bounds, palette, icons, window.title());
                content = title_bar.content(bounds);
            }
            if (startup.active()) {
                if (!first_drawn) {
                    first_drawn = true;
                    startup.mark("first-draw");
                }
                ready_drawn = ready_drawn || ready;
            }
            if (columns) {
                columns->draw(renderer, content, palette, icons, views::Browser::Clock::now());
                if (columns->animating()) {
                    window.request_redraw();  // next frame, paced by the compositor
                }
                note_state();  // every change shows in a frame, so frames are where to look
                return;
            }
            nordend::app::draw_test_scene(renderer, content, palette);
            renderer.draw_box(target,
                              {.fill = hovered ? palette.primary : palette.surface_container_high,
                               .radius = ui::Corners::all(10)});
            renderer.draw_text(std::format("Clicks: {}", clicks), target.x + 16.0F,
                               target.y + 14.0F, 15.0F,
                               hovered ? palette.on_primary : palette.on_surface);
            renderer.draw_text(last_pointer, 32.0F, bounds.bottom() - 56.0F, 14.0F,
                               palette.secondary);
            renderer.draw_text(last_key, 32.0F, bounds.bottom() - 32.0F, 14.0F, palette.tertiary);
        });
        if (!config_path.empty()) {
            config_watcher = std::make_unique<core::FileWatcher>(loop, config_path, [&] {
                const core::ConfigResult fresh = core::load_config(config_path);
                for (const std::string& problem : fresh.problems) {
                    log::warn("config: {}", problem);
                }
                if (!fresh.from_file && !fresh.problems.empty()) {
                    log::warn("config: keeping the settings in use");
                    return;
                }
                log::info("config: reloaded {}", config_path.string());
                apply_config(fresh.config);
            });
        }
        const int code = loop.run();
        keeper.flush();
        return code;
    } catch (const std::exception& e) {
        log::error("{}", e.what());
        return 1;
    }
}
