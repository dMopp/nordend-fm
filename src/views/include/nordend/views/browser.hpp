#pragma once

#include <chrono>
#include <cstdint>
#include <deque>
#include <filesystem>
#include <functional>
#include <map>
#include <memory>
#include <optional>
#include <string>
#include <unordered_set>
#include <vector>

#include "nordend/core/event_loop.hpp"
#include "nordend/core/worker_pool.hpp"
#include "nordend/fs/directory_model.hpp"
#include "nordend/fs/directory_watcher.hpp"
#include "nordend/fs/mime.hpp"
#include "nordend/fs/places.hpp"
#include "nordend/fs/transfer_queue.hpp"
#include "nordend/theme/palette.hpp"
#include "nordend/ui/clipboard.hpp"
#include "nordend/ui/geometry.hpp"
#include "nordend/ui/icons.hpp"
#include "nordend/ui/renderer.hpp"
#include "nordend/ui/scroller.hpp"
#include "nordend/ui/seat.hpp"
#include "nordend/views/breadcrumb.hpp"
#include "nordend/views/column_view.hpp"
#include "nordend/views/command_palette.hpp"
#include "nordend/views/confirm_dialog.hpp"
#include "nordend/views/conflict_dialog.hpp"
#include "nordend/views/context_menu.hpp"
#include "nordend/views/cut_marks.hpp"
#include "nordend/views/details_view.hpp"
#include "nordend/views/drop_mark.hpp"
#include "nordend/views/file_clipboard.hpp"
#include "nordend/views/file_finder.hpp"
#include "nordend/views/grid_view.hpp"
#include "nordend/views/info_pane.hpp"
#include "nordend/views/name_editor.hpp"
#include "nordend/views/pane.hpp"
#include "nordend/views/password_dialog.hpp"
#include "nordend/views/quick_look.hpp"
#include "nordend/views/shortcuts.hpp"
#include "nordend/views/shortcuts_overlay.hpp"
#include "nordend/views/sidebar.hpp"
#include "nordend/views/state.hpp"
#include "nordend/views/status_bar.hpp"
#include "nordend/views/tab_bar.hpp"
#include "nordend/views/thumbnails.hpp"
#include "nordend/views/undo.hpp"

namespace nordend::views {

// The window's content: the column view, the grid or the details table, one at a time, and Quick
// Look over them (Space opens it on the entry under the cursor; Space, Escape or a click close it;
// the other keys move the view underneath). Ctrl+1 shows the columns, Ctrl+2 the grid, Ctrl+3
// the details. The info pane shows what is selected in all three: after the last column, at the
// right of the grid and the details, where Ctrl+I shows or hides it. The grid and the details show
// the folder of the focused column, and the columns come back with that folder focused: the same
// folder object passes between them, so the cursor, the selection and the types sniffed so far
// stay. Above the view, the breadcrumb shows the folder's path, every step a way up. The sidebar at
// the left lists the places (set_places); Ctrl+B shows or hides it, F6 moves the keyboard into it
// and back, and a place chosen there opens in the view shown. Enter or a double click on a file
// hands the selected files to the open handler (their default applications); on a folder, the
// view goes into it. F2 renames the entry under the cursor in place, Ctrl+Shift+N makes a new
// folder and edits its name; both on a worker, never replacing anything. Ctrl+C and Ctrl+X take
// the selection (cut entries fade until pasted; Escape keeps them), Ctrl+V copies or moves it
// into the folder shown, on the transfer thread, with progress in the status bar and a question
// on a card for every name taken. Delete moves the selection to the trash (freedesktop), on the
// same thread; the cursor goes on to the entry after it. Shift+Delete (Delete in the trash folder)
// deletes it for good and "Empty Trash" empties every trash, after a card that says how much goes.
// A right click (or the Menu key, Shift+F10)
// opens a context menu with the same actions, "Open With" and "New File". Alt+Left and Alt+Right
// (the mouse's back and forward buttons) go through the folders opened, Ctrl+H shows the hidden
// files. Entries are dragged out of any view (to Nordend or another program, which only copies
// them) and dropped onto a folder, a view, a step of the breadcrumb or a place of the sidebar.
//
// Drag and drop as the app connects it (Wayland's wl_data_device); tests give a fake. Without it
// nothing is dragged out and nothing dropped in.
struct DragAndDrop {
    using Format = SystemClipboard::Format;
    using Done = SystemClipboard::Done;

