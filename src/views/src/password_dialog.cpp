#include "nordend/views/password_dialog.hpp"

#include <algorithm>
#include <tuple>
#include <utility>
#include <xkbcommon/xkbcommon-keysyms.h>

namespace nordend::views {
namespace {

constexpr float kCardWidth = 460.0F;
constexpr float kMargin = 24.0F;
constexpr float kPadding = 24.0F;
constexpr float kTitleSize = 16.0F;
constexpr float kTextSize = 13.0F;
constexpr float kFieldHeight = 30.0F;
constexpr float kRowGap = 10.0F;
constexpr float kLabelWidth = 84.0F;
constexpr float kModeHeight = 30.0F;
constexpr float kCheckSize = 16.0F;
constexpr float kButtonHeight = 34.0F;
constexpr float kButtonGap = 8.0F;
constexpr float kButtonWidth = 116.0F;
constexpr float kLineHeight = 20.0F;
constexpr std::size_t kMessageLines = 3;

// The server's message, a line each ("Authentication Required\nEnter user and password..."),
// at most kMessageLines of them, the empty ones left out.
std::vector<std::string_view> lines_of(std::string_view text) {
    std::vector<std::string_view> lines;
    while (!text.empty() && lines.size() < kMessageLines) {
        const std::size_t end = std::min(text.find('\n'), text.size());
        if (const std::string_view line = text.substr(0, end); !line.empty()) {
            lines.push_back(line);
        }
        text.remove_prefix(std::min(end + 1, text.size()));
    }
    return lines;
}

// The password is not kept once answered (nor in the field's undo history).
void wipe(ui::TextField& field) { field.set_text({}); }

}  // namespace

PasswordDialog::PasswordDialog(std::function<void()> redraw) : redraw_{std::move(redraw)} {
    password_.set_masked(true);
}

void PasswordDialog::open(std::string title, LoginRequest request, Answer answer) {
    title_ = std::move(title);
    request_ = std::move(request);
    answer_ = std::move(answer);
    user_.set_text(request_.user);
    domain_.set_text(request_.domain);
    wipe(password_);
    anonymous_ = false;
    remember_ = false;
    hovered_ = Target::none;
    // The first field to fill: the name when there is none yet, else the password.
    const std::vector<Item> order = items();
    focus_ = order.empty() ? Item::mode : order.front();
    for (const Item item : order) {
        if (item == Item::user && user_.text().empty()) {
            focus_ = item;
            break;
        }
        if (item == Item::password) {
            focus_ = item;
            break;
        }
    }
    redraw_();
}

void PasswordDialog::close() {
    if (answer_) {
        answer_ = nullptr;
        wipe(password_);
        redraw_();
    }
}

std::vector<PasswordDialog::Item> PasswordDialog::items() const {
    std::vector<Item> out;
    if (request_.anonymous) {
        out.push_back(Item::mode);
    }
    if (anonymous_) {
        return out;
    }
    if (request_.ask_user) {
        out.push_back(Item::user);
    }
    if (request_.ask_domain) {
        out.push_back(Item::domain);
    }
    if (request_.ask_password) {
        out.push_back(Item::password);
    }
    if (request_.saving && request_.ask_password) {
        out.push_back(Item::remember);
    }
    return out;
}

std::string_view PasswordDialog::focused() const {
    if (!is_open()) {
        return {};
    }
    switch (focus_) {
        case Item::mode:
            return "mode";
        case Item::user:
            return "user";
        case Item::domain:
            return "domain";
        case Item::password:
            return "password";
        case Item::remember:
            return "remember";
    }
    return {};
}

ui::TextField* PasswordDialog::field(Item item) {
    switch (item) {
        case Item::user:
            return &user_;
        case Item::domain:
            return &domain_;
        case Item::password:
            return &password_;
        case Item::mode:
        case Item::remember:
            break;
    }
    return nullptr;
}

void PasswordDialog::move_focus(int step) {
    const std::vector<Item> order = items();
    if (order.empty()) {
        return;
    }
    const auto it = std::ranges::find(order, focus_);
    const auto count = static_cast<int>(order.size());
    const int at = it == order.end() ? 0 : static_cast<int>(it - order.begin());
    focus_ = order[static_cast<std::size_t>(((at + step) % count + count) % count)];
    redraw_();
}

void PasswordDialog::choose(bool connect) {
    Answer answer = std::exchange(answer_, nullptr);
    std::optional<Login> login;
    if (connect) {
        login = Login{.user = anonymous_ ? std::string{} : user_.text(),
                      .domain = anonymous_ ? std::string{} : domain_.text(),
                      .password = anonymous_ ? std::string{} : password_.text(),
                      .anonymous = anonymous_,
                      .remember = remember_ && !anonymous_};
    }
    wipe(password_);
    redraw_();
    if (answer) {
        answer(std::move(login));
    }
}

bool PasswordDialog::key(const ui::KeyEvent& event) {
    if (!is_open()) {
        return false;
    }
    switch (event.keysym) {
        case XKB_KEY_Return:
        case XKB_KEY_KP_Enter:
            choose(true);
            return true;
        case XKB_KEY_Escape:
            choose(false);
            return true;
        case XKB_KEY_Tab:
        case XKB_KEY_ISO_Left_Tab:
            move_focus(event.modifiers.shift || event.keysym == XKB_KEY_ISO_Left_Tab ? -1 : 1);
            return true;
        default:
            break;
    }
    if (focus_ == Item::mode) {
        if (event.keysym == XKB_KEY_Left || event.keysym == XKB_KEY_Right ||
            event.keysym == XKB_KEY_space) {
            anonymous_ =
                event.keysym == XKB_KEY_space ? !anonymous_ : event.keysym == XKB_KEY_Right;
            redraw_();
        }
        return true;
    }
    if (focus_ == Item::remember) {
        if (event.keysym == XKB_KEY_space) {
            remember_ = !remember_;
            redraw_();
        }
        return true;
    }
    if (ui::TextField* const target = field(focus_); target != nullptr && target->key(event)) {
        redraw_();
    }
    return true;
}

void PasswordDialog::paste(std::string_view text) {
    if (!is_open()) {
        return;
    }
    text = text.substr(0, text.find_first_of("\r\n"));
    if (ui::TextField* const target = field(focus_); target != nullptr && !text.empty()) {
        target->insert(text);
        redraw_();
    }
}

PasswordDialog::Target PasswordDialog::target_at(float x, float y) const {
    const std::pair<const ui::Rect*, Target> targets[]{
        {&rects_.registered, Target::registered}, {&rects_.anonymous, Target::anonymous},
        {&rects_.remember, Target::remember},     {&rects_.cancel, Target::cancel},
        {&rects_.connect, Target::connect},
    };
    for (const auto& [rect, target] : targets) {
        if (!rect->empty() && rect->contains(x, y)) {
            return target;
        }
    }
    return Target::none;
}

bool PasswordDialog::clickable_at(float x, float y) const {
    return target_at(x, y) != Target::none;
}

void PasswordDialog::pointer(const ui::PointerEvent& event) {
    if (!is_open()) {
        return;
    }
    using Kind = ui::PointerEvent::Kind;
    const Target over = event.kind == Kind::leave ? Target::none : target_at(event.x, event.y);
    if (over != hovered_) {
        hovered_ = over;
        redraw_();
    }
    if (event.kind != Kind::press || event.button != ui::PointerButton::left) {
        return;
    }
    switch (over) {
        case Target::registered:
        case Target::anonymous:
            anonymous_ = over == Target::anonymous;
            focus_ = Item::mode;
            redraw_();
            return;
        case Target::remember:
            remember_ = !remember_;
            focus_ = Item::remember;
            redraw_();
            return;
        case Target::cancel:
            choose(false);
            return;
        case Target::connect:
            choose(true);
            return;
        case Target::none:
            break;
    }
    for (const auto& [rect, item] :
         {std::pair{rects_.user, Item::user}, std::pair{rects_.domain, Item::domain},
          std::pair{rects_.password, Item::password}}) {
        if (!rect.empty() && rect.contains(event.x, event.y)) {
            focus_ = item;
            field(item)->click(event.x, event.modifiers.shift);
            redraw_();
            return;
        }
    }
}

void PasswordDialog::draw(ui::Renderer& renderer, const ui::Rect& bounds,
                          const theme::Palette& palette) {
    if (!is_open()) {
        return;
    }
    rects_ = {};
    renderer.fill_rect(bounds, palette.scrim.with_alpha(0.55F));
    const bool fields = !anonymous_;
    const int rows = fields ? static_cast<int>(request_.ask_user) +
                                  static_cast<int>(request_.ask_domain) +
                                  static_cast<int>(request_.ask_password)
                            : 0;
    const bool remember = fields && request_.saving && request_.ask_password;
    const float width = std::min(kCardWidth, bounds.width - (2.0F * kMargin));
    const std::vector<std::string_view> message = lines_of(request_.message);
    float height = kPadding + 30.0F + (static_cast<float>(message.size()) * kLineHeight) + 16.0F;
    if (request_.anonymous) {
        height += kModeHeight + kRowGap + 4.0F;
    }
    height += static_cast<float>(rows) * (kFieldHeight + kRowGap);
    if (remember) {
        height += kCheckSize + kRowGap + 4.0F;
    }
    height += 12.0F + kButtonHeight + kPadding;
    const ui::Rect card{bounds.x + ((bounds.width - width) / 2.0F),
                        bounds.y + ((bounds.height - height) / 2.0F), width, height};
    renderer.draw_box(card, {.fill = palette.surface_container_high,
                             .radius = ui::Corners::all(14.0F),
                             .border_width = 1.0F,
                             .border = palette.outline_variant});
    renderer.push_clip(card);
    const float left = card.x + kPadding;
    const float room = width - (2.0F * kPadding);
    float y = card.y + kPadding;
    renderer.draw_text(title_, left, y, kTitleSize, palette.on_surface, room, ui::Ellipsis::middle);
    y += 30.0F;
    for (const std::string_view line : message) {
        renderer.draw_text(line, left, y, kTextSize, palette.on_surface_variant, room);
        y += kLineHeight;
    }
    y += 16.0F;

    const auto text_y = [](const ui::Rect& rect) {
        return rect.y + ((rect.height - kTextSize) / 2.0F) - 2.0F;
    };
    if (request_.anonymous) {
        // Two halves: as whom to connect.
        const float half = room / 2.0F;
        rects_.registered = {left, y, half, kModeHeight};
        rects_.anonymous = {left + half, y, half, kModeHeight};
        for (const auto& [rect, chosen, label, target] :
             {std::tuple{rects_.registered, !anonymous_, "Registered user", Target::registered},
              std::tuple{rects_.anonymous, anonymous_, "Anonymous", Target::anonymous}}) {
            ui::Color fill =
                chosen ? palette.secondary_container : palette.surface_container_highest;
            if (hovered_ == target && !chosen) {
                fill = palette.surface_bright;
            }
            renderer.draw_box(rect, {.fill = fill,
                                     .radius = ui::Corners::all(8.0F),
                                     .border_width = focus_ == Item::mode && chosen ? 1.5F : 0.0F,
                                     .border = palette.primary});
            const float label_width =
                renderer.fonts().layout(label, kTextSize, renderer.scale()).width /
                renderer.scale();
            renderer.draw_text(
                label, rect.x + ((rect.width - label_width) / 2.0F), text_y(rect), kTextSize,
                chosen ? palette.on_secondary_container : palette.on_surface, rect.width - 8.0F);
        }
        y += kModeHeight + kRowGap + 4.0F;
    }

    if (fields) {
        const ui::FieldColors focused_colors{.background = palette.surface_container_highest,
                                             .border = palette.primary,
                                             .text = palette.on_surface,
                                             .selection = palette.secondary_container,
                                             .caret = palette.on_surface};
        ui::FieldColors quiet_colors = focused_colors;
        quiet_colors.border = palette.outline_variant;
        quiet_colors.caret = palette.surface_container_highest;
        for (const auto& [ask, item, label, rect] :
             {std::tuple{request_.ask_user, Item::user, "User", &rects_.user},
              std::tuple{request_.ask_domain, Item::domain, "Domain", &rects_.domain},
              std::tuple{request_.ask_password, Item::password, "Password", &rects_.password}}) {
            if (!ask) {
                continue;
            }
            *rect = {left + kLabelWidth, y, room - kLabelWidth, kFieldHeight};
            renderer.draw_text(label, left, text_y(*rect), kTextSize, palette.on_surface_variant,
                               kLabelWidth - 8.0F);
            field(item)->draw(renderer, *rect, kTextSize,
                              focus_ == item ? focused_colors : quiet_colors);
            y += kFieldHeight + kRowGap;
        }
    }
    if (remember) {
        y += 4.0F;
        const ui::Rect box{left + kLabelWidth, y, kCheckSize, kCheckSize};
        rects_.remember = {box.x - 4.0F, y - 4.0F, room - kLabelWidth + 4.0F, kCheckSize + 8.0F};
        renderer.draw_box(box,
                          {.fill = remember_ ? palette.primary : palette.surface_container_highest,
                           .radius = ui::Corners::all(4.0F),
                           .border_width = focus_ == Item::remember ? 2.0F : 1.0F,
                           .border = focus_ == Item::remember ? palette.primary : palette.outline});
        if (remember_) {
            renderer.fill_rect(box.inset(4.5F), palette.on_primary);
        }
        renderer.draw_text(
            "Remember the password", box.right() + 10.0F,
            y + ((kCheckSize - kTextSize) / 2.0F) - 2.0F, kTextSize,
            hovered_ == Target::remember ? palette.on_surface : palette.on_surface_variant,
            room - kLabelWidth - kCheckSize - 10.0F);
        y += kCheckSize + kRowGap;
    }
    y += 12.0F;

    rects_.connect = {card.right() - kPadding - kButtonWidth, y, kButtonWidth, kButtonHeight};
    rects_.cancel = {rects_.connect.x - kButtonGap - kButtonWidth, y, kButtonWidth, kButtonHeight};
    for (const bool connect : {false, true}) {
        const ui::Rect& button = connect ? rects_.connect : rects_.cancel;
        const bool hover = hovered_ == (connect ? Target::connect : Target::cancel);
        ui::Color fill = connect ? palette.primary : palette.surface_container_highest;
        ui::Color text = connect ? palette.on_primary : palette.on_surface;
        if (hover) {
            fill = connect ? palette.primary_container : palette.surface_bright;
            text = connect ? palette.on_primary_container : palette.on_surface;
        }
        renderer.draw_box(button, {.fill = fill, .radius = ui::Corners::all(8.0F)});
        renderer.draw_text(connect ? "Connect   ↵" : "Cancel   Esc", button.x + 14.0F,
                           text_y(button), kTextSize, text, button.width - 20.0F);
    }
    renderer.pop_clip();
}

}  // namespace nordend::views
