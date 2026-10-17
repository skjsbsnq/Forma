#pragma once

#include <functional>
#include <mayonnaise/mayonnaise.hpp>
#include <string>
#include <vector>

namespace forma::mail {

// Uses the framework Label's wrapping, font metrics and rendering. Read-only instances
// provide selectable message text; editable instances add normal multi-line editing.
class TextEditor final : public mayo::LabelView {
public:
    explicit TextEditor(std::string text = {}, bool editable = true);
    void setValue(std::string value);
    [[nodiscard]] const std::string& value() const { return text(); }
    void setOnChange(std::function<void(const std::string&)> callback);
    [[nodiscard]] size_t caret() const { return m_caret; }
    [[nodiscard]] size_t anchor() const { return m_anchor; }
    void selectAll();
    void insert(std::string value);

    mayo::Size measureContent(mayo::Size available) override;
    bool acceptsFocus() const override { return true; }
    bool focusOnClick() const override { return true; }
    bool expandsContent(mayo::Axis axis) const override { return axis == mayo::Axis::Horizontal; }
    void draw(mayo::Painter& painter) override;
    bool onPointerDown(const mayo::PointerEvent& event) override;
    void onPointerMove(const mayo::PointerEvent& event) override;
    void onPointerUp(const mayo::PointerEvent& event) override;
    bool onKeyDown(const mayo::KeyEvent& event) override;
    bool onTextInput(const mayo::TextInputEvent& event) override;
    void onFocusChanged(bool focused) override;

private:
    struct Snapshot {
        std::string value;
        size_t caret, anchor;
    };
    struct Stop {
        size_t index;
        mayo::Point position;
    };
    std::vector<Stop> stops() const;
    size_t at(mayo::Point point) const;
    size_t before(size_t index, bool word = false) const;
    size_t after(size_t index, bool word = false) const;
    void move(size_t index, bool extending);
    void remember();
    void replaceSelection(std::string value);
    void changed();
    void revealCaret();
    void restore(std::vector<Snapshot>& from, std::vector<Snapshot>& to);
    bool m_editable = true, m_dragging = false, m_reveal = false;
    size_t m_caret = 0, m_anchor = 0;
    double m_blink = 0;
    std::function<void(const std::string&)> m_changed;
    std::vector<Snapshot> m_undo, m_redo;
};

}  // namespace forma::mail