    // Starts dragging these formats from the button press still held; the app shows
    // Browser::draw_drag_icon() under the pointer.
    std::function<void(std::vector<Format> formats)> start;
    // Ends Nordend's drag without a drop (Escape).
    std::function<void()> cancel;
    // The drag over the window: its types, the actions its source allows (ui::kDndCopy,
    // ui::kDndMove) and the one agreed for the drop.
    std::function<std::vector<std::string>()> types;
    std::function<std::uint32_t()> source_actions;
    std::function<std::uint32_t()> action;
    // Where the pointer is: type taken (nullopt: nothing), with actions, preferred first.
    std::function<void(std::optional<std::string> type, std::uint32_t actions,
                       std::uint32_t preferred)>
        accept;
    // After a drop: reads one type of it, then it is done.
    std::function<void(std::string type, Done done)> read;
    // After a drop of Nordend's own drag: done with it unread.
    std::function<void()> finish;
    // The modifiers held now (Ctrl copies, Shift moves).
    std::function<ui::Modifiers()> modifiers;
};

// Disks as the app connects them (udisks2); tests give a fake. Without it nothing is mounted,
// unmounted or ejected from Nordend.
struct Devices {
    using Done = std::function<void(const std::string& error)>;  // empty: it worked
    using Mounted =
        std::function<void(const std::filesystem::path& where, const std::string& error)>;

    std::function<void(const std::string& volume, Mounted done)> mount;
    std::function<void(const std::string& volume, Done done)> unmount;
    // Every file system on the volume's drive unmounted, then the drive powered off.
    std::function<void(const std::string& volume, Done done)> eject;
};

// Network folders as the app connects them (gvfs): remote bookmarks mounted by their URI; tests
// give a fake. Without it a remote bookmark says it cannot be mounted.
struct Remotes {
    using Done = Devices::Done;
    using Mounted = Devices::Mounted;

    // What a mount asks the person; the Browser answers on its cards.
    struct Prompts {
        std::function<void(const LoginRequest& request,
                           std::function<void(std::optional<Login> login)> answer)>
            login;
        // choices[0] goes on; nullopt gives up.
        std::function<void(const std::string& message, const std::vector<std::string>& choices,
                           std::function<void(std::optional<std::size_t> choice)> answer)>
            question;
        std::function<void()> cancel;  // the server stopped waiting: the cards close
    };

    // An error of "cancelled": the person gave up, nothing to report.
    std::function<void(const std::string& uri, Prompts prompts, Mounted done)> mount;
    std::function<void(const std::string& uri, Done done)> unmount;
};

// Loop thread only. Must not outlive the loop, pool or watcher.
class Browser {
public:
    using Clock = ui::Scroller::Clock;

    using Mode = ViewMode;

    // thumbnails: the cache the grid takes its thumbnails from; nullptr shows icons only.
    Browser(core::EventLoop& loop, core::WorkerPool& pool, fs::DirectoryWatcher& watcher,
            std::shared_ptr<const fs::MimeDatabase> mime, std::function<void()> redraw,
            std::function<void(ui::CursorShape)> set_cursor,
            std::shared_ptr<const thumbs::Cache> thumbnails = nullptr);
    ~Browser();

    Browser(const Browser&) = delete;
    Browser& operator=(const Browser&) = delete;
    Browser(Browser&&) = delete;
    Browser& operator=(Browser&&) = delete;

    // directory, no cursor, in the current mode.
    void open(const std::filesystem::path& directory);

    // folder in the current mode with the cursor on child (a name; empty: none), as when going
    // up: the columns focus the column showing folder when there is one.
    void go_to(const std::filesystem::path& folder, const std::string& child);

    // The columns from root down to target (see ColumnView::reveal); switches to the columns.
    void reveal(const std::filesystem::path& root, const std::filesystem::path& target);

    void set_mode(Mode mode);

    using Step = HistoryStep;

    static constexpr std::size_t kHistorySize = 100;

    // Alt+Left, Alt+Right and the mouse's side buttons: the folder opened before this one, or
    // after it, with the cursor where it was. Every folder the keyboard goes into is a step
    // (in the columns, the focused column's); opening one after going back drops the steps
    // ahead. A folder that no longer exists is left out of the history, with a message.
    void back();
    void forward();

    [[nodiscard]] const std::vector<Step>& history() const noexcept { return pane().history; }

    // Where history() is now.
    [[nodiscard]] std::size_t history_index() const noexcept { return pane().here; }

    // Ctrl+H: dotfiles listed in every view, or left out (at start). Left out, they leave the
    // selection, so nothing acts on what is not seen, and the cursor moves off them; the hidden
    // folders on the way to the one shown stay.
    void set_show_hidden(bool show);

    [[nodiscard]] bool shows_hidden() const noexcept { return show_hidden_; }

    // The window's order, the same in every view and every folder they show: from the context
    // menu's "Sort By", or a click on a title of the details.
    void set_sort(fs::SortKey key, bool descending);

    // The tab shown's lists' rows (columns and details) and grid cells: Ctrl+plus/minus/0,
    // Ctrl+wheel (ROADMAP 8.1), each tab its own; a new tab takes the one it comes from. Indexes
    // into kListSizes and GridView::kIconSizes.
    void set_list_size(std::size_t index);
    void set_grid_size(std::size_t index);
    // The index of a grid icon size in pixels (the usual one when it is not one of them).
    [[nodiscard]] static std::size_t grid_index(std::int32_t pixels);

