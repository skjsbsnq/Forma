#pragma once

#include <functional>
#include <string>
#include <vector>

#include "../control.hpp"

namespace mayo {

enum class TextFieldStyle : std::uint8_t {
    Rounded,  ///< standard bordered field
    Search,   ///< capsule with magnifier and clear button
    Plain,    ///< no bezel (inline editing)
};

class TextFieldView : public ControlView {
public:
    explicit TextFieldView(std::string placeholder);
    ~TextFieldView() override;
    /// Erases the current input before releasing its storage (for authentication forms).
    void clearSensitive();

    void setText(std::string text);
    [[nodiscard]] const std::string& text() const { return m_text; }
    void setPlaceholder(std::string p);
    void setStyle(TextFieldStyle style);
    void setSecure(bool secure);
    void setOnChange(std::function<void(const std::string&)> fn) { m_onChange = std::move(fn); }
    void setOnSubmit(std::function<void(const std::string&)> fn) { m_onSubmit = std::move(fn); }
    void selectAll();
    [[nodiscard]] size_t caret() const { return m_caret; }
    [[nodiscard]] size_t selectionAnchor() const { return m_anchor; }

    Size measureContent(Size available) override;
    [[nodiscard]] bool expandsContent(Axis axis) const override { return axis == Axis::Horizontal; }
    void draw(Painter& painter) override;
    void drawOverlay(Painter& /*painter*/) override {}  // the ring is drawn whenever focused, in draw()
    [[nodiscard]] bool focusOnClick() const override { return true; }
    bool onPointerDown(const PointerEvent& e) override;
    void onPointerMove(const PointerEvent& e) override;
    void onPointerUp(const PointerEvent& e) override;
    bool onKeyDown(const KeyEvent& e) override;
    bool onTextInput(const TextInputEvent& e) override;
    void onFocusChanged(bool focused) override;

    // Editing primitives (public for tests and programmatic control).
    void insert(std::string_view s);
    void deleteBackward(bool word);
    void deleteForward(bool word);
    void moveCaret(size_t to, bool extend);

protected:
    void focusRingShape(Rect& rect, CornerRadii& radii) const override;

private:
    [[nodiscard]] std::string displayText() const;
    [[nodiscard]] Rect textRect() const;
    [[nodiscard]] Rect clearButtonRect() const;
    [[nodiscard]] size_t indexAt(float x);
    [[nodiscard]] float xForIndex(size_t index);
    [[nodiscard]] size_t wordStart(size_t from) const;
    [[nodiscard]] size_t wordEnd(size_t from) const;
    [[nodiscard]] bool hasSelection() const { return m_anchor != m_caret; }
    void deleteSelection();
    void changed();
    void resetBlink();
    void ensureCaretVisible();
    void updateLayoutCache();

    std::string m_text;
    std::string m_placeholder;
    TextFieldStyle m_style = TextFieldStyle::Rounded;
    bool m_secure = false;
    size_t m_caret = 0;
    size_t m_anchor = 0;
    float m_scrollX = 0;
    double m_blinkStart = 0;
    bool m_dragging = false;
    bool m_clearPressed = false;
    // Cached caret positions for the current display text.
    std::vector<size_t> m_boundaries;
    std::vector<float> m_positions;
    bool m_cacheDirty = true;
    std::function<void(const std::string&)> m_onChange;
    std::function<void(const std::string&)> m_onSubmit;
};

class TextField : public WidgetHandle<TextField, TextFieldView> {
public:
    explicit TextField(std::string placeholder = {})
        : WidgetHandle(std::make_shared<TextFieldView>(std::move(placeholder))) {}
    TextField& text(std::string t) { return apply([&](TextFieldView& n) { n.setText(std::move(t)); }); }
    TextField& style(TextFieldStyle s) { return apply([&](TextFieldView& n) { n.setStyle(s); }); }
    TextField& secure(bool s = true) { return apply([&](TextFieldView& n) { n.setSecure(s); }); }
    TextField& onChange(std::function<void(const std::string&)> fn) {
        return apply([&](TextFieldView& n) { n.setOnChange(std::move(fn)); });
    }
    TextField& onSubmit(std::function<void(const std::string&)> fn) {
        return apply([&](TextFieldView& n) { n.setOnSubmit(std::move(fn)); });
    }
    void setText(std::string t) { m_node->setText(std::move(t)); }
    [[nodiscard]] const std::string& value() const { return m_node->text(); }
};

/// Shorthand for a search-styled TextField.
inline TextField SearchField(std::string placeholder = "Search") {
    return TextField(std::move(placeholder)).style(TextFieldStyle::Search);
}

}  // namespace mayo
