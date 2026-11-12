#include "nordend/views/browser.hpp"

#include <algorithm>
#include <array>
#include <chrono>
#include <cmath>
#include <cstdlib>
#include <format>
#include <string>
#include <sys/stat.h>
#include <sys/statvfs.h>
#include <system_error>
#include <unordered_set>
#include <utility>
#include <xkbcommon/xkbcommon-keysyms.h>

#include "draw_helpers.hpp"
#include "nordend/core/file.hpp"
#include "nordend/core/memory.hpp"
#include "nordend/fs/erase.hpp"
#include "nordend/fs/icon.hpp"
#include "nordend/fs/operations.hpp"
#include "nordend/fs/places.hpp"
#include "nordend/fs/trash.hpp"
#include "nordend/views/format.hpp"

namespace nordend::views {
namespace {

std::filesystem::path home_directory() {
    const char* const home = std::getenv("HOME");  // NOLINT(concurrency-mt-unsafe): at start
    return home != nullptr && *home == '/' ? std::filesystem::path{home} : std::filesystem::path{};
}

}  // namespace

Browser::Browser(core::EventLoop& loop, core::WorkerPool& pool, fs::DirectoryWatcher& watcher,
                 std::shared_ptr<const fs::MimeDatabase> mime, std::function<void()> redraw,
                 std::function<void(ui::CursorShape)> set_cursor,
                 std::shared_ptr<const thumbs::Cache> thumbnails)
    : loop_{loop},
      pool_{pool},
      redraw_{redraw},
      set_cursor_{std::move(set_cursor)},
      thumbnails_{thumbnails ? std::make_unique<Thumbnails>(pool, thumbnails, redraw) : nullptr},
      info_pane_{pool, thumbnails_.get(), redraw},
      watcher_{watcher},
      mime_{mime},
      quick_look_{pool, mime, std::move(thumbnails), redraw},
      sidebar_{redraw, resize_cursor(), [this](const fs::Place& place) { open_place(place); }},
      breadcrumb_{redraw,
                  [this](const std::filesystem::path& folder, const std::string& child) {
                      go_to(folder, child);
                  },
                  home_directory()},
      tab_bar_{redraw,
               {.select = [this](std::size_t index) { select_tab(index); },
                .close = [this](std::size_t index) { close_tab(index); },
                .add = [this] { new_tab(); }}},
      status_bar_{loop, pool, redraw,
                  [this](std::size_t view) { set_mode(static_cast<Mode>(view)); },
                  [this] { shortcuts_.open(); }},
      shortcuts_{redraw},
      editor_{redraw,
              [this](const std::shared_ptr<Folder>& folder, const std::string& from,
                     const std::string& to) { rename(folder, from, to); },
              [this](std::string problem) { show_message(std::move(problem), true); }},
      conflict_{redraw},
      confirm_{redraw},
      password_{redraw},
      menu_{std::move(redraw)},
      palette_{redraw_},
      trash_model_{loop, pool, watcher, std::move(mime)},
      transfers_{loop,
                 [this](const fs::TransferRequest& request, const fs::TransferProgress& progress) {
                     transfer_progress(request, progress);
                 },
                 [this](const fs::Conflict& conflict, std::function<void(fs::Resolution)> answer) {
                     ask(conflict, std::move(answer));
                 },
                 [this](const fs::TransferRequest& request, const fs::TransferResult& result) {
                     transfer_done(request, result);
                 }} {
    add_pane(0);
    palette_.set_chosen_handler([this](const std::string& id) {
        std::erase(palette_recent_, id);
        palette_recent_.insert(palette_recent_.begin(), id);
        if (palette_recent_.size() > State::kMaxRecent) {
            palette_recent_.resize(State::kMaxRecent);
        }
    });
    palette_.set_close_handler([this] {
        if (finder_) {
            finder_.reset();  // its paths go, and the memory they took back to the system
            core::give_back_free_memory();
        }
    });
    sidebar_.set_eject_handler([this](const fs::Place& place) { eject_place(place); });
    status_bar_.set_cancel_handler([this] { cancel_transfer(); });
    sidebar_.set_menu_handler(
        [this](const fs::Place& place, float x, float y) { place_menu(place, x, y); });
    home_trash_ = fs::home_trash();
    trash_model_.set_change_handler([this](const fs::ModelChange& /*change*/) {
        const bool full = !trash_model_.listing().entries().empty();
        if (full != trash_full_) {
            trash_full_ = full;
            show_places();
        }
    });
    trash_model_.open(fs::trash_path());
}

Pane& Browser::add_pane(std::size_t at) {
    at = std::min(at, panes_.size());
    const auto it =
        panes_.insert(panes_.begin() + static_cast<std::ptrdiff_t>(at),
                      std::make_unique<Pane>(loop_, pool_, watcher_, mime_, redraw_,
                                             resize_cursor(), thumbnails_.get(), &info_pane_));
    Pane& added = **it;
    const auto wire = [this](auto& view) {
        view.set_open_handler([this] { open_selection(); });
        view.set_edit_slot(&editor_.slot());
        view.set_menu_handler([this](float x, float y) { open_menu(x, y, false); });
        view.set_drop_mark(&drop_mark_);
        view.set_cut_marks(&cut_marks_);
    };
    wire(added.columns);
    wire(added.grid);
    wire(added.details);
    // A zoom is the window's: every list, or every grid, takes the size.
    // A zoom is the tab's: its two lists take the size (the grid has its own).
    Pane* const zoomed = &added;
    const auto lists = [zoomed](std::size_t index) {
        zoomed->columns.set_list_size(index);
        zoomed->details.set_list_size(index);
    };
    added.columns.set_zoom_handler(lists);
    added.details.set_zoom_handler(lists);
    // A click on a title of the details sorts the whole window.
    added.details.set_sort_handler([this](const fs::SortOrder& order) {
        for (const std::unique_ptr<Pane>& each : panes_) {
            each->details.set_sort(order.key, order.descending);
            each->columns.set_order(each->details.order());
            each->grid.set_order(each->details.order());
        }
    });
    if (panes_.size() > 1) {
        // The window's order and column widths; the zoom of the tab it comes from (the one shown,
        // which the insertion after it did not move).
        const Pane& from = *panes_[at <= active_ ? active_ + 1 : active_];
        added.details.set_sort(from.details.order().key, from.details.order().descending);
        added.columns.set_order(added.details.order());
        added.grid.set_order(added.details.order());
        added.details.set_widths(from.details.widths());
        added.columns.set_list_size(from.details.list_size());
        added.details.set_list_size(from.details.list_size());
        added.grid.set_size_index(from.grid.size_index());
    }
    added.columns.set_show_hidden(show_hidden_);
    added.grid.set_show_hidden(show_hidden_);
    added.details.set_show_hidden(show_hidden_);
    added.grid.set_info_visible(info_visible_);
    added.details.set_info_visible(info_visible_);
    return added;
}

void Browser::leave_pane() {
    quick_look_.close();
    if (menu_.is_open()) {
        menu_.close();
    }
    if (editor_.active()) {
        editor_.confirm();  // as a click elsewhere: the name typed is kept
    }
    press_.reset();
    band_press_.reset();
    end_band();
    if (pointer_target_ == Target::view) {
        view_pointer({.kind = ui::PointerEvent::Kind::leave}, Clock::now());
    }
}

void Browser::enter_pane() {
    if (pointer_inside_ && !button_held_) {
        dispatch_pointer({.kind = ui::PointerEvent::Kind::motion, .x = pointer_x_, .y = pointer_y_},
                         Clock::now());
    }
    redraw_();
}

void Browser::new_tab() {
    const std::shared_ptr<Folder> shown = current_folder();
    open_tab(shown ? shown->path() : home_directory(), true);
}

void Browser::open_tab(const std::filesystem::path& directory, bool show) {
    if (!show) {
        open_tab_behind(directory);
        return;
    }
    note_location();  // where the tab shown was, for its history
    const Mode mode = pane().mode;
    const std::size_t from = active_;
    leave_pane();
    add_pane(from + 1);
    active_ = from + 1;
    pane().mode = mode;
    open(directory);  // into the new pane: its first step
    enter_pane();
}

void Browser::open_tab_behind(const std::filesystem::path& directory) {
    note_location();
    const Mode mode = pane().mode;
    const std::size_t from = active_;
    add_pane(from + 1);
    active_ = from + 1;
    pane().mode = mode;
    open(directory);
    active_ = from;
    show_message(std::format("Opened “{}” in a new tab", directory.filename().string()), false);
    redraw_();
}

void Browser::open_tabs(const std::vector<std::filesystem::path>& folders) {
    const std::size_t from = active_;
    for (std::size_t i = 0; i < folders.size(); ++i) {
        active_ = from + i;  // the next one goes after the last opened
        open_tab_behind(folders[i]);
    }
    active_ = from;
    if (folders.size() > 1) {
        show_message(std::format("Opened {} folders in new tabs", folders.size()), false);
    }
}

void Browser::close_tab(std::size_t index) {
    if (panes_.size() <= 1 || index >= panes_.size()) {
        return;  // the last tab stays: Ctrl+W does nothing then
    }
    const bool shown = index == active_;
    if (shown) {
        leave_pane();
    }
    panes_.erase(panes_.begin() + static_cast<std::ptrdiff_t>(index));
    if (active_ > index || active_ >= panes_.size()) {
        --active_;  // the one after it takes its place, or the one before when it was the last
    }
    if (shown) {
        enter_pane();
    } else {
        redraw_();
    }
}

void Browser::select_tab(std::size_t index) {
    if (index >= panes_.size() || index == active_) {
        return;
    }
    note_location();
    leave_pane();
    active_ = index;
    enter_pane();
}

std::shared_ptr<Folder> Browser::folder_of(const Pane& pane) {
    switch (pane.mode) {
        case Mode::grid:
            return pane.grid.folder();
        case Mode::details:
            return pane.details.folder();
        case Mode::columns:
            break;
    }
    return pane.columns.focused_folder();
}

std::vector<std::string> Browser::tab_titles() const {
    const std::filesystem::path home = home_directory();
    std::vector<std::string> titles;
    titles.reserve(panes_.size());
    for (const std::unique_ptr<Pane>& each : panes_) {
        const std::shared_ptr<Folder> folder = folder_of(*each);
        if (!folder) {
            titles.emplace_back();
        } else if (folder->is_trash()) {
            titles.emplace_back("Trash");
        } else if (folder->path() == home) {
            titles.emplace_back("Home");
        } else if (!folder->path().has_relative_path()) {
            titles.push_back(folder->path().string());  // the root
        } else {
            titles.push_back(folder->path().filename().string());
        }
    }
    return titles;
}

std::vector<std::string> Browser::memory_lines() const {
    const auto mb = [](std::size_t bytes) {
        return std::format("{:.1f} MB", static_cast<double>(bytes) / 1e6);
    };
    std::vector<std::string> lines;
    if (finder_) {
        lines.push_back(
            std::format("finding files: {} paths, {}", finder_->entries(), mb(finder_->bytes())));
    }
    if (thumbnails_) {
        const Thumbnails::Usage usage = thumbnails_->usage();
        lines.push_back(
            std::format("thumbnails: {} slots, {} textures {} (budget {}), pixels "
                        "waiting {}",
                        usage.slots, usage.textures, mb(usage.texture_bytes),
                        mb(Thumbnails::kBudgetBytes), mb(usage.pixel_bytes)));
    }
    const QuickLook::Usage look = quick_look_.usage();
    lines.push_back(std::format("quick look: {} slots, textures {}, pixels kept {}", look.slots,
                                mb(look.texture_bytes), mb(look.pixel_bytes)));
    lines.push_back(std::format("glyph atlases: {}", mb(ui::Renderer::atlas_bytes())));
    std::size_t folders = 0;
    std::size_t entries = 0;
    for (std::size_t i = 0; i < pane().columns.column_count(); ++i) {
        ++folders;
        entries += pane().columns.column(i).folder().entries().size();
    }
    lines.push_back(std::format("columns: {} folders, {} entries", folders, entries));
    if (const std::shared_ptr<Folder> shown = current_folder();
        shown && pane().mode != Mode::columns) {
        lines.push_back(std::format("shown folder: {} entries", shown->entries().size()));
    }
    return lines;
}

std::vector<std::string> Browser::targets(const Folder& folder) {
    std::vector<std::string> names = folder.selected_names();
    if (names.empty()) {
        if (const fs::Entry* const cursor = folder.cursor_entry()) {
            names.push_back(cursor->name);
        }
    }
    return names;
}

void Browser::take_selection(bool cut) {
    const std::shared_ptr<Folder> folder = current_folder();
    if (!folder) {
        return;
    }
    std::vector<std::string> names = targets(*folder);
    if (names.empty()) {
        return;
    }
    const std::size_t count = names.size();
    clipboard_ = {.directory = folder->path(), .names = std::move(names), .cut = cut};
    cut_marks_ = {};
    if (cut) {
        cut_marks_ = {.directory = clipboard_.directory,
                      .names = {clipboard_.names.begin(), clipboard_.names.end()}};
    }
    if (system_.offer) {
        // Other programs paste them too: file managers, browsers, terminals.
        std::vector<std::filesystem::path> paths;
        paths.reserve(clipboard_.names.size());
        for (const std::string& name : clipboard_.names) {
            paths.push_back(clipboard_.directory / name);
        }
        const std::string lines = path_lines(paths);
        std::vector<SystemClipboard::Format> formats{
            {std::string{kGnomeFiles}, gnome_copied_files(cut, paths)},
            {std::string{kUriList}, uri_list(paths)},
            {std::string{kText}, lines},
            {"UTF8_STRING", lines},
            {"text/plain", lines}};
        if (cut) {
            formats.emplace_back(std::string{kKdeCut}, "1");
        }
        system_.offer(std::move(formats));
    }
    const std::string items =
        count == 1 ? std::format("“{}”", clipboard_.names.front()) : std::format("{} items", count);
    show_message(cut ? std::format("Ctrl+V moves {} where you want them", items)
                     : std::format("Ctrl+V copies {} where you want them", items),
                 false);
}

void Browser::copy_text(std::string text) {
    // The clipboard holds text now, not files: Ctrl+V has nothing to paste here.
    clipboard_ = {};
    cut_marks_ = {};
    const auto lines = static_cast<std::size_t>(std::ranges::count(text, '\n')) + 1;
    const std::string what = lines == 1
                                 ? std::format("{} characters", column_at_byte(text, text.size()))
                                 : std::format("{} lines", lines);
    if (system_.offer) {
        system_.offer({{std::string{kText}, text}, {"UTF8_STRING", text}, {"text/plain", text}});
    }
    show_message(std::format("Copied {}", what), false);
}

void Browser::paste() {
    const std::shared_ptr<Folder> folder = current_folder();
    if (!folder) {
        return;
    }
    if (system_.owned && system_.types && system_.read && !system_.owned()) {
        // Another program's: the files it offers, if any.
        const std::vector<std::string> types = system_.types();
        const auto has = [&types](std::string_view type) {
            return std::ranges::find(types, type) != types.end();
        };
        const bool gnome = has(kGnomeFiles);
        if (gnome || has(kUriList)) {
            const std::uint64_t generation = system_.generation ? system_.generation() : 0;
            const auto read_files = [this, gnome, generation,
                                     destination = folder->path()](bool kde_cut) {
                ++operations_;
                system_.read(std::string{gnome ? kGnomeFiles : kUriList},
                             [this, alive = alive_, gnome, kde_cut, generation,
                              destination](const std::optional<std::string>& data) {
                                 if (!*alive) {
                                     return;
                                 }
                                 --operations_;
                                 std::optional<ClipboardFiles> files;
                                 if (data) {
                                     files = gnome ? parse_gnome_copied_files(*data)
                                                   : parse_uri_list(*data);
                                 }
                                 if (!files) {
                                     show_message("The clipboard could not be read", true);
                                     return;
                                 }
                                 files->cut = files->cut || kde_cut;
                                 paste_files(destination, *files, generation);
                             });
            };
            if (!gnome && has(kKdeCut)) {
                // KDE says "cut" in a type of its own.
                ++operations_;
                system_.read(std::string{kKdeCut}, [this, alive = alive_, read_files](
                                                       const std::optional<std::string>& data) {
                    if (!*alive) {
                        return;
                    }
                    --operations_;
                    read_files(data && data->starts_with('1'));
                });
            } else {
                read_files(false);
            }
            return;
        }
        if (!types.empty()) {
            show_message("The clipboard holds no files", false);
            return;
        }
    }
    if (clipboard_.names.empty()) {
        show_message("Nothing to paste: Ctrl+C or Ctrl+X first", false);
        return;
    }
    const bool cut = clipboard_.cut;
    submit({.kind = cut ? fs::TransferKind::move : fs::TransferKind::copy,
            .source = clipboard_.directory,
            .names = clipboard_.names,
            .destination = folder->path(),
            .force_copy = false},
           open_batch(Purpose::user, cut ? FileOperation::Kind::move : FileOperation::Kind::copy));
    if (cut) {
        clipboard_ = {};  // moved once: there is nothing left to move
        cut_marks_ = {};
        if (system_.clear) {
            system_.clear();
        }
    }
    if (!status_bar_.transfer()) {
        status_bar_.set_transfer(TransferStatus{
            .text = cut ? "Getting ready to move…" : "Getting ready to copy…", .fraction = -1.0F});
    }
    redraw_();
}

std::uint64_t Browser::open_batch(Purpose purpose, FileOperation::Kind kind) {
    const std::uint64_t id = ++batch_ids_;
    batches_.emplace(
        id, Batch{.purpose = purpose, .kind = kind, .pending = 0, .items = {}, .failed = false});
    return id;
}

void Browser::submit(fs::TransferRequest request, std::uint64_t batch,
                     std::optional<std::uint64_t> empty_clipboard) {
    if (const auto it = batches_.find(batch); it != batches_.end()) {
        ++it->second.pending;
    }
    submitted_.push_back({.batch = batch, .empty_clipboard = empty_clipboard});
    transfers_.submit(std::move(request));
}

void Browser::paste_files(const std::filesystem::path& destination, const ClipboardFiles& files,
                          std::uint64_t generation) {
    if (fs::is_trash(destination)) {
        return;
    }
    if (files.paths.empty()) {
        show_message(files.refused > 0 ? "Only files on this computer can be pasted"
                                       : "The clipboard holds no files",
                     false);
        return;
    }
    transfer_paths(destination, files.paths, files.cut,
                   files.cut ? std::optional{generation} : std::nullopt);
}

void Browser::transfer_paths(const std::filesystem::path& destination,
                             const std::vector<std::filesystem::path>& paths, bool move,
                             std::optional<std::uint64_t> empty_clipboard) {
    // One transfer per folder the files are in, one operation for Ctrl+Z.
    const std::uint64_t batch =
        open_batch(Purpose::user, move ? FileOperation::Kind::move : FileOperation::Kind::copy);
    std::vector<std::pair<std::filesystem::path, std::vector<std::string>>> folders;
    for (const std::filesystem::path& path : paths) {
        const std::filesystem::path parent = path.parent_path();
        auto it = std::ranges::find(folders, parent, [](const auto& f) { return f.first; });
        if (it == folders.end()) {
            folders.emplace_back(parent, std::vector<std::string>{});
            it = std::prev(folders.end());
        }
        it->second.push_back(path.filename().string());
    }
    for (auto& [source, names] : folders) {
        submit({.kind = move ? fs::TransferKind::move : fs::TransferKind::copy,
                .source = source,
                .names = std::move(names),
                .destination = destination,
                .force_copy = false},
               batch, empty_clipboard);
    }
    if (!status_bar_.transfer()) {
        status_bar_.set_transfer(TransferStatus{
            .text = move ? "Getting ready to move…" : "Getting ready to copy…", .fraction = -1.0F});
    }
    redraw_();
}

void Browser::paste_text() {
    if (!system_.types || !system_.read) {
        return;
    }
    const std::vector<std::string> types = system_.types();
    for (const std::string_view type : kTextTypes) {
        if (std::ranges::find(types, type) == types.end()) {
            continue;
        }
        ++operations_;
        system_.read(std::string{type},
                     [this, alive = alive_](const std::optional<std::string>& text) {
                         if (!*alive) {
                             return;
                         }
                         --operations_;
                         if (!text) {
                             return;
                         }
                         if (password_.is_open()) {
                             password_.paste(*text);
                         } else if (palette_.is_open()) {
                             palette_.paste(*text);
                         } else {
                             editor_.paste(*text);
                         }
                     });
        return;
    }
}

void Browser::clipboard_lost() {
    clipboard_ = {};
    if (!cut_marks_.names.empty()) {
        cut_marks_ = {};
        redraw_();
    }
}

namespace {

// The names going and the darktable sidecars that go with them.
std::unordered_set<std::string> going_with_sidecars(const Folder& folder,
                                                    const std::vector<std::string>& names) {
    std::vector<std::string> listing;
    listing.reserve(folder.entries().size());
    for (const fs::Entry& entry : folder.entries()) {
        listing.push_back(entry.name);
    }
    std::unordered_set<std::string> going{names.begin(), names.end()};
    for (const std::string& name : names) {
        for (std::string& sidecar : fs::sidecars_of(listing, name)) {
            going.insert(std::move(sidecar));
        }
    }
    return going;
}

}  // namespace

void Browser::cursor_after(Folder& folder, const std::unordered_set<std::string>& going) {
    const std::vector<fs::Entry>& entries = folder.entries();
    std::size_t last = 0;
    for (std::size_t i = 0; i < entries.size(); ++i) {
        if (going.contains(entries[i].name)) {
            last = i;
        }
    }
    std::string next;
    for (std::size_t i = last + 1; i < entries.size() && next.empty(); ++i) {
        if (!going.contains(entries[i].name)) {
            next = entries[i].name;
        }
    }
    for (std::size_t i = last; i-- > 0 && next.empty();) {
        if (!going.contains(entries[i].name)) {
            next = entries[i].name;
        }
    }
    if (next.empty()) {
        folder.set_cursor(std::nullopt);
    } else {
        folder.select_name(std::move(next));
    }
}

void Browser::trash() {
    const std::shared_ptr<Folder> folder = current_folder();
    if (!folder) {
        return;
    }
    std::vector<std::string> names = targets(*folder);
    if (names.empty()) {
        return;
    }
    // The cursor goes on to the first entry after the last one trashed that stays (else the one
    // before them): their darktable sidecars go too.
    const std::unordered_set<std::string> going = going_with_sidecars(*folder, names);
    submit({.kind = fs::TransferKind::trash,
            .source = folder->path(),
            .names = std::move(names),
            .destination = home_trash_,
            .force_copy = false},
           open_batch(Purpose::user, FileOperation::Kind::trash));
    cursor_after(*folder, going);
    redraw_();
}

void Browser::restore() {
    const std::shared_ptr<Folder> folder = current_folder();
    if (!folder || !folder->is_trash()) {
        return;
    }
    const std::vector<std::string> names = targets(*folder);
    if (names.empty()) {
        return;
    }
    // The engine is given where the items are; it finds their sidecars itself.
    std::vector<std::string> paths;
    for (const fs::Entry& entry : folder->entries()) {
        if (std::ranges::find(names, entry.name) != names.end()) {
            paths.push_back(folder->path_of(entry).string());
        }
    }
    submit({.kind = fs::TransferKind::restore,
            .source = folder->path(),
            .names = std::move(paths),
            .destination = {},
            .force_copy = false},
           open_batch(Purpose::user, FileOperation::Kind::restore));
    cursor_after(*folder, going_with_sidecars(*folder, names));
    redraw_();
}

namespace {

// What the card says will go: "With its darktable sidecar · 21.3 MB", "12 items · 412 MB · 1203
// files and folders in all".
std::string erase_detail(const fs::ErasePlan& plan, bool emptying) {
    std::vector<std::string> parts;
    const std::size_t items = plan.names.size();
    if (emptying) {
        parts.push_back(std::format("{} {}", items, items == 1 ? "item" : "items"));
    } else if (plan.sidecars == 1) {
        parts.emplace_back(items == 1 ? "With its darktable sidecar" : "With 1 darktable sidecar");
    } else if (plan.sidecars > 1) {
        parts.push_back(std::format("With {} darktable sidecars", plan.sidecars));
    }
    parts.push_back(format_size(plan.bytes));
    if (plan.entries > plan.items.size()) {
        parts.push_back(std::format("{} files and folders in all", plan.entries));
    }
    std::string text;
    for (const std::string& part : parts) {
        text += text.empty() ? part : " · " + part;
    }
    return text;
}

}  // namespace

void Browser::erase() {
    const std::shared_ptr<Folder> folder = current_folder();
    if (!folder) {
        return;
    }
    std::vector<std::string> names = targets(*folder);
    if (names.empty()) {
        return;
    }
    std::string title = names.size() == 1
                            ? std::format("Delete “{}” permanently?", names.front())
                            : std::format("Delete {} items permanently?", names.size());
    fs::TransferRequest request{.kind = fs::TransferKind::erase,
                                .source = folder->path(),
                                .names = {},
                                .destination = {},
                                .force_copy = false};
    if (folder->is_trash()) {
        // Where the items are; the engine finds their sidecars itself.
        for (const fs::Entry& entry : folder->entries()) {
            if (std::ranges::find(names, entry.name) != names.end()) {
                request.names.push_back(folder->path_of(entry).string());
            }
        }
    } else {
        request.names = names;
    }
    ask_erase(
        std::move(request), std::move(title),
        [folder, going = going_with_sidecars(*folder, names)] { cursor_after(*folder, going); });
}

void Browser::empty_trash() {
    ask_erase({.kind = fs::TransferKind::empty_trash,
               .source = fs::trash_path(),
               .names = {},
               .destination = {},
               .force_copy = false},
              "Empty the trash?", nullptr);
}

void Browser::ask_erase(fs::TransferRequest request, std::string title,
                        std::function<void()> after) {
    const std::uint64_t id = ++erase_ids_;
    const bool emptying = request.kind == fs::TransferKind::empty_trash;
    erase_ = PendingErase{.id = id, .request = request, .planned = false, .confirmed = false};
    confirm_.open(std::move(title), "Adding up the size…", emptying ? "Empty" : "Delete",
                  [this, id, after = std::move(after)](bool confirmed) {
                      if (!erase_ || erase_->id != id) {
                          return;
                      }
                      if (!confirmed) {
                          erase_.reset();  // Escape: nothing goes
                          return;
                      }
                      erase_->confirmed = true;
                      if (after) {
                          after();
                      }
                      if (erase_->planned) {
                          submit_erase();
                      }
                  });
    // The trashes are found on the worker: a disk that does not answer does not stop the loop.
    std::optional<fs::TrashSources> sources;
    if (emptying) {
        sources = fs::trash_sources();
    }
    ++operations_;
    pool_.submit(
        core::Priority::visible,
        [request = std::move(request), sources](const core::CancelToken& token) mutable {
            if (sources) {
                for (const fs::TrashDir& trash : fs::existing_trashes(*sources)) {
                    request.names.push_back(trash.path.string());
                }
            }
            fs::ErasePlan plan = fs::plan_erase(request, token);
            return std::pair{std::move(request), std::move(plan)};
        },
        [this, alive = alive_, id,
         emptying](const std::pair<fs::TransferRequest, fs::ErasePlan>& planned) {
            if (!*alive) {
                return;
            }
            --operations_;
            if (!erase_ || erase_->id != id) {
                return;  // given up, or another one asked since
            }
            const auto& [complete, plan] = planned;
            if (plan.names.empty()) {
                confirm_.close();
                erase_.reset();
                if (emptying) {
                    show_message("The trash is empty already", false);
                } else {
                    show_message(plan.errors.empty() ? std::string{"Nothing to delete"}
                                                     : plan.errors.front(),
                                 !plan.errors.empty());
                }
                return;
            }
            erase_->request = complete;
            erase_->planned = true;
            confirm_.set_detail(erase_detail(plan, emptying));
            if (erase_->confirmed) {
                submit_erase();
            }
        });
}

void Browser::submit_erase() {
    if (!erase_) {
        return;
    }
    const bool emptying = erase_->request.kind == fs::TransferKind::empty_trash;
    submit(std::move(erase_->request), 0);
    erase_.reset();
    undo_ = FileOperation{.kind = FileOperation::Kind::erase, .items = {}};  // no way back
    redo_.reset();
    if (!status_bar_.transfer()) {
        status_bar_.set_transfer(TransferStatus{
            .text = emptying ? "Getting ready to empty the trash…" : "Getting ready to delete…",
            .fraction = -1.0F});
    }
    redraw_();
}

void Browser::place_menu(const fs::Place& place, float x, float y) {
    if (place.section == fs::Place::Section::devices && !place.disk.volume.empty() &&
        devices_.mount) {
        ++menu_generation_;
        menu_loaders_.clear();
        std::vector<MenuItem> items;
        MenuItem mount;
        mount.id = "mount";
        mount.label = "Mount";
        mount.enabled = !place.disk.mounted;
        mount.action = [this, place] { mount_place(place, false); };
        items.push_back(std::move(mount));
        MenuItem unmount;
        unmount.id = "unmount";
        unmount.label = "Unmount";
        unmount.enabled = place.disk.mounted;
        unmount.action = [this, place] { unmount_place(place); };
        items.push_back(std::move(unmount));
        if (place.disk.ejectable) {
            MenuItem eject;
            eject.id = "eject";
            eject.label = "Eject";
            eject.separator = true;
            eject.action = [this, place] { eject_place(place); };
            items.push_back(std::move(eject));
        }
        menu_.open(x, y, std::move(items), false);
        return;
    }
    if (place.section == fs::Place::Section::bookmarks) {
        ++menu_generation_;
        menu_loaders_.clear();
        std::vector<MenuItem> items;
        MenuItem remove;
        remove.id = "remove_bookmark";
        remove.label = "Remove from Sidebar";
        if (!place.uri.empty()) {
            MenuItem mount;
            mount.id = "mount";
            mount.label = "Mount";
            mount.enabled = !place.disk.mounted && remotes_.mount != nullptr;
            mount.action = [this, place] { mount_remote(place, false); };
            items.push_back(std::move(mount));
            MenuItem unmount;
            unmount.id = "unmount";
            unmount.label = "Unmount";
            unmount.enabled = place.disk.mounted && remotes_.unmount != nullptr;
            unmount.action = [this, place] { unmount_remote(place); };
            items.push_back(std::move(unmount));
            remove.separator = true;
            remove.action = [this, uri = place.uri, label = place.label] {
                remove_remote_from_sidebar(uri, label);
            };
        } else {
            remove.action = [this, path = place.path] { remove_from_sidebar(path); };
        }
        items.push_back(std::move(remove));
        menu_.open(x, y, std::move(items), false);
        return;
    }
    if (!fs::is_trash(place.path)) {
        return;  // the others have none
    }
    ++menu_generation_;
    menu_loaders_.clear();
    MenuItem empty;
    empty.id = "empty_trash";
    empty.label = "Empty Trash…";
    empty.enabled = trash_full_;
    empty.action = [this] { empty_trash(); };
    std::vector<MenuItem> items;
    items.push_back(std::move(empty));
    menu_.open(x, y, std::move(items), false);
}

void Browser::set_places(std::vector<fs::Place> places) {
    places_ = std::move(places);
    show_places();
}

void Browser::show_places() {
    std::vector<fs::Place> places = places_;
    for (fs::Place& place : places) {
        if (fs::is_trash(place.path)) {
            place.icon = trash_full_ ? "trash-filled" : "trash";
        }
    }
    sidebar_.set_places(std::move(places));
}

void Browser::cancel_transfer() {
    transfers_.cancel();
    conflict_.close();
}

void Browser::ask(const fs::Conflict& conflict, std::function<void(fs::Resolution)> answer) {
    if (for_the_rest_ && (*for_the_rest_ != fs::Resolution::replace ||
                          (conflict.directory == conflict.target_directory &&
                           (!conflict.directory || conflict.merge)))) {
        answer(*for_the_rest_);
        return;
    }
    copy_started_.reset();  // the time spent on the question does not count in the rate
    conflict_.open(conflict, conflict.destination.filename().string(),
                   [this, answer = std::move(answer)](fs::Resolution resolution, bool rest) {
                       if (rest) {
                           for_the_rest_ = resolution;
                       }
                       answer(resolution);
                   });
}

namespace {

// "8 s", "2 min", "1 h 5 min".
std::string format_duration(double seconds) {
    const long whole = std::lround(seconds);
    if (whole < 60) {
        return std::format("{} s", std::max(whole, 1L));
    }
    if (whole < 3600) {
        return std::format("{} min", (whole + 30) / 60);
    }
    return std::format("{} h {} min", whole / 3600, (whole % 3600) / 60);
}

}  // namespace

void Browser::transfer_progress(const fs::TransferRequest& request,
                                const fs::TransferProgress& progress) {
    const bool copy = request.kind == fs::TransferKind::copy;
    const std::size_t items = request.names.size();
    TransferStatus status;
    if (request.kind == fs::TransferKind::restore) {
        status.fraction = progress.files_total > 0 ? static_cast<float>(progress.files_done) /
                                                         static_cast<float>(progress.files_total)
                                                   : -1.0F;
        status.text =
            std::format("Restoring {} of {} items", progress.files_done, progress.files_total);
    } else if (request.kind == fs::TransferKind::trash) {
        status.fraction = progress.files_total > 0 ? static_cast<float>(progress.files_done) /
                                                         static_cast<float>(progress.files_total)
                                                   : -1.0F;
        status.text = std::format("Moving {} of {} items to the trash", progress.files_done,
                                  progress.files_total);
    } else if (request.kind == fs::TransferKind::erase ||
               request.kind == fs::TransferKind::empty_trash) {
        const bool emptying = request.kind == fs::TransferKind::empty_trash;
        if (progress.preparing) {
            status.text =
                emptying ? "Getting ready to empty the trash…" : "Getting ready to delete…";
        } else {
            status.fraction = progress.files_total > 0
                                  ? static_cast<float>(progress.files_done) /
                                        static_cast<float>(progress.files_total)
                                  : -1.0F;
            status.text = std::format("{} {} of {} files and folders",
                                      emptying ? "Emptying the trash:" : "Deleting",
                                      progress.files_done, progress.files_total);
        }
    } else if (progress.preparing) {
        status.text = std::format("Getting ready to {} {} {}…", copy ? "copy" : "move", items,
                                  items == 1 ? "item" : "items");
    } else if (progress.bytes_total > 0) {
        const auto now = std::chrono::steady_clock::now();
        if (!copy_started_) {
            copy_started_ = RateStart{.time = now, .bytes = progress.bytes_done};
        }
        status.fraction = static_cast<float>(static_cast<double>(progress.bytes_done) /
                                             static_cast<double>(progress.bytes_total));
        status.text =
            std::format("{} {} {} · {} of {}", copy ? "Copying" : "Moving", progress.files_total,
                        progress.files_total == 1 ? "file" : "files",
                        format_size(progress.bytes_done), format_size(progress.bytes_total));
        const double seconds = std::chrono::duration<double>(now - copy_started_->time).count();
        if (seconds >= 1.0 && progress.bytes_done > copy_started_->bytes) {
            const double rate =
                static_cast<double>(progress.bytes_done - copy_started_->bytes) / seconds;
            status.text += std::format(
                " · {}/s · {} left", format_size(static_cast<std::uint64_t>(rate)),
                format_duration(static_cast<double>(progress.bytes_total - progress.bytes_done) /
                                rate));
        }
    } else {
        status.fraction = progress.files_total > 0 ? static_cast<float>(progress.files_done) /
                                                         static_cast<float>(progress.files_total)
                                                   : -1.0F;
        status.text =
            std::format("Moving {} of {} items", progress.files_done, progress.files_total);
    }
    if (transfers_.pending() > 1) {
        status.text += std::format(" · {} more waiting", transfers_.pending() - 1);
    }
    status_bar_.set_transfer(std::move(status));
}

void Browser::transfer_done(const fs::TransferRequest& request, const fs::TransferResult& result) {
    Submitted submitted;
    if (!submitted_.empty()) {
        submitted = submitted_.front();
        submitted_.pop_front();
    }
    // Another program's cut that moved something: nobody should move those files again. Unless
    // the clipboard holds something else by now.
    if (submitted.empty_clipboard && !result.placed.empty() && system_.clear &&
        system_.generation && system_.generation() == *submitted.empty_clipboard) {
        system_.clear();
    }
    report_done(request, result);
    const auto batch = batches_.find(submitted.batch);
    if (batch == batches_.end()) {
        return;
    }
    Batch& done = batch->second;
    done.items.insert(done.items.end(), result.items.begin(), result.items.end());
    done.failed =
        done.failed || result.cancelled || !result.errors.empty() || !result.refused.empty();
    if (--done.pending == 0) {
        Batch finished = std::move(done);
        batches_.erase(batch);
        record({.kind = finished.kind, .items = std::move(finished.items)}, finished.purpose,
               finished.failed);
    }
}

void Browser::report_done(const fs::TransferRequest& request, const fs::TransferResult& result) {
    for_the_rest_.reset();
    copy_started_.reset();
    conflict_.close();
    if (transfers_.pending() == 0) {
        status_bar_.set_transfer(std::nullopt);
    }
    if (request.kind == fs::TransferKind::trash) {
        trash_done(result);
        return;
    }
    if (request.kind == fs::TransferKind::restore) {
        restore_done(result);
        return;
    }
    if (request.kind == fs::TransferKind::erase || request.kind == fs::TransferKind::empty_trash) {
        erase_done(request, result);
        return;
    }
    const bool copy = request.kind == fs::TransferKind::copy;
    const std::string_view done = copy ? "Copied" : "Moved";
    const std::size_t files = result.files_done;
    const std::string what = std::format("{} {}", files, files == 1 ? "file" : "files");
    if (!result.refused.empty()) {
        show_message(result.refused, true);
    } else if (result.cancelled) {
        show_message(std::format("Stopped: {} {} so far", what, copy ? "copied" : "moved"), false);
    } else if (!result.errors.empty()) {
        show_message(std::format("{} {}; {} could not be: {}", done, what, result.errors.size(),
                                 result.errors.front()),
                     true);
    } else if (result.placed.empty()) {
        show_message("Nothing to do: every name was skipped", false);
    } else if (result.bytes_done > 0) {
        show_message(std::format("{} {} ({})", done, what, format_size(result.bytes_done)), false);
    } else {
        show_message(std::format("{} {}", done, what), false);
    }
    // What arrived is selected, when its folder is the one shown.
    if (const std::shared_ptr<Folder> folder = current_folder();
        folder && folder->path() == request.destination && !result.placed.empty()) {
        folder->expect_names(result.placed);
    }
}

void Browser::trash_done(const fs::TransferResult& result) {
    const std::size_t items = result.placed.size();
    std::string what =
        items == 1 ? std::format("“{}”", result.placed.front()) : std::format("{} items", items);
    if (const std::size_t sidecars = result.files_done - items; sidecars > 0) {
        what += std::format(" with {} darktable sidecar{}", sidecars, sidecars == 1 ? "" : "s");
    }
    if (!result.refused.empty()) {
        show_message(result.refused, true);
    } else if (result.cancelled) {
        show_message(std::format("Stopped: {} moved to the trash so far", what), false);
    } else if (!result.errors.empty()) {
        show_message(std::format("{} moved to the trash; {} could not be: {}", what,
                                 result.errors.size(), result.errors.front()),
                     true);
    } else if (items > 0) {
        show_message(std::format("{} moved to the trash", what), false);
    }
}

void Browser::restore_done(const fs::TransferResult& result) {
    const std::size_t items = result.placed.size();
    std::string what =
        items == 1 ? std::format("“{}”", result.placed.front()) : std::format("{} items", items);
    if (const std::size_t sidecars = result.files_done - items; sidecars > 0) {
        what += std::format(" with {} darktable sidecar{}", sidecars, sidecars == 1 ? "" : "s");
    }
    if (!result.refused.empty()) {
        show_message(result.refused, true);
    } else if (result.cancelled) {
        show_message(std::format("Stopped: {} restored so far", what), false);
    } else if (!result.errors.empty()) {
        show_message(std::format("Restored {}; {} could not be: {}", what, result.errors.size(),
                                 result.errors.front()),
                     true);
    } else if (items == 0) {
        show_message("Nothing restored: every name was skipped", false);
    } else {
        show_message(std::format("Restored {}", what), false);
    }
}

void Browser::erase_done(const fs::TransferRequest& request, const fs::TransferResult& result) {
    const std::size_t items = result.placed.size();
    const std::string what =
        items == 1 ? std::format("“{}”", result.placed.front()) : std::format("{} items", items);
    const std::string size =
        result.bytes_done > 0 ? std::format(" ({})", format_size(result.bytes_done)) : "";
    if (result.cancelled) {
        show_message(std::format("Stopped: {} deleted so far", what), false);
    } else if (!result.errors.empty()) {
        show_message(std::format("Deleted {}; {} could not be: {}", what, result.errors.size(),
                                 result.errors.front()),
                     true);
    } else if (request.kind == fs::TransferKind::empty_trash) {
        show_message(std::format("Emptied the trash: {} deleted{}", what, size), false);
    } else {
        show_message(std::format("Deleted {} permanently{}", what, size), false);
    }
}

namespace {

// A new folder or file as it was made: an empty folder, or a file as long as its template (or
// empty). Only then does Ctrl+Z take it away.
bool unchanged_since_made(const fs::PlacedItem& item) {
    std::error_code error;
    const std::filesystem::file_status status = std::filesystem::symlink_status(item.to, error);
    if (error) {
        return false;
    }
    if (std::filesystem::is_directory(status)) {
        return std::filesystem::is_empty(item.to, error) && !error;
    }
    if (!std::filesystem::is_regular_file(status)) {
        return false;
    }
    const std::uintmax_t size = std::filesystem::file_size(item.to, error);
    if (error) {
        return false;
    }
    if (item.from.empty()) {
        return size == 0;
    }
    const std::uintmax_t made_from = std::filesystem::file_size(item.from, error);
    return !error && size == made_from;
}

}  // namespace

void Browser::record(FileOperation operation, Purpose purpose, bool failed) {
    if (purpose != Purpose::user) {
        undoing_ = false;
    }
    if (operation.items.empty()) {
        return;  // nothing was done: what Ctrl+Z undoes stays
    }
    const std::string what = describe(operation);
    switch (purpose) {
        case Purpose::user:
            undo_ = std::move(operation);
            redo_.reset();
            return;
        case Purpose::undo:
            redo_ = std::move(operation);
            undo_.reset();
            break;
        case Purpose::redo:
            undo_ = std::move(operation);
            redo_.reset();
            break;
    }
    if (!failed) {  // otherwise the transfer's own message says what went wrong
        show_message(std::format("{}: {}", purpose == Purpose::undo ? "Undone" : "Redone", what),
                     false);
    }
}

void Browser::undo() {
    if (undoing_) {
        show_message("Still undoing the last one", false);
    } else if (!undo_) {
        show_message("Nothing to undo", false);
    } else {
        start_inverse(*undo_, Purpose::undo);
    }
}

void Browser::redo() {
    if (undoing_) {
        show_message("Still undoing the last one", false);
    } else if (!redo_) {
        show_message("Nothing to redo", false);
    } else {
        start_inverse(*redo_, Purpose::redo);
    }
}

void Browser::trash_paths(const std::vector<std::filesystem::path>& paths, Purpose purpose) {
    const std::uint64_t batch = open_batch(purpose, FileOperation::Kind::trash);
    std::vector<std::pair<std::filesystem::path, std::vector<std::string>>> folders;
    for (const std::filesystem::path& path : paths) {
        auto it = std::ranges::find(folders, path.parent_path(),
                                    [](const auto& folder) { return folder.first; });
        if (it == folders.end()) {
            folders.emplace_back(path.parent_path(), std::vector<std::string>{});
            it = std::prev(folders.end());
        }
        it->second.push_back(path.filename().string());
    }
    if (purpose != Purpose::user) {
        undoing_ = true;
    }
    for (auto& [source, names] : folders) {
        submit({.kind = fs::TransferKind::trash,
                .source = source,
                .names = std::move(names),
                .destination = home_trash_,
                .force_copy = false},
               batch);
    }
}

void Browser::start_inverse(const FileOperation& operation, Purpose purpose) {
    std::vector<std::filesystem::path> placed;
    placed.reserve(operation.items.size());
    for (const fs::PlacedItem& item : operation.items) {
        placed.push_back(item.to);
    }
    switch (operation.kind) {
        case FileOperation::Kind::erase:
            show_message("A permanent delete cannot be undone", false);
            return;
        case FileOperation::Kind::rename: {
            const fs::PlacedItem item = operation.items.front();
            undoing_ = true;
            ++operations_;
            pool_.submit(
                core::Priority::visible,
                [item](const core::CancelToken& /*token*/) {
                    return fs::rename_entry(item.to.parent_path(), item.to.filename().string(),
                                            item.from.filename().string());
                },
                [this, alive = alive_, item, purpose](const fs::Renamed& renamed) {
                    if (!*alive) {
                        return;
                    }
                    --operations_;
                    if (renamed.error) {
                        undoing_ = false;
                        show_message(renamed.problem, true);
                        return;
                    }
                    record({.kind = FileOperation::Kind::rename,
                            .items = {{.from = item.to, .to = item.from}}},
                           purpose, !renamed.problem.empty());
                    if (!renamed.problem.empty()) {
                        show_message(renamed.problem, true);
                    }
                });
            return;
        }
        case FileOperation::Kind::create: {
            const fs::PlacedItem item = operation.items.front();
            undoing_ = true;
            ++operations_;
            pool_.submit(
                core::Priority::visible,
                [item](const core::CancelToken& /*token*/) { return unchanged_since_made(item); },
                [this, alive = alive_, item, purpose](bool unchanged) {
                    if (!*alive) {
                        return;
                    }
                    --operations_;
                    if (!unchanged) {
                        undoing_ = false;
                        show_message(std::format("“{}” has changed since it was made: not undone",
                                                 item.to.filename().string()),
                                     false);
                        return;
                    }
                    trash_paths({item.to}, purpose);
                });
            return;
        }
        case FileOperation::Kind::copy:
        case FileOperation::Kind::restore:
            trash_paths(placed, purpose);  // copies go to the trash: an undo can be undone
            return;
        case FileOperation::Kind::trash: {
            std::vector<std::string> paths;
            paths.reserve(placed.size());
            for (const std::filesystem::path& path : placed) {
                paths.push_back(path.string());
            }
            undoing_ = true;
            submit({.kind = fs::TransferKind::restore,
                    .source = fs::trash_path(),
                    .names = std::move(paths),
                    .destination = {},
                    .force_copy = false},
                   open_batch(purpose, FileOperation::Kind::restore));
            return;
        }
        case FileOperation::Kind::move: {
            // Back where each came from, one transfer per pair of folders.
            const std::uint64_t batch = open_batch(purpose, FileOperation::Kind::move);
            std::vector<std::pair<std::pair<std::filesystem::path, std::filesystem::path>,
                                  std::vector<std::string>>>
                moves;
            for (const fs::PlacedItem& item : operation.items) {
                const std::pair folders{item.to.parent_path(), item.from.parent_path()};
                auto it = std::ranges::find(moves, folders, [](const auto& m) { return m.first; });
                if (it == moves.end()) {
                    moves.emplace_back(folders, std::vector<std::string>{});
                    it = std::prev(moves.end());
                }
                it->second.push_back(item.to.filename().string());
            }
            undoing_ = true;
            for (auto& [folders, names] : moves) {
                submit({.kind = fs::TransferKind::move,
                        .source = folders.first,
                        .names = std::move(names),
                        .destination = folders.second,
                        .force_copy = false},
                       batch);
            }
            return;
        }
    }
}

Browser::~Browser() {
    *alive_ = false;
    loop_.remove(auto_scroll_timer_);
    loop_.remove(band_timer_);
}

void Browser::start_rename() {
    const std::shared_ptr<Folder> folder = current_folder();
    const fs::Entry* const entry = folder ? folder->cursor_entry() : nullptr;
    if (entry == nullptr) {
        return;
    }
    editor_.start(folder, entry->name, entry->is_directory());
}

void Browser::new_folder() {
    std::shared_ptr<Folder> folder = current_folder();
    if (!folder) {
        return;
    }
    ++operations_;
    pool_.submit(
        core::Priority::visible,
        [directory = folder->path()](const core::CancelToken& /*token*/) {
            return fs::make_folder(directory, "New Folder");
        },
        [this, alive = alive_, folder](const fs::MadeFolder& made) {
            if (!*alive) {
                return;
            }
            --operations_;
            if (made.error) {
                show_message(std::format("Cannot make a folder in {}: {}",
                                         folder->path().filename().string(), made.error.message()),
                             true);
                return;
            }
            folder->expect_name(made.name);  // lands when the watcher reports it
            record({.kind = FileOperation::Kind::create,
                    .items = {{.from = {}, .to = folder->path() / made.name}}},
                   Purpose::user, false);
            if (current_folder() == folder && !editor_.active()) {
                editor_.start(folder, made.name, true);
            }
        });
}

void Browser::new_file(const std::string& name, const std::filesystem::path& from) {
    std::shared_ptr<Folder> folder = current_folder();
    if (!folder) {
        return;
    }
    ++operations_;
    pool_.submit(
        core::Priority::visible,
        [directory = folder->path(), name, from](const core::CancelToken& /*token*/) {
            return fs::make_file(directory, name, from);
        },
        [this, alive = alive_, folder, from](const fs::MadeFile& made) {
            if (!*alive) {
                return;
            }
            --operations_;
            if (made.error) {
                show_message(std::format("Cannot make a file in {}: {}",
                                         folder->path().filename().string(), made.error.message()),
                             true);
                return;
            }
            folder->expect_name(made.name);  // lands when the watcher reports it
            record({.kind = FileOperation::Kind::create,
                    .items = {{.from = from, .to = folder->path() / made.name}}},
                   Purpose::user, false);
            if (current_folder() == folder && !editor_.active()) {
                editor_.start(folder, made.name, false);
            }
        });
}

std::vector<MenuItem> Browser::entry_menu(const std::shared_ptr<Folder>& folder) {
    const std::vector<std::string> names = targets(*folder);
    const fs::Entry* const cursor = folder->cursor_entry();
    std::vector<MenuItem> items;
    if (folder->is_trash()) {
        MenuItem back;
        back.id = "restore";
        back.label = "Restore";
        back.action = [this] { restore(); };
        items.push_back(std::move(back));
        MenuItem gone;
        gone.id = "erase";
        gone.label = "Delete Permanently…";
        gone.keys = keys_for(Command::trash);
        gone.separator = true;
        gone.action = [this] { erase(); };
        items.push_back(std::move(gone));
        return items;
    }
    const auto add = [&items](std::string id, std::string label, std::string keys,
                              std::function<void()> action, bool separator = false,
                              bool enabled = true) {
        MenuItem item;
        item.id = std::move(id);
        item.label = std::move(label);
        item.keys = std::move(keys);
        item.action = std::move(action);
        item.separator = separator;
        item.enabled = enabled;
        items.push_back(std::move(item));
    };
    add("open", "Open", "Enter", [this] {
        ui::KeyEvent enter;
        enter.keysym = XKB_KEY_Return;
        view_key(enter, Clock::now());
    });
    if (cursor != nullptr && choices_ && open_with_) {
        MenuItem with;
        with.id = "open_with";
        with.label = "Open With";
        with.submenu = true;
        with.empty_text = "Looking for applications…";
        items.push_back(std::move(with));
        // What it would open: the selected entries of the cursor's kind (files, or folders), else
        // the cursor's; their types as known.
        std::vector<fs::Entry> files;
        for (const fs::Entry& entry : folder->entries()) {
            if (entry.is_directory() == cursor->is_directory() &&
                std::ranges::find(names, entry.name) != names.end()) {
                fs::Entry typed = entry;
                typed.mime = folder->mime_of(entry);
                files.push_back(std::move(typed));
            }
        }
        fs::Entry typed = *cursor;
        typed.mime = folder->mime_of(*cursor);
        ++menu_loads_;
        menu_loaders_.emplace_back([this, directory = folder->path(), typed = std::move(typed),
                                    files = std::move(files)]() mutable {
            choices_(directory, std::move(typed),
                     [this, alive = alive_, generation = menu_generation_, directory,
                      files = std::move(files)](const std::vector<AppChoice>& apps) {
                         if (!*alive) {
                             return;
                         }
                         --menu_loads_;
                         if (generation != menu_generation_) {
                             return;  // another menu now
                         }
                         std::vector<MenuItem> children;
                         for (const AppChoice& app : apps) {
                             MenuItem item;
                             item.id = app.id;
                             item.label = app.is_default ? app.name + " (default)" : app.name;
                             item.action = [this, directory, files, id = app.id] {
                                 open_with_(directory, files, id);
                             };
                             children.push_back(std::move(item));
                         }
                         if (children.empty()) {
                             MenuItem none;  // a dimmed line says so
                             none.label = "No applications";
                             none.enabled = false;
                             children.push_back(std::move(none));
                         }
                         menu_.set_submenu("open_with", std::move(children));
                     });
        });
    }
    const std::size_t count = names.size();
    std::vector<std::filesystem::path> folders;
    for (const fs::Entry& entry : folder->entries()) {
        if (entry.is_directory() && std::ranges::find(names, entry.name) != names.end()) {
            folders.push_back(folder->path() / entry.name);
        }
    }
    if (!folders.empty() && folders.size() == count) {
        // Behind the tab shown, in their order: each right after the one before.
        add("open_tab", count == 1 ? "Open in New Tab" : "Open in New Tabs", "",
            [this, folders] { open_tabs(folders); });
    }
    if (cursor != nullptr && cursor->is_directory() && terminal_) {
        add("open_terminal", "Open in Terminal", keys_for(Command::open_terminal),
            [this] { open_terminal(); });
    }
    if (!folders.empty() && folders.size() == count) {
        add("add_bookmark", "Add to Sidebar", count == 1 ? keys_for(Command::add_bookmark) : "",
            [this, folders] { add_to_sidebar(folders); });
    }
    add("copy", "Copy", keys_for(Command::copy), [this] { take_selection(false); }, true);
    add("cut", "Cut", keys_for(Command::cut), [this] { take_selection(true); });
    add(
        "paste", "Paste", keys_for(Command::paste), [this] { paste(); }, false,
        !clipboard_.names.empty());
    add(
        "rename", "Rename…", keys_for(Command::rename), [this] { start_rename(); }, true,
        count == 1);
    add("trash", "Move to Trash", keys_for(Command::trash), [this] { trash(); });
    add("erase", "Delete Permanently…", keys_for(Command::erase), [this] { erase(); });
    items.push_back(sort_menu());
    return items;
}

std::vector<MenuItem> Browser::folder_menu() {
    std::vector<MenuItem> items;
    MenuItem make_folder;
    make_folder.id = "new_folder";
    make_folder.label = "New Folder";
    make_folder.keys = keys_for(Command::new_folder);
    make_folder.action = [this] { new_folder(); };
    items.push_back(std::move(make_folder));

    // "New File": an empty one at once, the templates when they have been looked for.
    const auto empty_file = [this] {
        MenuItem item;
        item.id = "empty_file";
        item.label = "Empty File";
        item.action = [this] { new_file("New File", {}); };
        return item;
    };
    MenuItem make_file;
    make_file.id = "new_file";
    make_file.label = "New File";
    make_file.submenu = true;
    make_file.children.push_back(empty_file());
    items.push_back(std::move(make_file));
    ++menu_loads_;
    menu_loaders_.emplace_back([this, empty_file] {
        pool_.submit(
            core::Priority::visible,
            [sources = place_sources_, fixed = templates_dir_](const core::CancelToken& /*token*/) {
                std::filesystem::path directory = fixed ? *fixed : fs::templates_dir(sources);
                std::vector<std::string> names =
                    directory.empty() ? std::vector<std::string>{} : fs::templates_in(directory);
                return std::pair{std::move(directory), std::move(names)};
            },
            [this, alive = alive_, generation = menu_generation_,
             empty_file](const std::pair<std::filesystem::path, std::vector<std::string>>& found) {
                if (!*alive) {
                    return;
                }
                --menu_loads_;
                if (generation != menu_generation_ || found.second.empty()) {
                    return;
                }
                std::vector<MenuItem> children{empty_file()};
                bool first = true;
                for (const std::string& name : found.second) {
                    MenuItem item;
                    item.id = "template:" + name;
                    item.label = name;
                    item.separator = std::exchange(first, false);
                    item.action = [this, name, from = found.first / name] { new_file(name, from); };
                    children.push_back(std::move(item));
                }
                menu_.set_submenu("new_file", std::move(children));
            });
    });

    MenuItem paste_here;
    paste_here.id = "paste";
    paste_here.label = "Paste";
    paste_here.keys = keys_for(Command::paste);
    paste_here.separator = true;
    paste_here.enabled = !clipboard_.names.empty();
    paste_here.action = [this] { paste(); };
    items.push_back(std::move(paste_here));

    if (const std::shared_ptr<Folder> shown = current_folder()) {
        MenuItem bookmark;
        bookmark.id = "add_bookmark";
        bookmark.label = "Add to Sidebar";
        bookmark.keys = keys_for(Command::add_bookmark);
        bookmark.separator = true;
        bookmark.action = [this, path = shown->path()] { add_to_sidebar({path}); };
        items.push_back(std::move(bookmark));
        if (terminal_) {
            MenuItem terminal;
            terminal.id = "open_terminal";
            terminal.label = "Open in Terminal";
            terminal.keys = keys_for(Command::open_terminal);
            terminal.action = [this, path = shown->path()] { terminal_(path); };
            items.push_back(std::move(terminal));
        }
    }
    items.push_back(sort_menu());
    return items;
}

void Browser::open_menu(float x, float y, bool keyboard) {
    const std::shared_ptr<Folder> folder = current_folder();
    if (!folder) {
        return;
    }
    if (sidebar_.focused()) {
        sidebar_.set_focused(false);
    }
    ++menu_generation_;
    menu_loaders_.clear();
    std::vector<MenuItem> items;
    if (!targets(*folder).empty()) {
        items = entry_menu(folder);
    } else if (!folder->is_trash()) {
        items = folder_menu();
    } else {
        MenuItem empty;  // nothing is made in the trash
        empty.id = "empty_trash";
        empty.label = "Empty Trash…";
        empty.enabled = !folder->entries().empty();
        empty.action = [this] { empty_trash(); };
        items.push_back(std::move(empty));
    }
    if (items.empty()) {
        menu_loaders_.clear();
        return;
    }
    menu_.open(x, y, std::move(items), keyboard);
    // Submenus found later, once the menu they fill is open (the answer may come at once).
    for (const std::function<void()>& load : std::exchange(menu_loaders_, {})) {
        load();
    }
}

void Browser::open_menu_from_keyboard() {
    std::optional<std::pair<float, float>> anchor;
    switch (pane().mode) {
        case Mode::columns:
            anchor = pane().columns.cursor_anchor();
            break;
        case Mode::grid:
            anchor = pane().grid.cursor_anchor();
            break;
        case Mode::details:
            anchor = pane().details.cursor_anchor();
            break;
    }
    // No entry shown under the cursor: near the view's top left.
    const auto [x, y] =
        anchor.value_or(std::pair{view_area_.x + (view_area_.width / 4.0F), view_area_.y + 40.0F});
    open_menu(x, y, true);
}

void Browser::rename(const std::shared_ptr<Folder>& folder, const std::string& from,
                     const std::string& to) {
    ++operations_;
    pool_.submit(
        core::Priority::visible,
        [directory = folder->path(), from, to](const core::CancelToken& /*token*/) {
            return fs::rename_entry(directory, from, to);
        },
        [this, alive = alive_, folder, from, to](const fs::Renamed& renamed) {
            if (!*alive) {
                return;
            }
            --operations_;
            if (renamed.error) {
                show_message(renamed.problem, true);
                editor_.finished(false);
                return;
            }
            // The cursor follows the new name by itself (the watcher reports a rename), unless it
            // still waits for the old one: a new folder renamed before the watcher saw it.
            // A hidden name would leave the listing: the cursor keeps it in sight.
            if (folder->cursor_pending() || (!folder->shows_hidden() && to.starts_with('.'))) {
                folder->expect_name(to);
            }
            record({.kind = FileOperation::Kind::rename,
                    .items = {{.from = folder->path() / from, .to = folder->path() / to}}},
                   Purpose::user, false);
            if (!renamed.problem.empty()) {
                show_message(renamed.problem, true);
            } else if (renamed.sidecars > 0) {
                show_message(std::format("Renamed {} with {} darktable sidecar{}", to,
                                         renamed.sidecars, renamed.sidecars == 1 ? "" : "s"),
                             false);
            }
            editor_.finished(true);
        });
}

void Browser::open(const std::filesystem::path& directory) {
    pane().columns.open(directory);
    if (pane().mode == Mode::grid) {
        pane().columns.suspend();
        pane().grid.show(pane().columns.focused_folder());
    } else if (pane().mode == Mode::details) {
        pane().columns.suspend();
        pane().details.show(pane().columns.focused_folder());
    }
    note_location();
}

void Browser::reveal(const std::filesystem::path& root, const std::filesystem::path& target) {
    set_mode(Mode::columns);
    pane().columns.reveal(root, target);
    note_location();
}

void Browser::note_location() {
    if (pane().mode == Mode::columns && pane().columns.revealing()) {
        return;  // noted once the columns have unfolded
    }
    const std::shared_ptr<Folder> shown = current_folder();
    if (!shown) {
        return;
    }
    const fs::Entry* const cursor = shown->cursor_entry();
    if (!pane().history.empty() && pane().history[pane().here].folder == shown->path()) {
        if (cursor != nullptr) {
            pane().history[pane().here].cursor = cursor->name;
        }
        return;
    }
    if (!pane().history.empty()) {
        pane().history.erase(pane().history.begin() + static_cast<std::ptrdiff_t>(pane().here) + 1,
                             pane().history.end());
    }
    pane().history.push_back({.folder = shown->path(), .cursor = cursor ? cursor->name : ""});
    if (pane().history.size() > kHistorySize) {
        pane().history.erase(pane().history.begin());
    }
    pane().here = pane().history.size() - 1;
}

void Browser::back() { travel(-1); }

void Browser::forward() { travel(1); }

void Browser::travel(int direction) {
    note_location();  // the cursor where it is now
    quick_look_.close();
    const auto ahead = [&] {
        return direction < 0 ? pane().here > 0 : pane().here + 1 < pane().history.size();
    };
    std::vector<std::string> gone;
    const auto report_gone = [&] {
        if (!gone.empty()) {
            show_message(gone.size() == 1 ? std::format("{} no longer exists", gone.front())
                                          : std::format("{} folders no longer exist", gone.size()),
                         true);
        }
    };
    bool moved = false;
    while (ahead()) {
        pane().here = direction < 0 ? pane().here - 1 : pane().here + 1;
        const Step step = pane().history[pane().here];  // go_to() notes the cursor it finds there
        go_to(step.folder, step.cursor);
        moved = true;
        const std::shared_ptr<Folder> shown = current_folder();
        if (!shown) {
            return;
        }
        // Opening a folder that is not there fails at once (it cannot be watched).
        const std::error_code& error = shown->model().error();
        if (error != std::errc::no_such_file_or_directory && error != std::errc::not_a_directory) {
            if (!step.cursor.empty() &&
                (shown->cursor_entry() == nullptr || shown->cursor_entry()->name != step.cursor)) {
                shown->select_name(step.cursor);
            }
            pane().history[pane().here].cursor = step.cursor;
            report_gone();
            return;
        }
        gone.push_back(step.folder.filename().string());
        pane().history.erase(pane().history.begin() + static_cast<std::ptrdiff_t>(pane().here));
        if (direction > 0) {
            --pane().here;  // back on the step we left: the next one ahead took its place
        }
    }
    if (moved) {
        const Step step = pane().history[pane().here];
        go_to(step.folder, step.cursor);  // nothing there: back where it started
    }
    report_gone();
}

void Browser::set_list_size(std::size_t index) {
    pane().columns.set_list_size(index);
    pane().details.set_list_size(index);
}

void Browser::set_grid_size(std::size_t index) { pane().grid.set_size_index(index); }

std::size_t Browser::grid_index(std::int32_t pixels) {
    const auto* found = std::ranges::find(GridView::kIconSizes, static_cast<float>(pixels));
    return found == GridView::kIconSizes.end()
               ? GridView::kDefaultSize
               : static_cast<std::size_t>(found - GridView::kIconSizes.begin());
}

void Browser::set_sort(fs::SortKey key, bool descending) {
    for (const std::unique_ptr<Pane>& each : panes_) {
        each->details.set_sort(key, descending);
        each->columns.set_order(each->details.order());
        each->grid.set_order(each->details.order());
    }
}

MenuItem Browser::sort_menu() {
    const fs::SortOrder& now = sort_order();
    MenuItem sort;
    sort.id = "sort";
    sort.label = "Sort By";
    sort.submenu = true;
    sort.separator = true;
    const auto choice = [](std::string id, std::string label, bool checked,
                           std::function<void()> action, bool separator = false) {
        MenuItem item;
        item.id = std::move(id);
        item.label = std::move(label);
        item.radio = true;
        item.checked = checked;
        item.separator = separator;
        item.action = std::move(action);
        return item;
    };

    struct Key {
        fs::SortKey key;
        const char* id;
        const char* label;
    };

    constexpr std::array<Key, 4> sort_keys{{
        {.key = fs::SortKey::name, .id = "sort:name", .label = "Name"},
        {.key = fs::SortKey::size, .id = "sort:size", .label = "Size"},
        {.key = fs::SortKey::modified, .id = "sort:modified", .label = "Modified"},
        {.key = fs::SortKey::type, .id = "sort:type", .label = "Type"},
    }};
    for (const Key& choice_of : sort_keys) {
        sort.children.push_back(
            choice(choice_of.id, choice_of.label, now.key == choice_of.key,
                   [this, key = choice_of.key] { set_sort(key, sort_order().descending); }));
    }
    sort.children.push_back(choice(
        "sort:ascending", "Ascending", !now.descending,
        [this] { set_sort(sort_order().key, false); }, true));
    sort.children.push_back(choice("sort:descending", "Descending", now.descending,
                                   [this] { set_sort(sort_order().key, true); }));
    return sort;
}

void Browser::set_show_hidden(bool show) {
    show_hidden_ = show;
    for (const std::unique_ptr<Pane>& each : panes_) {
        each->columns.set_show_hidden(show);
        each->grid.set_show_hidden(show);
        each->details.set_show_hidden(show);
    }
    show_message(show ? "Hidden files shown" : "Hidden files hidden", false);
    redraw_();
}

void Browser::set_info_visible(bool visible) {
    info_visible_ = visible;
    for (const std::unique_ptr<Pane>& each : panes_) {
        each->grid.set_info_visible(visible);
        each->details.set_info_visible(visible);
    }
}

void Browser::set_sidebar_visible(bool visible) {
    if (visible == sidebar_visible_) {
        return;
    }
    sidebar_visible_ = visible;
    if (!visible) {
        sidebar_.set_focused(false);
        if (pointer_target_ == Target::sidebar) {
            send_pointer(Target::sidebar, {.kind = ui::PointerEvent::Kind::leave}, Clock::now());
            pointer_target_ = Target::view;
            button_held_ = false;
        }
    }
    redraw_();
}

// The remembered mode is the browser's, by value.
static_assert(static_cast<int>(State::Mode::columns) == static_cast<int>(Browser::Mode::columns) &&
              static_cast<int>(State::Mode::grid) == static_cast<int>(Browser::Mode::grid) &&
              static_cast<int>(State::Mode::details) == static_cast<int>(Browser::Mode::details));

State Browser::state() const {
    State state;
    state.mode = static_cast<State::Mode>(pane().mode);
    state.show_hidden = show_hidden_;
    state.sidebar_visible = sidebar_visible_;
    state.sidebar_width = sidebar_.width();
    state.info_visible = info_visible_;
    state.info_width = info_pane_.width();
    state.info_expanded = info_pane_.expanded();
    state.sort_key = pane().details.order().key;
    state.sort_descending = pane().details.order().descending;
    state.details_widths = pane().details.widths();
    state.list_size = pane().details.list_size();
    state.grid_icon = static_cast<std::int32_t>(pane().grid.icon_size());
    state.palette_recent = palette_recent_;
    if (panes_.size() > 1) {  // one tab: nothing to restore, the start folder is the start's
        for (std::size_t i = 0; i < panes_.size() && i < State::kMaxTabs; ++i) {
            const Pane& each = *panes_[i];
            const std::shared_ptr<Folder> folder = folder_of(each);
            if (!folder || folder->is_trash()) {
                continue;
            }
            State::Tab tab{.folder = folder->path(),
                           .mode = static_cast<State::Mode>(each.mode),
                           .root = {},
                           .cursor = {},
                           .list_size = {},
                           .grid_icon = {}};
            if (each.mode == Mode::columns && each.columns.column_count() > 0) {
                tab.root = each.columns.column(0).folder().path();
            }
            if (const fs::Entry* entry = folder->cursor_entry()) {
                tab.cursor = entry->name;
            }
            tab.list_size = each.details.list_size();
            tab.grid_icon = static_cast<std::int32_t>(each.grid.icon_size());
            if (i == active_) {
                state.shown_tab = state.tabs.size();
            }
            state.tabs.push_back(std::move(tab));
        }
    }
    return state;
}

bool Browser::restore_tabs(const std::vector<State::Tab>& tabs, std::size_t shown) {
    const auto folder_there = [](const std::filesystem::path& path) {
        std::error_code error;
        return !path.empty() && std::filesystem::is_directory(path, error);
    };
    // Tabs written before they had a zoom of their own take the window's (apply()).
    const std::size_t window_list = pane().details.list_size();
    const std::size_t window_grid = pane().grid.size_index();
    std::size_t restored = 0;
    std::size_t to_show = 0;
    for (std::size_t i = 0; i < tabs.size(); ++i) {
        const State::Tab& tab = tabs[i];
        if (!folder_there(tab.folder)) {
            continue;  // gone meanwhile (an unplugged disk): left out
        }
        if (restored > 0) {
            add_pane(restored);
        }
        active_ = restored;
        pane().mode = static_cast<Mode>(tab.mode);
        const std::filesystem::path relative = tab.folder.lexically_relative(tab.root);
        if (pane().mode == Mode::columns && folder_there(tab.root) && !relative.empty() &&
            *relative.begin() != "..") {
            // The columns unfold as they were, down to the cursor in the folder shown.
            pane().columns.reveal(tab.root,
                                  tab.cursor.empty() ? tab.folder : tab.folder / tab.cursor);
        } else {
            open(tab.folder);
            if (const std::shared_ptr<Folder> opened = current_folder();
                opened && !tab.cursor.empty()) {
                opened->select_name(tab.cursor);
            }
        }
        set_list_size(tab.list_size.value_or(window_list));
        set_grid_size(tab.grid_icon ? grid_index(*tab.grid_icon) : window_grid);
        note_location();
        if (i == shown) {
            to_show = restored;
        }
        ++restored;
    }
    active_ = std::min(to_show, panes_.size() - 1);
    redraw_();
    return restored > 0;
}

void Browser::apply(const State& state) {
    show_hidden_ = state.show_hidden;
    palette_recent_ = state.palette_recent;
    for (const std::unique_ptr<Pane>& each : panes_) {
        each->columns.set_show_hidden(state.show_hidden);
        each->grid.set_show_hidden(state.show_hidden);
        each->details.set_show_hidden(state.show_hidden);
        each->details.set_widths(state.details_widths);
    }
    set_sidebar_visible(state.sidebar_visible);
    sidebar_.set_width(state.sidebar_width);
    set_info_visible(state.info_visible);
    info_pane_.set_width(state.info_width);
    info_pane_.set_expanded(state.info_expanded);
    set_sort(state.sort_key, state.sort_descending);
    set_list_size(state.list_size);
    set_grid_size(grid_index(state.grid_icon));
    const auto mode = static_cast<Mode>(state.mode);
    if (current_folder()) {
        set_mode(mode);
    } else {
        pane().mode = mode;  // open() shows the folder in it
    }
    redraw_();
}

void Browser::set_mode(Mode mode) {
    if (mode == pane().mode) {
        return;
    }
    std::shared_ptr<Folder> folder = current_folder();
    if (!folder) {
        return;
    }
    // The view that leaves forgets the pointer; the one that comes finds it where it is.
    const auto now = Clock::now();
    const ui::PointerEvent leave{.kind = ui::PointerEvent::Kind::leave};
    const ui::PointerEvent motion{
        .kind = ui::PointerEvent::Kind::motion, .x = pointer_x_, .y = pointer_y_};
    if (pointer_target_ == Target::view) {
        view_pointer(leave, now);
    }
    if (pane().mode == Mode::columns) {
        pane().columns.suspend();
    }
    pane().mode = mode;
    switch (pane().mode) {
        case Mode::columns:
            pane().columns.show(std::move(folder));
            break;
        case Mode::grid:
            pane().grid.show(std::move(folder));
            break;
        case Mode::details:
            pane().details.show(std::move(folder));
            break;
    }
    if (pointer_inside_) {
        dispatch_pointer(motion, now);
    }
    redraw_();
}

void Browser::draw(ui::Renderer& renderer, const ui::Rect& bounds, const theme::Palette& palette,
                   ui::IconSet& icons, Clock::time_point now) {
    const float sidebar_width = sidebar_visible_ ? sidebar_.width_in(bounds.width) : 0.0F;
    sidebar_drawn_ = sidebar_width > 0.0F;
    info_pane_.forget_layout();  // drawn again below by the view that shows it
    if (!sidebar_drawn_ && sidebar_.focused()) {
        sidebar_.set_focused(false);  // no room for it: the keyboard goes back to the view
    }
    settle_found(now);
    const std::shared_ptr<Folder> folder = current_folder();
    if (sidebar_drawn_) {
        sidebar_.draw(renderer, {bounds.x, bounds.y, sidebar_width, bounds.height}, palette, icons,
                      folder ? folder->path() : std::filesystem::path{}, now);
    } else {
        sidebar_.not_drawn();
    }
    ui::Rect view{bounds.x + sidebar_width, bounds.y, bounds.width - sidebar_width, bounds.height};
    if (panes_.size() > 1 && view.height > TabBar::kHeight + kTitleHeight) {
        tab_bar_.draw(renderer, {view.x, view.y, view.width, TabBar::kHeight}, palette, icons,
                      tab_titles(), active_);
        view.y += TabBar::kHeight;
        view.height -= TabBar::kHeight;
    } else {
        tab_bar_.not_drawn();
    }
    const float bar = std::min(StatusBar::kHeight, std::max(0.0F, view.height - kTitleHeight));
    if (folder) {
        breadcrumb_.draw(renderer, {view.x, view.y, view.width, kTitleHeight}, palette, icons,
                         *folder);
        status_bar_.draw(renderer, {view.x, view.bottom() - bar, view.width, bar}, palette, icons,
                         *folder, static_cast<std::size_t>(pane().mode));
    }
    if (editor_.active() && !editor_.busy()) {
        // The entry went away meanwhile (another program): nothing left to rename. A new folder
        // not listed yet is still on its way: the cursor waits for it.
        const Folder& edited = *editor_.folder();
        if (!edited.cursor_pending() &&
            std::ranges::find(edited.entries(), editor_.slot().name, &fs::Entry::name) ==
                edited.entries().end()) {
            editor_.cancel();
        }
    }
    editor_.slot().rect.reset();  // the view places it again if it shows the name
    view_area_ = {view.x, view.y + kTitleHeight, view.width,
                  std::max(0.0F, view.height - kTitleHeight - bar)};
    draw_view(renderer, view_area_, palette, icons, now);
    draw_band(renderer, palette);
    editor_.draw(renderer, palette);
    if (quick_look_.is_open()) {
        quick_look_.follow(current_folder());  // entries may have moved or gone meanwhile
    }
    quick_look_.draw(renderer, bounds, palette, icons, now);
    shortcuts_.draw(renderer, bounds, palette);
    menu_.draw(renderer, bounds, palette);
    palette_.draw(renderer, bounds, palette);
    conflict_.draw(renderer, bounds, palette);  // last: over everything else
    confirm_.draw(renderer, bounds, palette);
    password_.draw(renderer, bounds, palette);
    if (thumbnails_) {
        thumbnails_->end_frame();
    }
}

void Browser::draw_view(ui::Renderer& renderer, const ui::Rect& bounds,
                        const theme::Palette& palette, ui::IconSet& icons, Clock::time_point now) {
    switch (pane().mode) {
        case Mode::columns:
            pane().columns.draw(renderer, bounds, palette, icons, now);
            break;
        case Mode::grid:
            pane().grid.draw(renderer, bounds, palette, icons, now);
            break;
        case Mode::details:
            pane().details.draw(renderer, bounds, palette, icons, now);
            break;
    }
}

bool Browser::animating() const {
    bool view = false;
    switch (pane().mode) {
        case Mode::columns:
            view = pane().columns.animating();
            break;
        case Mode::grid:
            view = pane().grid.animating();
            break;
        case Mode::details:
            view = pane().details.animating();
            break;
    }
    return view || info_pane_.animating() || sidebar_.animating() ||
           quick_look_.animating(Clock::now());
}

bool Browser::loading() const {
    if (finder_ && (finder_->scanning() || finder_->matching())) {
        return true;
    }
    if (operations_ > 0 || menu_loads_ > 0 || transfers_.pending() > 0 || quick_look_.loading() ||
        info_pane_.loading() || status_bar_.loading()) {
        return true;
    }
    switch (pane().mode) {
        case Mode::grid:
            return pane().grid.loading() || (thumbnails_ && thumbnails_->loading());
        case Mode::details:
            return pane().details.loading();
        case Mode::columns:
            break;
    }
    return pane().columns.loading();
}

void Browser::pointer(const ui::PointerEvent& event, Clock::time_point now) {
    const bool button = event.kind == ui::PointerEvent::Kind::press ||
                        event.kind == ui::PointerEvent::Kind::release;
    if (button) {
        note_location();  // the cursor of the folder the click may leave
    }
    take_pointer(event, now);
    if (button) {
        note_location();
    }
    if (event.kind == ui::PointerEvent::Kind::leave) {
        // Someone else sets the shape now (the title bar, another window): send it again on
        // the way back.
        hover_cursor_ = ui::CursorShape::standard;
        shown_cursor_.reset();
        return;
    }
    hover_cursor_ = cursor_at(event.x, event.y);  // after the event: what it opened counts
    update_cursor();
}

std::function<void(ui::CursorShape)> Browser::resize_cursor() {
    return [this](ui::CursorShape shape) {
        resize_cursor_ = shape;
        update_cursor();
    };
}

void Browser::update_cursor() {
    const ui::CursorShape shape =
        resize_cursor_ != ui::CursorShape::standard ? resize_cursor_ : hover_cursor_;
    if (shape != shown_cursor_ && set_cursor_) {
        shown_cursor_ = shape;
        set_cursor_(shape);
    }
}

ui::CursorShape Browser::cursor_at(float x, float y) const {
    using Shape = ui::CursorShape;
    const auto hand = [](bool clickable) { return clickable ? Shape::pointer : Shape::standard; };
    // What takes the pointer first, as take_pointer() hands it out.
    if (conflict_.is_open()) {
        return hand(conflict_.clickable_at(x, y));
    }
    if (confirm_.is_open()) {
        return hand(confirm_.clickable_at(x, y));
    }
    if (password_.is_open()) {
        return hand(password_.clickable_at(x, y));
    }
    if (palette_.is_open()) {
        if (palette_.field_bounds().contains(x, y)) {
            return Shape::text;
        }
        return palette_.list_bounds().contains(x, y) ? Shape::pointer : Shape::standard;
    }
    if (menu_.is_open() || shortcuts_.is_open() || quick_look_.is_open()) {
        return Shape::standard;
    }
    if (editor_.active() && editor_.field().bounds().contains(x, y)) {
        return Shape::text;
    }
    if (sidebar_drawn_ && sidebar_.claims(x, y)) {
        return hand(sidebar_.clickable_at(x, y));
    }
    if (tab_bar_.claims(x, y)) {
        return hand(tab_bar_.clickable_at(x, y));
    }
    if (breadcrumb_.claims(x, y)) {
        return hand(breadcrumb_.clickable_at(x, y));
    }
    if (status_bar_.claims(x, y)) {
        return hand(status_bar_.clickable_at(x, y));
    }
    return hand(info_pane_.clickable_at(x, y));
}

void Browser::take_pointer(const ui::PointerEvent& event, Clock::time_point now) {
    if (event.kind == ui::PointerEvent::Kind::press) {
        found_after_.reset();
    }
    if (event.kind == ui::PointerEvent::Kind::release &&
        (conflict_.is_open() || confirm_.is_open() || password_.is_open() || palette_.is_open() ||
         menu_.is_open() || shortcuts_.is_open() || quick_look_.is_open())) {
        // Taken by what the press opened (a menu from a right click): the button is up all the
        // same, and the next press finds its target again.
        button_held_ = false;
        band_press_.reset();
        end_band();
    }
    if (conflict_.is_open()) {
        conflict_.pointer(event);  // the question first: nothing under it reacts
        return;
    }
    if (confirm_.is_open()) {
        confirm_.pointer(event);
        return;
    }
    if (password_.is_open()) {
        password_.pointer(event);
        return;
    }
    if (palette_.is_open()) {
        palette_.pointer(event);  // a click outside closes it and does nothing else
        if (event.kind != ui::PointerEvent::Kind::leave) {
            pointer_x_ = event.x;
            pointer_y_ = event.y;
        }
        return;
    }
    if (menu_.is_open()) {
        menu_.pointer(event);  // a click outside closes it and does nothing else
        if (event.kind != ui::PointerEvent::Kind::leave) {
            pointer_x_ = event.x;
            pointer_y_ = event.y;
        }
        return;
    }
    // While a name is edited, a press in its field is the field's; elsewhere it keeps the name,
    // then does what it does.
    if (editor_.pointer(event)) {
        return;
    }
    if (shortcuts_.is_open()) {
        // A click anywhere closes the shortcuts; nothing under them reacts meanwhile.
        if (event.kind == ui::PointerEvent::Kind::press) {
            shortcuts_.close();
        }
        if (event.kind != ui::PointerEvent::Kind::leave) {
            pointer_x_ = event.x;
            pointer_y_ = event.y;
        }
        return;
    }
    if (quick_look_.is_open()) {
        // Over the overlay: a click on a text page selects, elsewhere it closes; scrolling
        // moves a text page; the rest waits until it is gone.
        const bool taken = quick_look_.pointer(event, now);
        if (event.kind == ui::PointerEvent::Kind::press && !taken) {
            quick_look_.close();
        }
        if (event.kind != ui::PointerEvent::Kind::leave) {
            pointer_x_ = event.x;
            pointer_y_ = event.y;
        }
        return;
    }
    if (event.kind == ui::PointerEvent::Kind::leave) {
        pointer_inside_ = false;
    } else {
        pointer_inside_ = true;
        pointer_x_ = event.x;
        pointer_y_ = event.y;
    }
    if (event.kind == ui::PointerEvent::Kind::press &&
        (event.button == ui::PointerButton::back || event.button == ui::PointerButton::forward)) {
        travel(event.button == ui::PointerButton::back ? -1 : 1);
        return;
    }
    dispatch_pointer(event, now);
}

void Browser::dispatch_pointer(const ui::PointerEvent& event, Clock::time_point now) {
    using Kind = ui::PointerEvent::Kind;
    if (event.kind == Kind::leave) {
        send_pointer(pointer_target_, event, now);
        pointer_target_ = Target::view;
        button_held_ = false;
        band_press_.reset();
        end_band();
        return;
    }
    // While a button is held (a drag) the pointer stays with whoever took the press.
    if (!button_held_) {
        Target target = Target::view;
        if (sidebar_drawn_ && sidebar_.claims(event.x, event.y)) {
            target = Target::sidebar;
        } else if (tab_bar_.claims(event.x, event.y)) {
            target = Target::tabs;
        } else if (breadcrumb_.claims(event.x, event.y)) {
            target = Target::breadcrumb;
        } else if (status_bar_.claims(event.x, event.y)) {
            target = Target::status_bar;
        }
        if (target != pointer_target_) {
            send_pointer(pointer_target_, {.kind = Kind::leave}, now);
            pointer_target_ = target;
        }
    }
    if (event.kind == Kind::press) {
        button_held_ = true;
        if (pointer_target_ != Target::sidebar && sidebar_.focused()) {
            sidebar_.set_focused(false);  // a click elsewhere takes the keyboard back
        }
    } else if (event.kind == Kind::release) {
        button_held_ = false;
    }
    if (event.kind == Kind::press && event.button == ui::PointerButton::middle &&
        pointer_target_ == Target::view) {
        // A middle click on a folder opens it in a new tab, behind the one shown; the views take
        // no middle clicks.
        if (const std::optional<ViewHit> hit = view_hit(event.x, event.y);
            hit && hit->folder && hit->entry && !hit->folder->is_trash()) {
            const std::vector<fs::Entry>& entries = hit->folder->entries();
            if (*hit->entry < entries.size() && entries[*hit->entry].is_directory()) {
                const fs::Entry& clicked = entries[*hit->entry];
                // On a selected folder: every selected folder, as "Open in New Tabs".
                std::vector<std::filesystem::path> folders;
                if (hit->folder->is_selected(clicked.name)) {
                    for (const fs::Entry& entry : entries) {
                        if (entry.is_directory() && hit->folder->is_selected(entry.name)) {
                            folders.push_back(hit->folder->path() / entry.name);
                        }
                    }
                }
                if (folders.size() > 1) {
                    open_tabs(folders);
                } else {
                    open_tab_behind(hit->folder->path() / clicked.name);
                }
            }
        }
        return;
    }
    if (event.kind == Kind::motion && press_ && dnd_.start && button_held_ &&
        pointer_target_ == Target::view &&
        std::hypot(event.x - press_->x, event.y - press_->y) >= kDragThreshold) {
        Press press = std::move(*press_);
        press_.reset();
        start_drag(std::move(press));
        return;
    }
    if (event.kind == Kind::motion && band_) {
        band_->x = event.x;
        band_->y = event.y;
        update_band();
        return;  // the views see nothing meanwhile (no hover)
    }
    if (event.kind == Kind::motion && band_press_ && button_held_ &&
        pointer_target_ == Target::view &&
        std::hypot(event.x - band_press_->x, event.y - band_press_->y) >= kDragThreshold) {
        start_band(event.x, event.y);
        return;
    }
    send_pointer(pointer_target_, event, now);
    if (event.kind == Kind::scroll && band_) {
        update_band();  // the list moved under it
    }
    if (event.kind == Kind::press) {
        press_.reset();
        band_press_.reset();
        end_band();
        if (pointer_target_ == Target::view && event.button == ui::PointerButton::left &&
            event.click_count == 1) {
            // On an entry's icon or name, selected by the press just handled, a drag may start;
            // on empty space, or beside the name, a band.
            if (std::optional<ViewHit> hit = view_hit(event.x, event.y); hit && hit->on_item) {
                press_ = Press{.x = event.x, .y = event.y, .hit = std::move(*hit)};
            } else if (hit) {
                band_press_ = BandPress{.x = event.x,
                                        .y = event.y,
                                        .folder = std::move(hit->folder),
                                        .add = event.modifiers.ctrl};
            }
        }
    } else if (event.kind == Kind::release && event.button == ui::PointerButton::left) {
        if (press_) {
            press_->hit.folder->release_press();
            press_.reset();
        }
        if (band_press_) {
            band_press_->folder->release_press();
            band_press_.reset();
        }
        end_band();
    }
}

std::optional<BandLane> Browser::band_lane(const Folder& folder) const {
    switch (pane().mode) {
        case Mode::grid:
            return pane().grid.band_lane(folder);
        case Mode::details:
            return pane().details.band_lane(folder);
        case Mode::columns:
            break;
    }
    return pane().columns.band_lane(folder);
}

void Browser::start_band(float x, float y) {
    if (!band_press_) {
        return;
    }
    BandPress press = std::move(*band_press_);
    band_press_.reset();
    const std::optional<BandLane> lane = band_lane(*press.folder);
    if (!lane) {
        return;
    }
    std::vector<std::string> base;
    if (press.add) {
        base = press.folder->selected_names();
    }
    band_ = Band{.folder = std::move(press.folder),
                 .anchor_x = press.x - lane->area.x,
                 .anchor_y = press.y - lane->area.y + lane->offset,
                 .base = std::move(base),
                 .x = x,
                 .y = y};
    update_band();
}

void Browser::update_band() {
    if (!band_) {
        return;
    }
    Band& band = *band_;
    const std::optional<BandLane> lane = band_lane(*band.folder);
    if (!lane || lane->area.width <= 0.0F || lane->area.height <= 0.0F) {
        end_band();  // its folder is not shown any more
        return;
    }
    const ui::Rect& area = lane->area;
    const float x = std::clamp(band.x, area.x, area.right()) - area.x;
    const float y = std::clamp(band.y, area.y, area.bottom()) - area.y + lane->offset;
    const ui::Rect content{std::min(x, band.anchor_x), std::min(y, band.anchor_y),
                           std::abs(x - band.anchor_x), std::abs(y - band.anchor_y)};
    std::vector<std::size_t> touched;
    switch (pane().mode) {
        case Mode::grid:
            touched = pane().grid.band_entries(*band.folder, content);
            break;
        case Mode::details:
            touched = pane().details.band_entries(*band.folder, content);
            break;
        case Mode::columns:
            touched = pane().columns.band_entries(*band.folder, content);
            break;
    }
    band.folder->select_band(band.base, touched);
    redraw_();
    // Beyond the top or the bottom of the list, it scrolls on its own.
    const bool beyond = band.y < area.y || band.y >= area.bottom();
    if (beyond && band_timer_ == 0) {
        band_timer_ = loop_.add_timer(std::chrono::milliseconds{16}, std::chrono::milliseconds{16},
                                      [this] { scroll_band(); });
    } else if (!beyond && band_timer_ != 0) {
        loop_.remove(band_timer_);
        band_timer_ = 0;
    }
}

void Browser::scroll_band() {
    const std::optional<BandLane> lane = band_ ? band_lane(*band_->folder) : std::nullopt;
    if (!lane) {
        end_band();
        return;
    }
    // Faster the farther the pointer is: a pixel a tick per 4 pixels beyond, up to 40.
    const float beyond =
        band_->y < lane->area.y ? band_->y - lane->area.y : band_->y - lane->area.bottom();
    const float step = std::clamp(beyond / 4.0F, -40.0F, 40.0F);
    const float pixels = step < 0.0F ? std::min(step, -2.0F) : std::max(step, 2.0F);
    Folder& folder = *band_->folder;
    switch (pane().mode) {
        case Mode::grid:
            pane().grid.band_scroll(folder, pixels);
            break;
        case Mode::details:
            pane().details.band_scroll(folder, pixels);
            break;
        case Mode::columns:
            pane().columns.band_scroll(folder, pixels);
            break;
    }
    update_band();
}

void Browser::end_band() {
    loop_.remove(band_timer_);
    band_timer_ = 0;
    if (band_) {
        band_.reset();
        redraw_();
    }
}

void Browser::draw_band(ui::Renderer& renderer, const theme::Palette& palette) const {
    if (!band_) {
        return;
    }
    const std::optional<BandLane> lane = band_lane(*band_->folder);
    if (!lane) {
        return;
    }
    const ui::Rect& area = lane->area;
    const float left = area.x + band_->anchor_x;
    const float top = area.y + band_->anchor_y - lane->offset;
    const float x = std::clamp(band_->x, area.x, area.right());
    const float y = std::clamp(band_->y, area.y, area.bottom());
    renderer.push_clip(area);
    renderer.draw_box({std::min(left, x), std::min(top, y), std::abs(x - left), std::abs(y - top)},
                      // The text's colour: it shows over the selection it makes, which has
                      // the accent's.
                      {.fill = palette.on_surface.with_alpha(0.12F),
                       .radius = ui::Corners::all(3.0F),
                       .border_width = 1.5F,
                       .border = palette.on_surface.with_alpha(0.85F)});
    renderer.pop_clip();
}

void Browser::send_pointer(Target target, const ui::PointerEvent& event, Clock::time_point now) {
    switch (target) {
        case Target::view:
            view_pointer(event, now);
            break;
        case Target::sidebar:
            sidebar_.pointer(event, now);
            break;
        case Target::breadcrumb:
            breadcrumb_.pointer(event);
            break;
        case Target::status_bar:
            status_bar_.pointer(event);
            break;
        case Target::tabs:
            tab_bar_.pointer(event);
            break;
    }
}

void Browser::go_to(const std::filesystem::path& folder, const std::string& child) {
    if (pane().mode == Mode::columns) {
        // The column that shows folder already has its cursor on child.
        if (!pane().columns.focus_folder(folder)) {
            pane().columns.reveal(folder, child.empty() ? folder : folder / child);
        }
        redraw_();
    } else {
        open(folder);
        if (const std::shared_ptr<Folder> shown = current_folder(); shown && !child.empty()) {
            shown->select_name(child);
        }
    }
    note_location();
}

std::optional<ViewHit> Browser::view_hit(float x, float y) const {
    switch (pane().mode) {
        case Mode::grid:
            return pane().grid.hit(x, y);
        case Mode::details:
            return pane().details.hit(x, y);
        case Mode::columns:
            break;
    }
    return pane().columns.hit(x, y);
}

void Browser::start_drag(Press press) {
    Folder& folder = *press.hit.folder;
    folder.forget_press();
    if (folder.is_trash() || !press.hit.entry || *press.hit.entry >= folder.entries().size()) {
        return;  // the trash only gives back: Restore
    }
    const fs::Entry& entry = folder.entries()[*press.hit.entry];
    std::vector<std::string> names = folder.is_selected(entry.name)
                                         ? folder.selected_names()
                                         : std::vector<std::string>{entry.name};
    fs::Entry typed = entry;
    typed.mime = folder.mime_of(entry);
    std::vector<std::filesystem::path> paths;
    paths.reserve(names.size());
    for (const std::string& name : names) {
        paths.push_back(folder.path() / name);
    }
    bool folders_only = true;
    for (const fs::Entry& listed : folder.entries()) {
        if (!listed.is_directory() && std::ranges::find(names, listed.name) != names.end()) {
            folders_only = false;
        }
    }
    drag_ = Drag{.directory = folder.path(),
                 .names = std::move(names),
                 .icon = std::string{fs::icon_name(folder.mime(), typed)},
                 .folders_only = folders_only};
    // The view lets go of the press: the drag has the pointer now.
    send_pointer(Target::view, {.kind = ui::PointerEvent::Kind::leave}, Clock::now());
    button_held_ = false;
    const std::string lines = path_lines(paths);
    dnd_.start({{std::string{kUriList}, uri_list(paths)},
                {std::string{kText}, lines},
                {"text/plain", lines}});
    redraw_();
}

void Browser::drag_ended() {
    drag_.reset();
    show_drop_target(std::nullopt);
    stop_auto_scroll();
}

void Browser::draw_drag_icon(ui::Renderer& renderer, const ui::Rect& bounds,
                             const theme::Palette& palette, ui::IconSet& icons) const {
    renderer.draw_box(
        bounds,
        {.fill = palette.surface_container_high, .border_width = 1.0F, .border = palette.primary});
    if (!drag_) {
        return;
    }
    constexpr float icon_size = 20.0F;
    constexpr float text_size = 13.0F;
    const float left = bounds.x + 10.0F;
    icons.draw(renderer, drag_->icon, left, bounds.y + ((bounds.height - icon_size) / 2.0F),
               icon_size, palette.primary);
    const std::string label = drag_->names.size() == 1
                                  ? drag_->names.front()
                                  : std::format("{} items", drag_->names.size());
    const float text_x = left + icon_size + 8.0F;
    renderer.draw_text(label, text_x, bounds.y + ((bounds.height - text_size) / 2.0F) - 2.0F,
                       text_size, palette.on_surface,
                       std::max(1.0F, bounds.right() - 10.0F - text_x), ui::Ellipsis::middle);
}

std::optional<Browser::DropTarget> Browser::drop_target_at(float x, float y) const {
    if (sidebar_drawn_) {
        if (sidebar_.in_bookmark_zone(x, y)) {
            return DropTarget{.folder = {}, .mark = {}, .place = {}, .crumb = {}, .bookmark = true};
        }
        if (const fs::Place* const place = sidebar_.place_under(x, y)) {
            if (place->path.empty()) {
                return std::nullopt;  // a disk not mounted: nowhere to put anything yet
            }
            return DropTarget{.folder = place->path, .mark = {}, .place = place->path, .crumb = {}};
        }
        if (sidebar_.claims(x, y)) {
            return std::nullopt;
        }
    }
    if (const std::optional<std::filesystem::path> crumb = breadcrumb_.path_at(x, y)) {
        return DropTarget{.folder = *crumb, .mark = {}, .place = {}, .crumb = crumb};
    }
    const std::optional<ViewHit> hit = view_hit(x, y);
    if (!hit || hit->folder->is_trash()) {
        return std::nullopt;
    }
    if (hit->entry && *hit->entry < hit->folder->entries().size()) {
        const fs::Entry& entry = hit->folder->entries()[*hit->entry];
        if (entry.is_directory()) {
            return DropTarget{.folder = hit->folder->path() / entry.name,
                              .mark = {.directory = hit->folder->path(), .name = entry.name},
                              .place = {},
                              .crumb = {}};
        }
    }
    return DropTarget{.folder = hit->folder->path(),
                      .mark = {.directory = hit->folder->path(), .name = {}},
                      .place = {},
                      .crumb = {}};
}

std::string Browser::refusal(const std::filesystem::path& folder,
                             const std::vector<std::filesystem::path>& paths) {
    if (fs::is_trash(folder)) {
        return {};
    }
    for (const std::filesystem::path& path : paths) {
        const std::filesystem::path inside = folder.lexically_relative(path);
        if (!inside.empty() && *inside.begin() != "..") {
            return std::format("“{}” cannot go into itself", path.filename().string());
        }
    }
    if (std::ranges::all_of(paths, [&folder](const std::filesystem::path& path) {
            return path.parent_path() == folder;
        })) {
        return "Already here";
    }
    return {};
}

void Browser::show_drop_target(const std::optional<DropTarget>& target) {
    DropMark mark = target ? target->mark : DropMark{};
    if (mark.directory != drop_mark_.directory || mark.name != drop_mark_.name) {
        drop_mark_ = std::move(mark);
        redraw_();
    }
    sidebar_.set_drop_target(target ? target->place : std::nullopt);
    sidebar_.set_bookmark_drop(target && target->bookmark);
    breadcrumb_.set_drop_target(target ? target->crumb : std::nullopt);
}

void Browser::drop_event(const ui::DropEvent& event) {
    using Kind = ui::DropEvent::Kind;
    drop_x_ = event.x;
    drop_y_ = event.y;
    if (event.kind == Kind::leave) {
        show_drop_target(std::nullopt);
        stop_auto_scroll();
        return;
    }
    std::optional<DropTarget> target = drop_target_at(event.x, event.y);
    const std::vector<std::string> types = dnd_.types ? dnd_.types() : std::vector<std::string>{};
    const bool files = std::ranges::find(types, kUriList) != types.end();
    std::vector<std::filesystem::path> own;
    if (drag_) {
        for (const std::string& name : drag_->names) {
            own.push_back(drag_->directory / name);
        }
        if (target &&
            (target->bookmark ? !drag_->folders_only : !refusal(target->folder, own).empty())) {
            target.reset();  // not there: nothing would happen, or it cannot be
        }
    } else if (!files) {
        target.reset();
    }
    if (event.kind == Kind::drop) {
        show_drop_target(std::nullopt);
        stop_auto_scroll();
        if (drag_) {
            if (dnd_.finish) {
                dnd_.finish();
            }
            if (target && target->bookmark) {
                add_to_sidebar(std::move(own));
            } else if (target) {
                drop_paths(target->folder, std::move(own), true, false);
            }
            return;
        }
        if (!target || !dnd_.read) {
            return;
        }
        const bool move = dnd_.action && dnd_.action() == ui::kDndMove;
        ++operations_;
        dnd_.read(std::string{kUriList},
                  [this, alive = alive_, folder = target->folder, bookmark = target->bookmark,
                   move](const std::optional<std::string>& data) {
                      if (!*alive) {
                          return;
                      }
                      --operations_;
                      ClipboardFiles dropped = data ? parse_uri_list(*data) : ClipboardFiles{};
                      if (dropped.paths.empty()) {
                          show_message(dropped.refused > 0 ? "Only files can be dropped"
                                                           : "Nothing could be read from the drop",
                                       false);
                          return;
                      }
                      if (bookmark) {
                          add_to_sidebar(std::move(dropped.paths));
                      } else {
                          drop_paths(folder, std::move(dropped.paths), false, move);
                      }
                  });
        return;
    }
    show_drop_target(target);
    if (dnd_.accept) {
        if (!target) {
            dnd_.accept(std::nullopt, 0, 0);
        } else if (drag_) {
            dnd_.accept(std::string{kUriList}, ui::kDndCopy, ui::kDndCopy);  // decided at the drop
        } else {
            const std::uint32_t allowed = dnd_.source_actions ? dnd_.source_actions() : 0;
            dnd_.accept(std::string{kUriList}, ui::kDndCopy | (allowed & ui::kDndMove),
                        ui::kDndCopy);
        }
    }
    // Near the top or the bottom of the view, it scrolls.
    float notches = 0.0F;
    if (event.x >= view_area_.x && event.x < view_area_.right()) {
        if (event.y >= view_area_.y && event.y < view_area_.y + kAutoScrollEdge) {
            notches = -0.3F;
        } else if (event.y < view_area_.bottom() &&
                   event.y >= view_area_.bottom() - kAutoScrollEdge) {
            notches = 0.3F;
        }
    }
    auto_scroll_ = notches;
    if (notches == 0.0F) {
        stop_auto_scroll();
    } else if (auto_scroll_timer_ == 0) {
        auto_scroll_timer_ =
            loop_.add_timer(std::chrono::milliseconds{0}, std::chrono::milliseconds{40},
                            [this] { scroll_near_edge(); });
    }
}

void Browser::scroll_near_edge() {
    if (auto_scroll_ == 0.0F) {
        stop_auto_scroll();
        return;
    }
    view_pointer({.kind = ui::PointerEvent::Kind::scroll,
                  .x = drop_x_,
                  .y = drop_y_,
                  .wheel_y = auto_scroll_},
                 Clock::now());
    redraw_();
}

void Browser::stop_auto_scroll() {
    auto_scroll_ = 0.0F;
    loop_.remove(auto_scroll_timer_);
    auto_scroll_timer_ = 0;
}

void Browser::drop_paths(const std::filesystem::path& folder,
                         std::vector<std::filesystem::path> paths, bool own, bool move) {
    if (fs::is_trash(folder)) {
        trash_paths(paths, Purpose::user);
        return;
    }
    if (const std::string why = refusal(folder, paths); !why.empty()) {
        show_message(why, false);
        return;
    }
    if (!own) {
        transfer_paths(folder, paths, move, std::nullopt);
        return;
    }
    const ui::Modifiers modifiers = dnd_.modifiers ? dnd_.modifiers() : ui::Modifiers{};
    if (modifiers.ctrl || modifiers.shift) {
        transfer_paths(folder, paths, modifiers.shift && !modifiers.ctrl, std::nullopt);
        return;
    }
    // On the same disk it moves, onto another it copies: which disk is a stat, on a worker.
    ++operations_;
    std::filesystem::path from = paths.front().parent_path();  // before paths moves away
    pool_.submit(
        core::Priority::visible,
        [folder, from = std::move(from)](const core::CancelToken& /*token*/) {
            struct stat to_stat{};
            struct stat from_stat{};
            return ::stat(folder.c_str(), &to_stat) == 0 && ::stat(from.c_str(), &from_stat) == 0 &&
                   to_stat.st_dev == from_stat.st_dev;
        },
        [this, alive = alive_, folder, paths = std::move(paths)](bool same_disk) {
            if (!*alive) {
                return;
            }
            --operations_;
            transfer_paths(folder, paths, same_disk, std::nullopt);
        });
}

void Browser::add_to_sidebar(std::vector<std::filesystem::path> folders) {
    std::erase_if(folders, [](const std::filesystem::path& path) { return fs::is_trash(path); });
    if (folders.empty()) {
        show_message("Already in the sidebar", false);
        return;
    }
    bookmark_changes_.push_back({.add = std::move(folders), .remove = std::nullopt});
    next_bookmark_change();
}

void Browser::remove_from_sidebar(const std::filesystem::path& folder) {
    bookmark_changes_.push_back({.add = {}, .remove = folder});
    next_bookmark_change();
}

void Browser::remove_remote_from_sidebar(const std::string& uri, const std::string& label) {
    bookmark_changes_.push_back(
        {.add = {}, .remove = std::nullopt, .remove_uri = uri, .remove_label = label});
    next_bookmark_change();
}

void Browser::next_bookmark_change() {
    if (bookmark_busy_ || bookmark_changes_.empty()) {
        return;
    }
    BookmarkChange change = std::move(bookmark_changes_.front());
    bookmark_changes_.pop_front();
    bookmark_busy_ = true;
    ++operations_;

    // What the change did, for the message.
    struct Outcome {
        std::vector<std::string> added;
        std::size_t present{0};
        std::size_t not_folders{0};
        std::string removed;
        std::string error;
    };

    pool_.submit(
        core::Priority::visible,
        [file = bookmarks_file_.value_or(fs::bookmarks_file(place_sources_)),
         change = std::move(change)](const core::CancelToken& /*token*/) {
            Outcome outcome;
            core::TextFile read = core::read_text_file(file);
            if (!read.error.empty()) {
                outcome.error = read.error;
                return outcome;
            }
            std::string text = std::move(read.text);
            bool changed = false;
            for (const std::filesystem::path& folder : change.add) {
                std::error_code ignored;
                if (!std::filesystem::is_directory(folder, ignored)) {
                    ++outcome.not_folders;
                } else if (std::optional<std::string> more = fs::add_bookmark(text, folder)) {
                    text = std::move(*more);
                    changed = true;
                    std::filesystem::path normal = folder.lexically_normal();
                    if (!normal.has_filename()) {
                        normal = normal.parent_path();  // a trailing slash
                    }
                    outcome.added.push_back(normal.filename().string());
                } else {
                    ++outcome.present;
                }
            }
            if (change.remove) {
                if (std::optional<std::string> less = fs::remove_bookmark(text, *change.remove)) {
                    text = std::move(*less);
                    changed = true;
                    outcome.removed = change.remove->filename().string();
                }
            }
            if (change.remove_uri) {
                if (std::optional<std::string> less =
                        fs::remove_remote_bookmark(text, *change.remove_uri)) {
                    text = std::move(*less);
                    changed = true;
                    outcome.removed = change.remove_label;
                }
            }
            if (changed) {
                if (const std::error_code error = core::write_file_atomically(file, text)) {
                    outcome.error = error.message();
                    outcome.added.clear();
                    outcome.removed.clear();
                }
            }
            return outcome;
        },
        [this, alive = alive_](const Outcome& outcome) {
            if (!*alive) {
                return;
            }
            --operations_;
            bookmark_busy_ = false;
            if (!outcome.error.empty()) {
                show_message(std::format("The sidebar could not be changed: {}", outcome.error),
                             true);
            } else if (outcome.added.size() == 1) {
                show_message(std::format("“{}” added to the sidebar", outcome.added.front()),
                             false);
            } else if (!outcome.added.empty()) {
                show_message(std::format("{} folders added to the sidebar", outcome.added.size()),
                             false);
            } else if (!outcome.removed.empty()) {
                show_message(std::format("“{}” removed from the sidebar", outcome.removed), false);
            } else if (outcome.present > 0) {
                show_message("Already in the sidebar", false);
            } else if (outcome.not_folders > 0) {
                show_message("Only folders go in the sidebar", false);
            }
            if ((!outcome.added.empty() || !outcome.removed.empty()) && places_reload_) {
                places_reload_();
            }
            next_bookmark_change();
        });
}

void Browser::open_place(const fs::Place& place) {
    if (!place.uri.empty() && !place.disk.mounted) {
        mount_remote(place, true);
        return;
    }
    if (!place.disk.mounted) {
        mount_place(place, true);
        return;
    }
    open(place.path);
}

void Browser::mount_place(const fs::Place& place, bool open) {
    if (!devices_.mount || place.disk.volume.empty()) {
        return;
    }
    show_message(std::format("Mounting “{}”…", place.label), false);
    ++operations_;
    devices_.mount(
        place.disk.volume, [this, alive = alive_, name = place.label, open](
                               const std::filesystem::path& where, const std::string& error) {
            if (!*alive) {
                return;
            }
            --operations_;
            if (!error.empty()) {
                show_message(std::format("“{}” could not be mounted: {}", name, error), true);
                return;
            }
            if (open) {
                this->open(where);
            }
            // A disk mounted read-only says why: NTFS left unclean by Windows or by a pull.
            ++operations_;
            pool_.submit(
                core::Priority::visible,
                [where](const core::CancelToken& /*token*/) {
                    struct statvfs info{};
                    return ::statvfs(where.c_str(), &info) == 0 && (info.f_flag & ST_RDONLY) != 0;
                },
                [this, alive, name](bool read_only) {
                    if (!*alive) {
                        return;
                    }
                    --operations_;
                    show_message(read_only ? std::format("“{}” is read-only: it was not closed "
                                                         "properly (Windows' chkdsk or ntfsfix -d "
                                                         "fixes it)",
                                                         name)
                                           : std::format("“{}” mounted", name),
                                 read_only);
                });
        });
}

void Browser::mount_remote(const fs::Place& place, bool open) {
    if (place.uri.empty()) {
        return;
    }
    if (!remotes_.mount) {
        show_message(std::format("“{}” could not be mounted: gvfs is not available", place.label),
                     true);
        return;
    }
    show_message(std::format("Connecting to “{}”…", place.label), false);
    ++operations_;
    Remotes::Prompts prompts{
        .login =
            [this, alive = alive_, name = place.label](
                const LoginRequest& request,
                const std::function<void(std::optional<Login>)>& answer) {
                if (!*alive) {
                    answer(std::nullopt);
                    return;
                }
                password_.open(std::format("Connect to “{}”", name), request, answer);
            },
        .question =
            [this, alive = alive_](const std::string& message,
                                   const std::vector<std::string>& choices,
                                   const std::function<void(std::optional<std::size_t>)>& answer) {
                if (!*alive || choices.empty()) {
                    answer(std::nullopt);
                    return;
                }
                // The first line is the question, the rest says why.
                const std::size_t line = message.find('\n');
                confirm_.open(message.substr(0, line),
                              line == std::string::npos ? std::string{} : message.substr(line + 1),
                              choices.front(), [answer](bool confirmed) {
                                  answer(confirmed ? std::optional<std::size_t>{0} : std::nullopt);
                              });
            },
        .cancel =
            [this, alive = alive_] {
                if (*alive) {
                    password_.close();
                    confirm_.close();
                }
            },
    };
    remotes_.mount(place.uri, std::move(prompts),
                   [this, alive = alive_, name = place.label, open](
                       const std::filesystem::path& where, const std::string& error) {
                       if (!*alive) {
                           return;
                       }
                       --operations_;
                       password_.close();
                       if (error == "cancelled") {
                           show_message(std::format("Not connected to “{}”", name), false);
                           return;
                       }
                       if (!error.empty()) {
                           show_message(std::format("“{}” could not be mounted: {}", name, error),
                                        true);
                           return;
                       }
                       if (open) {
                           this->open(where);
                       }
                       show_message(std::format("“{}” mounted", name), false);
                   });
}

void Browser::unmount_remote(const fs::Place& place) {
    if (!remotes_.unmount || place.uri.empty()) {
        return;
    }
    leave_disk(place.path);
    ++operations_;
    remotes_.unmount(place.uri, [this, alive = alive_,
                                 name = place.label](const std::string& error) {
        if (!*alive) {
            return;
        }
        --operations_;
        if (error == "in use") {
            show_message(std::format("“{}” is in use: close the files open from it", name), true);
        } else if (!error.empty()) {
            show_message(std::format("“{}” could not be unmounted: {}", name, error), true);
        } else {
            show_message(std::format("“{}” unmounted", name), false);
        }
    });
}

void Browser::leave_disk(const std::filesystem::path& mount_point) {
    const std::shared_ptr<Folder> shown = current_folder();
    if (!shown || mount_point.empty()) {
        return;
    }
    const std::filesystem::path inside = shown->path().lexically_relative(mount_point);
    if (!inside.empty() && *inside.begin() != "..") {
        const std::filesystem::path home = place_sources_.home;
        open(home.empty() ? std::filesystem::path{"/"} : home);
    }
}

void Browser::unmount_place(const fs::Place& place) {
    if (!devices_.unmount || place.disk.volume.empty()) {
        return;
    }
    leave_disk(place.path);
    ++operations_;
    devices_.unmount(place.disk.volume, [this, alive = alive_,
                                         name = place.label](const std::string& error) {
        if (!*alive) {
            return;
        }
        --operations_;
        if (error == "in use") {
            show_message(std::format("“{}” is in use: close the files open from it", name), true);
        } else if (!error.empty()) {
            show_message(std::format("“{}” could not be unmounted: {}", name, error), true);
        } else {
            show_message(std::format("“{}” unmounted", name), false);
        }
    });
}

void Browser::eject_place(const fs::Place& place) {
    if (!place.uri.empty()) {
        unmount_remote(place);  // a network folder's eject icon
        return;
    }
    if (!devices_.eject || place.disk.volume.empty()) {
        return;
    }
    leave_disk(place.path);
    show_message(std::format("Ejecting “{}”…", place.label), false);
    ++operations_;
    devices_.eject(place.disk.volume, [this, alive = alive_,
                                       name = place.label](const std::string& error) {
        if (!*alive) {
            return;
        }
        --operations_;
        if (error == "in use") {
            show_message(std::format("“{}” is in use: close the files open from it", name), true);
        } else if (!error.empty()) {
            show_message(std::format("“{}” could not be ejected: {}", name, error), true);
        } else {
            show_message(std::format("“{}” can be unplugged now", name), false);
        }
    });
}

void Browser::view_pointer(const ui::PointerEvent& event, Clock::time_point now) {
    switch (pane().mode) {
        case Mode::columns:
            pane().columns.pointer(event, now);
            break;
        case Mode::grid:
            pane().grid.pointer(event, now);
            break;
        case Mode::details:
            pane().details.pointer(event, now);
            break;
    }
}

std::shared_ptr<Folder> Browser::current_folder() const { return folder_of(pane()); }

bool Browser::key(const ui::KeyEvent& event, Clock::time_point now) {
    note_location();  // the cursor of the folder the key may leave
    const bool handled = take_key(event, now);
    note_location();
    return handled;
}

bool Browser::take_key(const ui::KeyEvent& event, Clock::time_point now) {
    if (drag_ && event.keysym == XKB_KEY_Escape && event.modifiers == ui::Modifiers{}) {
        if (dnd_.cancel) {
            dnd_.cancel();
        }
        drag_ended();
        return true;
    }
    if (conflict_.is_open()) {
        return conflict_.key(event);  // a copy waits for the answer: every key is the card's
    }
    if (confirm_.is_open()) {
        return confirm_.key(event);
    }
    if (password_.is_open()) {
        if (event.modifiers == ui::Modifiers{.ctrl = true} &&
            (event.keysym == XKB_KEY_v || event.keysym == XKB_KEY_V)) {
            paste_text();
            return true;
        }
        return password_.key(event);  // every key is the card's meanwhile
    }
    found_after_.reset();  // a key while a file found is on its way: it stays shown, no more
    if (palette_.is_open()) {
        const Command command = command_for(event);
        if (command == Command::command_palette || command == Command::find_files) {
            // The same key closes it; the other switches.
            const bool files = palette_.external();
            palette_.close();
            if (command == Command::find_files && !files) {
                open_finder();
            } else if (command == Command::command_palette && files) {
                open_palette();
            }
        } else if (event.modifiers == ui::Modifiers{.ctrl = true} &&
                   (event.keysym == XKB_KEY_v || event.keysym == XKB_KEY_V)) {
            paste_text();
        } else {
            palette_.key(event);  // every key is the palette's meanwhile
        }
        return true;
    }
    if (menu_.is_open()) {
        return menu_.key(event);  // every key is the menu's meanwhile
    }
    if (editor_.active()) {
        if (event.modifiers == ui::Modifiers{.ctrl = true} &&
            (event.keysym == XKB_KEY_v || event.keysym == XKB_KEY_V)) {
            paste_text();
            return true;
        }
        return editor_.key(event);  // every key is the name's meanwhile
    }
    const Command command = command_for(event);
    if (event.keysym == XKB_KEY_Escape && event.modifiers == ui::Modifiers{} &&
        !cut_marks_.names.empty() && !shortcuts_.is_open() && !quick_look_.is_open() &&
        !sidebar_.focused()) {
        clipboard_ = {};  // Escape keeps cut entries where they are; the view gets it too
        cut_marks_ = {};
        redraw_();
        view_key(event, now);
        return true;
    }
    if (shortcuts_.is_open()) {
        if (command == Command::show_shortcuts ||
            (event.keysym == XKB_KEY_Escape && event.modifiers == ui::Modifiers{})) {
            shortcuts_.close();
        }
        return true;  // nothing under them reacts meanwhile
    }
    if (command == Command::select_tab) {
        // Alt+1…9: the keysyms 1 to 9 follow each other.
        select_tab(static_cast<std::size_t>(event.keysym - XKB_KEY_1));
        return true;
    }
    if (quick_look_.is_open()) {
        if (command == Command::quick_look ||
            (event.keysym == XKB_KEY_Escape && event.modifiers == ui::Modifiers{})) {
            quick_look_.close();
            return true;
        }
        // On a text page Home, End, Page Up and Page Down move through it; Ctrl+A selects its
        // text and Ctrl+C copies it (with nothing selected, Ctrl+C copies the file, as without
        // Quick Look).
        if (quick_look_.text_key(event, now)) {
            return true;
        }
        if (quick_look_.shows_text()) {
            if ((event.keysym == XKB_KEY_a || event.keysym == XKB_KEY_A) &&
                event.modifiers == ui::Modifiers{.ctrl = true}) {
                quick_look_.select_all_text();
                return true;
            }
            if (command == Command::copy) {
                if (std::string text = quick_look_.selected_text(); !text.empty()) {
                    copy_text(std::move(text));
                    return true;
                }
            }
        }
        if (command != Command::none) {
            run(command, now);
            return true;
        }
        // The rest moves the view underneath; Quick Look shows where it lands.
        const bool handled = view_key(event, now);
        quick_look_.follow(current_folder());
        return handled;
    }
    // What is not listed for the sidebar is nothing there (Space: Quick Look shows the view's
    // entries; F2: the sidebar's places are not renamed).
    const bool ignored = sidebar_.focused() && !listed(event, scope::kSidebar);
    if (command != Command::none && !ignored) {
        run(command, now);
        return true;
    }
    if (sidebar_.focused()) {
        // The view does not see keys meanwhile.
        return listed(event, scope::kSidebar) && sidebar_.key(event);
    }
    return view_key(event, now);
}

void Browser::run(Command command, Clock::time_point now) {
    if (const std::shared_ptr<Folder> shown = current_folder(); shown && shown->is_trash()) {
        switch (command) {
            case Command::rename:
            case Command::new_folder:
            case Command::copy:
            case Command::cut:
            case Command::paste:
                show_message("The trash only gives back: right click, Restore", false);
                return;
            case Command::trash:
                erase();  // from the trash, only for good
                return;
            default:
                break;
        }
    }
    switch (command) {
        case Command::none:
            break;
        case Command::columns_view:
            set_mode(Mode::columns);
            break;
        case Command::grid_view:
            set_mode(Mode::grid);
            break;
        case Command::details_view:
            set_mode(Mode::details);
            break;
        case Command::toggle_info:
            set_info_visible(!info_visible_);
            break;
        case Command::toggle_sidebar:
            set_sidebar_visible(!sidebar_visible_);
            break;
        case Command::focus_sidebar:
            if (!sidebar_.focused()) {
                set_sidebar_visible(true);
            }
            sidebar_.set_focused(!sidebar_.focused());
            break;
        case Command::quick_look:
            if (quick_look_.is_open()) {
                quick_look_.close();
            } else {
                quick_look_.open(current_folder(), now);
            }
            break;
        case Command::show_shortcuts:
            shortcuts_.open();
            break;
        case Command::rename:
            quick_look_.close();
            start_rename();
            break;
        case Command::new_folder:
            quick_look_.close();
            new_folder();
            break;
        case Command::copy:
            take_selection(false);
            break;
        case Command::cut:
            take_selection(true);
            break;
        case Command::paste:
            quick_look_.close();
            paste();
            break;
        case Command::trash:
            trash();
            break;
        case Command::erase:
            erase();
            break;
        case Command::undo:
            undo();
            break;
        case Command::redo:
            redo();
            break;
        case Command::context_menu:
            open_menu_from_keyboard();
            break;
        case Command::back:
            back();
            break;
        case Command::forward:
            forward();
            break;
        case Command::toggle_hidden:
            set_show_hidden(!show_hidden_);
            break;
        case Command::new_tab:
            new_tab();
            break;
        case Command::close_tab:
            close_tab(active_);
            break;
        case Command::next_tab:
            select_tab((active_ + 1) % panes_.size());
            break;
        case Command::previous_tab:
            select_tab((active_ + panes_.size() - 1) % panes_.size());
            break;
        case Command::select_tab:
            break;  // take_key: it needs the digit
        case Command::add_bookmark:
            if (const std::shared_ptr<Folder> shown = current_folder()) {
                add_to_sidebar({shown->path()});
            }
            break;
        case Command::open_terminal:
            open_terminal();
            break;
        case Command::command_palette:
            open_palette();
            break;
        case Command::find_files:
            open_finder();
            break;
    }
}

bool Browser::open_selection() {
    const std::shared_ptr<Folder> folder = current_folder();
    const fs::Entry* const cursor = folder ? folder->cursor_entry() : nullptr;
    if (cursor == nullptr || cursor->is_directory()) {
        return false;
    }
    if (folder->is_trash()) {
        show_message("Restore it first to open it", false);
        return true;
    }
    if (!open_) {
        return false;
    }
    const auto with_type = [&folder](fs::Entry entry) {
        entry.mime = folder->mime_of(entry);
        return entry;
    };
    std::vector<fs::Entry> files;
    if (!folder->is_selected(cursor->name)) {
        files.push_back(with_type(*cursor));
    } else {
        for (const fs::Entry& entry : folder->entries()) {
            if (!entry.is_directory() && folder->is_selected(entry.name)) {
                files.push_back(with_type(entry));
            }
        }
    }
    open_(folder->path(), std::move(files));
    return true;
}

void Browser::open_terminal() {
    const std::shared_ptr<Folder> folder = current_folder();
    if (!folder || !terminal_) {
        return;
    }
    if (folder->is_trash()) {
        show_message("Restore it first to open it", false);
        return;
    }
    const fs::Entry* const cursor = folder->cursor_entry();
    terminal_(cursor != nullptr && cursor->is_directory() ? folder->path() / cursor->name
                                                          : folder->path());
}

bool Browser::view_key(const ui::KeyEvent& event, Clock::time_point now) {
    if ((event.keysym == XKB_KEY_Return || event.keysym == XKB_KEY_KP_Enter) &&
        event.modifiers == ui::Modifiers{} && open_selection()) {
        return true;  // a file: its application; a folder goes on to the view
    }
    // Only what the shortcuts list reaches the view.
    switch (pane().mode) {
        case Mode::columns:
            return listed(event, scope::kColumns) && pane().columns.key(event, now);
        case Mode::grid:
            return listed(event, scope::kGrid) && pane().grid.key(event, now);
        case Mode::details:
            return listed(event, scope::kDetails) && pane().details.key(event, now);
    }
    return false;
}

}  // namespace nordend::views