    [[nodiscard]] const fs::SortOrder& sort_order() const noexcept {
        return pane().details.order();
    }

    [[nodiscard]] Mode mode() const noexcept { return pane().mode; }

    // The info pane in the grid and the details, one setting for both (shown at start); the
    // columns always show it.
    void set_info_visible(bool visible);

    [[nodiscard]] bool info_visible() const noexcept { return info_visible_; }

    // The sidebar (shown at start; left out when the window is too narrow for it).
    void set_sidebar_visible(bool visible);

    // The pointer's shape over a window point, as last drawn: a hand over what a click acts on
    // (buttons, the path's crumbs, an eject icon, "More details"), the text cursor over a name
    // being edited, the arrow elsewhere. The resize arrows are the views' own.
    [[nodiscard]] ui::CursorShape cursor_at(float x, float y) const;

    // What is remembered from one start to the next (ROADMAP 7.5a); the window's part is main's.
    [[nodiscard]] State state() const;
    // Before open(): the view comes up in the remembered mode; afterwards, as if chosen.
    void apply(const State& state);
    // At a start without a folder, after apply() and instead of open(): the tabs as they were
    // left (State::tabs), the one shown then shown again; folders gone meanwhile are left out.
    // false when none is left: open() the start folder then.
    bool restore_tabs(const std::vector<State::Tab>& tabs, std::size_t shown);

    [[nodiscard]] bool sidebar_visible() const noexcept { return sidebar_visible_; }

    // What the sidebar lists.
    void set_places(std::vector<fs::Place> places);

    // Opens files (directory's entries, never folders, types filled in where known). Without
    // one, Enter on a file does nothing.
    using OpenHandler =
        std::function<void(const std::filesystem::path& directory, std::vector<fs::Entry> files)>;

    void set_open_handler(OpenHandler open) { open_ = std::move(open); }

    // An application "Open With" offers.
    struct AppChoice {
        std::string id;
        std::string name;
        bool is_default{false};
    };

    // The applications for entry, handed to done later on the loop thread; and opening files
    // with one of them. Without them the menu has no "Open With".
    using ChoicesHandler =
        std::function<void(const std::filesystem::path& directory, fs::Entry entry,
                           std::function<void(std::vector<AppChoice>)> done)>;
    using OpenWithHandler = std::function<void(const std::filesystem::path& directory,
                                               std::vector<fs::Entry> files, std::string app_id)>;

    void set_open_with_handlers(ChoicesHandler choices, OpenWithHandler open_with) {
        choices_ = std::move(choices);
        open_with_ = std::move(open_with);
    }

    // Opens a terminal in a folder (F4, "Open in Terminal"). Without one, neither is there.
    using TerminalHandler = std::function<void(const std::filesystem::path& directory)>;

    void set_terminal_handler(TerminalHandler terminal) { terminal_ = std::move(terminal); }

    // Where "New File" takes its templates from: fs::templates_dir() unless set (tests).
    void set_templates_dir(std::filesystem::path directory) {
        templates_dir_ = std::move(directory);
    }

    // A line in the status bar for a few seconds (what opening a file did).
    void show_message(std::string text, bool error) {
        status_bar_.show_message(std::move(text), error);
    }

    void draw(ui::Renderer& renderer, const ui::Rect& bounds, const theme::Palette& palette,
              ui::IconSet& icons, Clock::time_point now);

    [[nodiscard]] bool animating() const;

    void pointer(const ui::PointerEvent& event, Clock::time_point now);
    // false when event did nothing: no shortcut (shortcuts()) where the keyboard is.
    bool key(const ui::KeyEvent& event, Clock::time_point now);

    // The view shown is still loading, its thumbnails are being made, or a new folder or a rename
    // is on its way (snapshots, tests).
    [[nodiscard]] bool loading() const;

    // Tabs (ROADMAP 9.1), a pane each; the bar above the path shows them while there are two or
    // more. Ctrl+T: a new tab on the folder shown, in the same view, shown at once.
    void new_tab();
    // A new tab on directory, right after the one shown; shown, or left behind it (a middle
    // click on a folder).
    void open_tab(const std::filesystem::path& directory, bool show);
    // "Open in New Tab(s)": folders behind the tab shown, in their order.
    void open_tabs(const std::vector<std::filesystem::path>& folders);
    // Ctrl+W, a tab's ×, a middle click on it: the tab closes; the last one stays.
    void close_tab(std::size_t index);
    // A click on a tab, Ctrl+Tab, Ctrl+Shift+Tab, Alt+1…9.
    void select_tab(std::size_t index);

    [[nodiscard]] std::size_t tab_count() const noexcept { return panes_.size(); }

    [[nodiscard]] std::size_t active_tab() const noexcept { return active_; }

    // Each tab's name: its folder's (Home, Trash, / for the root).
    [[nodiscard]] std::vector<std::string> tab_titles() const;

