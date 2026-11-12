#pragma once

#include <cstddef>
#include <cstdint>
#include <string>
#include <string_view>
#include <utility>
#include <vector>

#include "nordend/ui/color.hpp"
#include "nordend/ui/geometry.hpp"
#include "nordend/ui/renderer.hpp"
#include "nordend/ui/seat.hpp"

namespace nordend::ui {

// The colors a TextField is drawn with.
struct FieldColors {
    Color background;
    Color border;
    Color text;
    Color selection;  // behind the selected text
    Color caret;
};

// One line of editable UTF-8 text: a caret, a selection, the usual editing keys, drawn into a
// rectangle and scrolled sideways to keep the caret in sight. Positions are byte offsets, always
// on character (code point) boundaries. The caret does not blink: a blinking one would need a
// frame every half second, and an idle Nordend draws nothing. Ctrl+Z undoes the last edit and
// Ctrl+Shift+Z redoes it: characters typed one after another, or removed one after another, are
// one edit; a paste is one; moving the caret ends one.
//
// Loop thread only.
class TextField {
public:
    explicit TextField(std::string text = {});

    // How many edits Ctrl+Z can undo; older ones are forgotten.
    static constexpr std::size_t kHistory = 100;

    // Replaces the text; the caret goes to its end, nothing selected, no edits to undo.
    void set_text(std::string text);

    [[nodiscard]] const std::string& text() const noexcept { return text_; }

    // A password's field: every character drawn as a dot, Ctrl+Left and Ctrl+Right go to the
    // ends (the words would show through).
    void set_masked(bool masked) noexcept { masked_ = masked; }

    [[nodiscard]] bool masked() const noexcept { return masked_; }

    // Selects [begin, end) (clamped, moved back to character boundaries); the caret at end.
    void select(std::size_t begin, std::size_t end);

    void select_all() { select(0, text_.size()); }

    [[nodiscard]] std::size_t caret() const noexcept { return caret_; }

    // The selection as [first, last); empty (first == last == caret) when there is none.
    [[nodiscard]] std::pair<std::size_t, std::size_t> selection() const noexcept;

    // Replaces the selection (or inserts at the caret) with text: one edit (a paste).
    void insert(std::string_view text);

    // Ctrl+Z, Ctrl+Shift+Z: false when there is nothing to undo or redo.
    bool undo();
    bool redo();

    // Typing (event.text without Ctrl or Alt), Backspace, Delete, Left, Right, Home, End, Up
    // (Home), Down (End), with Shift to select and Ctrl to go by words; Ctrl+A selects all;
    // Ctrl+Z undoes, Ctrl+Shift+Z redoes.
    // false for keys it does not use (Enter, Escape, Tab, function keys).
    bool key(const KeyEvent& event);

    // Puts the caret at the character nearest x (window coordinates, from the last draw); with
    // extend, the selection reaches it from where it starts.
    void click(float x, bool extend);

    // A box in rect, the text at size (logical pixels) with the selection behind it and the caret.
    void draw(Renderer& renderer, const Rect& rect, float size, const FieldColors& colors);

    // Where the box was last drawn.
    [[nodiscard]] const Rect& bounds() const noexcept { return bounds_; }

    static constexpr float kPadding = 4.0F;

private:
    // What an edit does: typing and removing go on as one edit while repeated.
    enum class Edit : std::uint8_t { other, typing, removing };

    struct State {
        std::string text;
        std::size_t caret{0};
        std::size_t anchor{0};
    };

    // Before an edit of kind: keeps what is there for Ctrl+Z, unless it goes on the last edit.
    void remember(Edit kind);
    void restore(State state);

    [[nodiscard]] std::size_t previous(std::size_t at) const noexcept;
    [[nodiscard]] std::size_t next(std::size_t at) const noexcept;
    [[nodiscard]] std::size_t previous_word(std::size_t at) const noexcept;
    [[nodiscard]] std::size_t next_word(std::size_t at) const noexcept;
    // Moves the caret; with extend the anchor stays, otherwise it follows.
    void move_to(std::size_t at, bool extend);
    // Removes [first, last) and puts the caret there.
    void erase(std::size_t first, std::size_t last);
    [[nodiscard]] float width_of(FontSystem& fonts, std::size_t bytes) const;
    // What is drawn of the first bytes of the text: themselves, or a dot for each character.
    [[nodiscard]] std::string drawn(std::size_t bytes) const;

    std::string text_;
    std::size_t caret_{0};
    std::size_t anchor_{0};  // the selection's other end; == caret_ when nothing is selected
    float scroll_{0.0F};     // how far the text is shifted left
    Rect bounds_{};
    float size_{14.0F};
    float scale_{1.0F};
    FontSystem* fonts_{nullptr};  // from the last draw: click() measures with it
    std::vector<State> undo_;     // oldest first
    std::vector<State> redo_;
    Edit last_edit_{Edit::other};  // other: the next edit starts a new one
    bool masked_{false};
};

}  // namespace nordend::ui
