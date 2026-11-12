#pragma once

#include <cstddef>
#include <cstdint>
#include <functional>
#include <optional>
#include <string>
#include <string_view>
#include <vector>

#include "nordend/theme/palette.hpp"
#include "nordend/ui/geometry.hpp"
#include "nordend/ui/renderer.hpp"
#include "nordend/ui/seat.hpp"
#include "nordend/ui/text_field.hpp"

namespace nordend::views {

// What a network folder asks before it mounts (gvfs's AskPassword): the fields to show.
struct LoginRequest {
    std::string message;  // the server's question ("Password required for share public on ...")
    std::string user;     // filled in already
    std::string domain;
    bool ask_user{false};
    bool ask_domain{false};
    bool ask_password{true};
    bool anonymous{false};  // "Anonymous" can be chosen instead
    bool saving{false};     // "Remember the password" can be chosen
};

// The answer.
struct Login {
    std::string user;
    std::string domain;
    std::string password;
    bool anonymous{false};
    bool remember{false};
};

// The name and password a network folder needs, on a card over the window: Tab goes from one
// field to the next, Enter connects, Escape gives up; so do its buttons. While it is open it takes
// every key and click. The password is drawn as dots and forgotten when the card closes.
//
// Loop thread only.
class PasswordDialog {
public:
    // nullopt: Escape or Cancel.
    using Answer = std::function<void(std::optional<Login> login)>;

    explicit PasswordDialog(std::function<void()> redraw);

    // title: "Connect to “Public on fileshare”".
    void open(std::string title, LoginRequest request, Answer answer);

    // Closes without answering (gvfs stopped waiting).
    void close();

    [[nodiscard]] bool is_open() const noexcept { return answer_ != nullptr; }

    [[nodiscard]] const std::string& title() const noexcept { return title_; }

    [[nodiscard]] bool anonymous() const noexcept { return anonymous_; }

    // The field the keys go to: "user", "domain", "password", "mode", "remember"; empty when none.
    [[nodiscard]] std::string_view focused() const;

    // Every key while open: true.
    bool key(const ui::KeyEvent& event);

    // Ctrl+V: text into the field with the keyboard (its first line).
    void paste(std::string_view text);

    void pointer(const ui::PointerEvent& event);

    void draw(ui::Renderer& renderer, const ui::Rect& bounds, const theme::Palette& palette);

    // A button, a field or a toggle is there, as last drawn.
    [[nodiscard]] bool clickable_at(float x, float y) const;

private:
    enum class Item : std::uint8_t { mode, user, domain, password, remember };

    // The items Tab goes through, in order, as the mode shows them.
    [[nodiscard]] std::vector<Item> items() const;
    [[nodiscard]] ui::TextField* field(Item item);
    void move_focus(int step);
    void choose(bool connect);

    // What the pointer is over, of what it can click.
    enum class Target : std::uint8_t { none, registered, anonymous, remember, cancel, connect };

    [[nodiscard]] Target target_at(float x, float y) const;

    struct Rects {
        ui::Rect registered;
        ui::Rect anonymous;
        ui::Rect user;
        ui::Rect domain;
        ui::Rect password;
        ui::Rect remember;
        ui::Rect cancel;
        ui::Rect connect;
    };

    std::function<void()> redraw_;
    std::string title_;
    LoginRequest request_;
    Answer answer_;
    ui::TextField user_;
    ui::TextField domain_;
    ui::TextField password_;
    bool anonymous_{false};
    bool remember_{false};
    Item focus_{Item::password};
    Rects rects_{};  // as last drawn
    Target hovered_{Target::none};
};

}  // namespace nordend::views