    [[nodiscard]] TabBar& tab_bar() noexcept { return tab_bar_; }

    // The pane shown (a tab, ROADMAP 9.1): its views, mode and history.
    [[nodiscard]] Pane& pane() noexcept { return *panes_[active_]; }

    [[nodiscard]] const Pane& pane() const noexcept { return *panes_[active_]; }

    [[nodiscard]] ColumnView& columns() noexcept { return pane().columns; }

    [[nodiscard]] GridView& grid() noexcept { return pane().grid; }

    [[nodiscard]] DetailsView& details() noexcept { return pane().details; }

    [[nodiscard]] QuickLook& quick_look() noexcept { return quick_look_; }

    [[nodiscard]] InfoPane& info_pane() noexcept { return info_pane_; }

    [[nodiscard]] Sidebar& sidebar() noexcept { return sidebar_; }

    [[nodiscard]] Breadcrumb& breadcrumb() noexcept { return breadcrumb_; }

    [[nodiscard]] StatusBar& status_bar() noexcept { return status_bar_; }

    [[nodiscard]] ShortcutsOverlay& shortcuts_overlay() noexcept { return shortcuts_; }

    [[nodiscard]] NameEditor& name_editor() noexcept { return editor_; }

    // F2: edits the name of the entry under the cursor (nothing without one).
    void start_rename();

    // Ctrl+Shift+N: makes "New Folder" (or "New Folder 2"...) in the folder shown, puts the cursor
    // on it and edits its name.
    void new_folder();

    // Ctrl+C, Ctrl+X: the selection (or the entry under the cursor) is what Ctrl+V will copy or
    // move.
    void take_selection(bool cut);

    // Ctrl+C on Quick Look's text page: text on the clipboard (no files any more).
    void copy_text(std::string text);

    // Ctrl+V: copies or moves what was taken into the folder shown; when another program holds
    // the clipboard, the files it offers (x-special/gnome-copied-files, else text/uri-list).
    void paste();

    // The system clipboard: Ctrl+C and Ctrl+X offer the files there too, Ctrl+V reads it, and
    // Ctrl+V in a name's field pastes its text.
    void set_system_clipboard(SystemClipboard clipboard) { system_ = std::move(clipboard); }

    // Another program took the clipboard: what Ctrl+C or Ctrl+X took is forgotten.
    void clipboard_lost();

    // Drag and drop: a press on an entry moved this far starts a drag of it (of the selection,
    // when it is selected).
    static constexpr float kDragThreshold = 6.0F;
    // The image under the pointer while dragging, in logical pixels.
    static constexpr std::int32_t kDragIconWidth = 220;
    static constexpr std::int32_t kDragIconHeight = 40;
    // How close to the top or bottom of a view a drag scrolls it.
    static constexpr float kAutoScrollEdge = 40.0F;

    void set_drag_and_drop(DragAndDrop dnd) { dnd_ = std::move(dnd); }

    // A drag over the window, from Nordend or another program.
    void drop_event(const ui::DropEvent& event);

    // Nordend's drag ended (dropped somewhere or not).
    void drag_ended();

    // Nordend is dragging entries.
    [[nodiscard]] bool dragging() const noexcept { return drag_.has_value(); }

    // A press on empty space (or on an entry's row away from its icon and name) moved past
    // kDragThreshold draws a rubber band: what it touches is selected, with Ctrl added to what
    // was selected. Beyond the top or bottom of its list the list scrolls under it.
    [[nodiscard]] bool banding() const noexcept { return band_.has_value(); }

    // The image under the pointer: the first entry's icon and name, or how many there are.
    void draw_drag_icon(ui::Renderer& renderer, const ui::Rect& bounds,
                        const theme::Palette& palette, ui::IconSet& icons) const;

    // Ctrl+D, "Add to Sidebar" and folders dropped by the bookmarks: added to the GTK bookmarks
    // (the sidebar's, and other programs' file choosers), on a worker, one change at a time.
    // Folders already there and anything not a folder are left out, with a message.
    void add_to_sidebar(std::vector<std::filesystem::path> folders);

    // "Remove from Sidebar" on a bookmark.
    void remove_from_sidebar(const std::filesystem::path& folder);
    // A remote bookmark's line goes from the bookmarks file; label names it in the message.
    void remove_remote_from_sidebar(const std::string& uri, const std::string& label);

    // The bookmarks file: fs::bookmarks_file() unless set (tests).
    void set_bookmarks_file(std::filesystem::path file) { bookmarks_file_ = std::move(file); }

    // Runs once the bookmarks file was written: the app reads the places again.
    void set_places_reload(std::function<void()> reload) { places_reload_ = std::move(reload); }

    void set_devices(Devices devices) { devices_ = std::move(devices); }

    void set_remotes(Remotes remotes) { remotes_ = std::move(remotes); }

    // A place chosen in the sidebar: opened, after mounting a disk or a network folder that is
    // not mounted.
    void open_place(const fs::Place& place);

    // A disk's place: mounted (then opened, when open), unmounted, or ejected (unmounted and
    // powered off, the view going home first when it shows that disk), with a message.
    void mount_place(const fs::Place& place, bool open);
    void unmount_place(const fs::Place& place);
    void eject_place(const fs::Place& place);

    // A remote bookmark's place: mounted through gvfs (asking for a password on a card when the
    // server wants one; then opened, when open), or unmounted, the view leaving it first.
    void mount_remote(const fs::Place& place, bool open);
    void unmount_remote(const fs::Place& place);

    // Where a drop would go now, as the views mark it.
    [[nodiscard]] const DropMark& drop_mark() const noexcept { return drop_mark_; }

    // Delete: moves the selection (or the entry under the cursor) to the trash, and the cursor to
    // the entry after it.
    void trash();

    // Where Delete puts what is on the home trash's disk: fs::home_trash() unless set (tests).
    void set_home_trash(std::filesystem::path trash) { home_trash_ = std::move(trash); }

    // The context menu at (x, y): the selection's when there is one, else the folder's. From the
    // keyboard (Menu, Shift+F10), by the cursor's entry.
    void open_menu(float x, float y, bool keyboard);
    void open_menu_from_keyboard();

    [[nodiscard]] ContextMenu& context_menu() noexcept { return menu_; }

    // Ctrl+K (ROADMAP 8a): the command palette with palette_items(), the recently used first.
    void open_palette();

    // What the palette offers now: every command that can be used here (the shortcuts' and the
    // menus'), each going the way its key or menu item goes; the sidebar's places ("Go to …");
    // the other tabs ("Tab 2: …").
    [[nodiscard]] std::vector<PaletteItem> palette_items();

    [[nodiscard]] CommandPalette& command_palette() noexcept { return palette_; }

    // The palette's recently used items, most recent first (state.toml).
    [[nodiscard]] const std::vector<std::string>& palette_recent() const noexcept {
        return palette_recent_;
    }

    // Ctrl+F (ROADMAP 8b): the palette finds files under the folder shown, matched by their paths.
    void open_finder();

    // While finding files: the finder (tests); nullptr otherwise.
    [[nodiscard]] FileFinder* file_finder() noexcept { return finder_.get(); }

    // What happens once a file found is shown: nothing more, Quick Look, or its application.
    enum class FoundAction : std::uint8_t { show, quick_look, open };

    // Shows root/relative in the view: the columns unfold down to it (from their first folder
    // when it is inside it), the grid and the details open its folder; the cursor on it. Then
    // after, once it is there (settle_found()).
    void show_found(const std::filesystem::path& root, const std::string& relative,
                    FoundAction after);

    // Runs show_found()'s action once its folder is shown with the cursor on the file; draw()
    // calls it, tests too. A key or a click meanwhile forgets it.
    void settle_found(Clock::time_point now);

    // Quits Nordend (the palette's "Quit"; Ctrl+Q is the app's). Without one, no "Quit".
    void set_quit_handler(std::function<void()> quit) { quit_ = std::move(quit); }

    // "New File": makes name (from a template when from is set) in the folder shown, puts the
    // cursor on it and edits its name.
    void new_file(const std::string& name, const std::filesystem::path& from);

    // Shift+Delete, and Delete in the trash folder: asks on a card (how many items, how many MB),
    // then deletes the selection (or the entry under the cursor) for good with its darktable
    // sidecars, on the transfer thread; the cursor goes on to the entry after it.
    void erase();

    // "Empty Trash": asks on a card, then deletes everything in every trash for good.
    void empty_trash();

    [[nodiscard]] ConfirmDialog& confirm_dialog() noexcept { return confirm_; }

    [[nodiscard]] PasswordDialog& password_dialog() noexcept { return password_; }

    // In the trash folder: puts the selection (or the entry under the cursor) back where it was,
    // with its darktable sidecars, and the cursor on the entry after it.
    void restore();

    // Ctrl+Z: undoes the last operation on files (a rename, a new folder or file, a copy, a move,
    // a trashing, a restore) by doing its inverse: copies and new entries go to the trash, never
    // deleted. Ctrl+Shift+Z: undoes the undo. Only the last one, while Nordend runs.
    void undo();
    void redo();

    // What Ctrl+Z would undo, and Ctrl+Shift+Z redo.
    [[nodiscard]] const std::optional<FileOperation>& last_operation() const noexcept {
        return undo_;
    }

    [[nodiscard]] const std::optional<FileOperation>& undone_operation() const noexcept {
        return redo_;
    }

    // Stops the transfer running (the status bar's ×).
    void cancel_transfer();

    [[nodiscard]] ConflictDialog& conflict_dialog() noexcept { return conflict_; }

    [[nodiscard]] const CutMarks& cut_marks() const noexcept { return cut_marks_; }

    [[nodiscard]] std::size_t transfers_pending() const noexcept { return transfers_.pending(); }

    // What the views hold, as lines for the memory report (ROADMAP 6.3b).
    [[nodiscard]] std::vector<std::string> memory_lines() const;

private:
    // Where a drop at (x, y) goes: a folder, and how it is marked.
    struct DropTarget {
        std::filesystem::path folder;
        DropMark mark;                               // in a view
        std::optional<std::filesystem::path> place;  // in the sidebar
        std::optional<std::filesystem::path> crumb;  // in the breadcrumb
        bool bookmark{false};  // the sidebar's bookmarks: added there, nothing moves
    };

    // A change to the bookmarks file, waiting its turn.
    struct BookmarkChange {
        std::vector<std::filesystem::path> add;
        std::optional<std::filesystem::path> remove;
        std::optional<std::string> remove_uri{};  // a remote bookmark
        std::string remove_label{};               // its name, for the message
    };

    void next_bookmark_change();

    [[nodiscard]] std::optional<DropTarget> drop_target_at(float x, float y) const;
    // What the view shown has at (x, y).
    [[nodiscard]] std::optional<ViewHit> view_hit(float x, float y) const;
    // Why paths cannot go into folder; empty when they can (into the trash, always).
    [[nodiscard]] static std::string refusal(const std::filesystem::path& folder,
                                             const std::vector<std::filesystem::path>& paths);
    void show_drop_target(const std::optional<DropTarget>& target);
    struct Press;
    // Starts Nordend's drag from press.
    void start_drag(Press press);
    // Drops paths into folder: own (Nordend's drag: Ctrl copies, Shift moves, else the same disk
    // moves), or another program's with the action agreed.
    void drop_paths(const std::filesystem::path& folder, std::vector<std::filesystem::path> paths,
                    bool own, bool move);
    // Copies or moves paths into destination as one operation.
    void transfer_paths(const std::filesystem::path& destination,
                        const std::vector<std::filesystem::path>& paths, bool move,
                        std::optional<std::uint64_t> empty_clipboard);
    void scroll_near_edge();
    // The rubber band: where it runs in the view shown, the selection it makes, the list
    // scrolled under it (on a timer, while the pointer is beyond an edge), the band drawn.
    [[nodiscard]] std::optional<BandLane> band_lane(const Folder& folder) const;
    void start_band(float x, float y);
    void update_band();
    void end_band();
    void scroll_band();
    void draw_band(ui::Renderer& renderer, const theme::Palette& palette) const;
    void stop_auto_scroll();
    // The folder shown into the history: a new step when it changed, else its cursor.
    void note_location();
    // Goes through the history by direction (-1 back, 1 forward).
    void travel(int direction);
    void trash_done(const fs::TransferResult& result);
    void restore_done(const fs::TransferResult& result);
    // Pastes files read from the system clipboard (at generation) into destination.
    void paste_files(const std::filesystem::path& destination, const ClipboardFiles& files,
                     std::uint64_t generation);
    // Why an operation runs: the user's, or an undo or redo of one.
    enum class Purpose : std::uint8_t { user, undo, redo };

    // A new batch: the transfers of one operation, recorded as one when the last is done.
    std::uint64_t open_batch(Purpose purpose, FileOperation::Kind kind);
    // Every transfer goes through here; batch 0: not recorded (a permanent delete).
    // empty_clipboard: another program's cut, read from the clipboard at that generation:
    // emptied once something arrived, unless it changed meanwhile.
    void submit(fs::TransferRequest request, std::uint64_t batch,
                std::optional<std::uint64_t> empty_clipboard = std::nullopt);
    // An operation done: what Ctrl+Z undoes now (or, for an undo, what Ctrl+Shift+Z redoes).
    void record(FileOperation operation, Purpose purpose, bool failed);
    // Starts the inverse of operation, for an undo or a redo.
    void start_inverse(const FileOperation& operation, Purpose purpose);
    // Moves paths to the trash as one operation of purpose.
    void trash_paths(const std::vector<std::filesystem::path>& paths, Purpose purpose);
    void report_done(const fs::TransferRequest& request, const fs::TransferResult& result);
    // Ctrl+V in the name's field: the clipboard's text.
    void paste_text();
    // "Open With…" in the palette: the palette again, with the applications for the cursor's entry.
    void open_with_palette();
    // The palette's list from the finder's results.
    void refresh_found();
    void erase_done(const fs::TransferRequest& request, const fs::TransferResult& result);
    // Opens the card for request (erase or empty_trash), adds up what it removes on a worker and
    // submits it once confirmed and added up; after runs when it is confirmed.
    void ask_erase(fs::TransferRequest request, std::string title, std::function<void()> after);
    // The delete confirmed and added up goes to the transfer thread.
    void submit_erase();
    // The sidebar's menu on a place: the trash's has "Empty Trash".
    void place_menu(const fs::Place& place, float x, float y);
    // The sidebar's places, the trash's icon full or empty.
    void show_places();
    // Puts the cursor on the entry after the last one of going that stays (else the one before).
    static void cursor_after(Folder& folder, const std::unordered_set<std::string>& going);
    // The folder the keyboard is in: the focused column's, or the grid's.
    [[nodiscard]] std::shared_ptr<Folder> current_folder() const;
    void run(Command command, Clock::time_point now);
    // Opens the selected files when the cursor is on a file (only the cursor's when it is not
    // selected); false when the cursor is on a folder or nowhere.
    bool open_selection();
    // F4: a terminal in the cursor's folder, else in the folder shown.
    void open_terminal();
    void rename(const std::shared_ptr<Folder>& folder, const std::string& from,
                const std::string& to);
    void transfer_progress(const fs::TransferRequest& request,
                           const fs::TransferProgress& progress);
    void transfer_done(const fs::TransferRequest& request, const fs::TransferResult& result);
    void ask(const fs::Conflict& conflict, std::function<void(fs::Resolution)> answer);
    bool take_key(const ui::KeyEvent& event, Clock::time_point now);
    void take_pointer(const ui::PointerEvent& event, Clock::time_point now);
    bool view_key(const ui::KeyEvent& event, Clock::time_point now);
    void dispatch_pointer(const ui::PointerEvent& event, Clock::time_point now);
    // The callback the views and the sidebar set their resize arrows through.
    [[nodiscard]] std::function<void(ui::CursorShape)> resize_cursor();
    void update_cursor();
    void view_pointer(const ui::PointerEvent& event, Clock::time_point now);

    // Who gets the pointer: the view, the sidebar or the breadcrumb above the view.
    enum class Target : std::uint8_t { view, sidebar, breadcrumb, status_bar, tabs };

    void send_pointer(Target target, const ui::PointerEvent& event, Clock::time_point now);
    void draw_view(ui::Renderer& renderer, const ui::Rect& bounds, const theme::Palette& palette,
                   ui::IconSet& icons, Clock::time_point now);

    // The names a command acts on: the selection, else the cursor's entry.
    [[nodiscard]] static std::vector<std::string> targets(const Folder& folder);
    [[nodiscard]] std::vector<MenuItem> entry_menu(const std::shared_ptr<Folder>& folder);
    [[nodiscard]] std::vector<MenuItem> folder_menu();
    [[nodiscard]] MenuItem sort_menu();
    // A new pane at index at, its views wired to the window, sorted and filtered as the window is.
    Pane& add_pane(std::size_t at);
    // A new tab on directory right after the one shown, which stays shown.
    void open_tab_behind(const std::filesystem::path& directory);
    // Before another pane is shown: what belongs to the one shown (Quick Look, a menu, a name
    // being edited, a press, a band, the pointer's hover) is let go.
    void leave_pane();
    // After: the view shown finds the pointer where it is.
    void enter_pane();
    // The folder a pane's view shows (the focused column's).
    [[nodiscard]] static std::shared_ptr<Folder> folder_of(const Pane& pane);

    core::EventLoop& loop_;
    core::WorkerPool& pool_;
    std::function<void()> redraw_;
    std::function<void(ui::CursorShape)> set_cursor_;
    OpenHandler open_;
    ChoicesHandler choices_;
    OpenWithHandler open_with_;
    TerminalHandler terminal_;
    std::optional<std::filesystem::path> templates_dir_;
    fs::PlaceSources place_sources_{
        fs::PlaceSources::from_environment()};  // read at start: where the templates folder is
                                                // found
    std::unique_ptr<Thumbnails> thumbnails_;    // before the views that draw them
    InfoPane info_pane_;                        // the same: every view draws it
    fs::DirectoryWatcher& watcher_;
    std::shared_ptr<const fs::MimeDatabase> mime_;
    // Here, before the window's parts and the transfers: destroyed after them, as the views were.
    std::vector<std::unique_ptr<Pane>> panes_;
    std::size_t active_{0};
    QuickLook quick_look_;
    Sidebar sidebar_;
    Breadcrumb breadcrumb_;
    TabBar tab_bar_;
    StatusBar status_bar_;
    ShortcutsOverlay shortcuts_;
    NameEditor editor_;
    ConflictDialog conflict_;
    ConfirmDialog confirm_;
    PasswordDialog password_;
    ContextMenu menu_;
    CommandPalette palette_;
    std::vector<std::string> palette_recent_;
    std::unique_ptr<FileFinder> finder_;  // while finding files

    struct FoundAfter {
        std::filesystem::path folder;
        std::string name;
        FoundAction action{FoundAction::show};
    };

    std::optional<FoundAfter> found_after_;
    std::function<void()> quit_;
    std::uint64_t menu_generation_{0};  // a submenu found late fills only the menu it was for
    std::size_t menu_loads_{0};         // applications and templates being looked for
    std::vector<std::function<void()>> menu_loaders_;  // started once the menu is open
    ui::Rect view_area_{};                             // where the view was last drawn
    bool info_visible_{true};
    bool sidebar_visible_{true};
    bool show_hidden_{false};
    bool sidebar_drawn_{false};  // in the last draw: the window had room for it
    Target pointer_target_{Target::view};
    bool button_held_{false};  // since a press: the pointer stays with its target
    // The pointer's shape: a view's or the sidebar's resize arrow wins, otherwise what is under
    // the pointer (cursor_at); sent only when it changes.
    ui::CursorShape resize_cursor_{ui::CursorShape::standard};
    ui::CursorShape hover_cursor_{ui::CursorShape::standard};
    std::optional<ui::CursorShape> shown_cursor_;

    // The last pointer position, handed to the view that comes on screen.
    float pointer_x_{0.0F};
    float pointer_y_{0.0F};
    bool pointer_inside_{false};
    // False once destroyed: renames and new folders still on their way find out here.
    std::shared_ptr<bool> alive_{std::make_shared<bool>(true)};
    std::size_t operations_{0};  // new folders and renames on workers

    // What Ctrl+C or Ctrl+X took.
    struct Clipboard {
        std::filesystem::path directory;
        std::vector<std::string> names;
        bool cut{false};
    };

    Clipboard clipboard_;
    SystemClipboard system_;

    DragAndDrop dnd_;
    Devices devices_;
    Remotes remotes_;
    // Leaves a folder on the disk at mount point before it goes: the view goes home.
    void leave_disk(const std::filesystem::path& mount_point);
    std::optional<std::filesystem::path> bookmarks_file_;
    std::function<void()> places_reload_;
    std::deque<BookmarkChange> bookmark_changes_;
    bool bookmark_busy_{false};

    // A press on an entry, which may become a drag.
    struct Press {
        float x{0.0F};
        float y{0.0F};
        ViewHit hit;
    };

    std::optional<Press> press_;

    // A press where a rubber band may start.
    struct BandPress {
        float x{0.0F};
        float y{0.0F};
        std::shared_ptr<Folder> folder;
        bool add{false};  // Ctrl: keep what is selected
    };

    std::optional<BandPress> band_press_;

    struct Band {
        std::shared_ptr<Folder> folder;
        float anchor_x{0.0F};  // where it started, in content points (BandLane)
        float anchor_y{0.0F};
        std::vector<std::string> base;  // selected before, kept (Ctrl)
        float x{0.0F};                  // the pointer, in the window
        float y{0.0F};
    };

    std::optional<Band> band_;
    core::EventLoop::SourceId band_timer_{0};

    // Nordend's drag.
    struct Drag {
        std::filesystem::path directory;
        std::vector<std::string> names;
        std::string icon;          // the first entry's
        bool folders_only{false};  // every entry is a folder
    };

    std::optional<Drag> drag_;
    DropMark drop_mark_;
    float drop_x_{0.0F};  // the drag over the window, last seen
    float drop_y_{0.0F};
    float auto_scroll_{0.0F};  // wheel notches per tick near an edge; 0: none
    core::EventLoop::SourceId auto_scroll_timer_{0};

    // For each transfer submitted and not done, in order (the queue's): its batch and the
    // clipboard generation to empty after it (submit()).
    struct Submitted {
        std::uint64_t batch{0};
        std::optional<std::uint64_t> empty_clipboard;
    };

    std::deque<Submitted> submitted_;

    struct Batch {
        Purpose purpose{Purpose::user};
        FileOperation::Kind kind{FileOperation::Kind::copy};
        std::size_t pending{0};
        std::vector<fs::PlacedItem> items;
        bool failed{false};  // something could not be done, or it was stopped
    };

    std::map<std::uint64_t, Batch> batches_;
    std::uint64_t batch_ids_{0};
    std::optional<FileOperation> undo_;  // what Ctrl+Z undoes
    std::optional<FileOperation> redo_;  // what Ctrl+Shift+Z undoes again
    bool undoing_{false};                // an undo or redo on its way
    std::filesystem::path home_trash_;
    std::vector<fs::Place> places_;
    fs::DirectoryModel trash_model_;  // every trash, for the sidebar's icon
    bool trash_full_{false};
    CutMarks cut_marks_;
    std::optional<fs::Resolution> for_the_rest_;

    // The delete the card asks about.
    struct PendingErase {
        std::uint64_t id{0};
        fs::TransferRequest request;
        bool planned{false};    // added up: request is complete (empty_trash: its trashes)
        bool confirmed{false};  // submitted once planned
    };

    std::optional<PendingErase> erase_;
    std::uint64_t erase_ids_{0};  // the running transfer's "the same for the rest"

    // Where the rate is measured from: a time and the bytes copied by then (after a question the
    // rate starts again).
    struct RateStart {
        std::chrono::steady_clock::time_point time;
        std::uint64_t bytes{0};
    };

    std::optional<RateStart> copy_started_;
    // Last: its thread stops first, while everything its callbacks touch is still there.
    fs::TransferQueue transfers_;
};

}  // namespace nordend::views
