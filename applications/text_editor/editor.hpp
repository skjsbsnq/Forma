#pragma once

#include <functional>
#include <mayonnaise/mayonnaise.hpp>
#include <string>
#include <vector>

namespace forma::text {

// Owns one verbatim UTF-8 buffer. Font/wrap/caret metrics use the renderer's
// CPU-only helpers; readonly instances provide selectable message text.
class TextEditor final : public mayo::Widget {
public:
    explicit TextEditor(std::string text = {}, bool editable = true);
    void setValue(std::string value);
    [[nodiscard]] const std::string& value() const { return m_value; }
    [[nodiscard]] uint64_t revision() const { return m_version; }
    void setOnChange(std::function<void(const std::string&)> callback);
    [[nodiscard]] size_t caret() const { return m_caret; }
    [[nodiscard]] size_t anchor() const { return m_anchor; }
    void selectAll();
    void insert(std::string value);
    void setSelection(size_t anchor, size_t caret);
    bool find(std::string_view query, bool backwards = false);
    size_t replaceAll(std::string_view query, std::string_view replacement);
    void setLineNumbers(bool on);
    void setWrap(bool on);
    void setHighlight(bool on) {
        if (m_highlight == on) return;
        m_highlight = on;
        invalidate();
    }
    void setEditorFont(float size);
    [[nodiscard]] size_t lineCount() const;
    [[nodiscard]] size_t characterCount() const;
    [[nodiscard]] std::pair<size_t, size_t> location() const;
    [[nodiscard]] std::string selection() const;

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
    bool onTextEditing(const mayo::TextEditingEvent& event) override;
    bool isComposing() const override { return !m_preedit.empty(); }
    void cancelComposition() override;
    void onFocusChanged(bool focused) override;

private:
    friend struct EditorTestAccess;
    struct Edit {
        size_t offset;
        std::string removed, inserted;
    };
    struct UndoRecord {
        std::vector<Edit> edits;  // ascending offsets in the pre-edit buffer
        size_t beforeCaret, beforeAnchor, afterCaret, afterAnchor;
        size_t bytes = 0;
    };
    struct Line {
        size_t begin, end, number;
        float width;
        bool continuation;
        mutable std::vector<size_t> boundaries;
        mutable std::vector<float> positions;
    };
    const std::string& text() const { return m_value; }
    const mayo::Font& font() const { return m_font; }
    void layout(float width) const;
    void layout() const;
    void caretLayout(size_t line) const;
    size_t lineAt(size_t index) const;
    size_t caretLine() const;
    mayo::Point pointAt(size_t index) const;
    mayo::Point caretPosition() const;
    void preeditLayout() const;
    mayo::Point inputPosition() const;
    void drawPreedit(mayo::Painter& painter);
    size_t at(mayo::Point point) const;
    size_t before(size_t index, bool word = false) const;
    size_t after(size_t index, bool word = false) const;
    void move(size_t index, bool extending);
    void replaceSelection(std::string value);
    void commit(UndoRecord record);
    void apply(const UndoRecord& record, bool undo);
    void changed();
    void revealCaret();
    void restore(bool undo);
    void statistics() const;
    bool m_lineNumbers = true, m_wrap = true, m_highlight = true;
    bool m_editable = true, m_dragging = false, m_reveal = false;
    size_t m_caret = 0, m_anchor = 0;
    double m_blink = 0;
    std::string m_value;
    std::string m_preedit;
    size_t m_preeditOffset = 0, m_preeditStart = 0, m_preeditEnd = 0;
    mutable std::vector<Line> m_preeditLines;
    mutable bool m_preeditDirty = true;
    mutable float m_preeditWidth = -1, m_preeditFirstOffset = 0;
    mutable mayo::Font m_preeditFont;
    mayo::Font m_font;
    std::function<void(const std::string&)> m_changed;
    std::vector<UndoRecord> m_undo, m_redo;
    size_t m_historyBytes = 0;
    uint64_t m_version = 1;
    mutable uint64_t m_layoutVersion = 0, m_statisticsVersion = 0;
    mutable float m_layoutWidth = -1, m_maxWidth = 0;
    mutable mayo::Font m_layoutFont;
    mutable std::vector<Line> m_lines;
    mutable std::vector<size_t> m_lineStarts;
    mutable size_t m_characters = 0, m_layoutBuilds = 0, m_caretBuilds = 0;
};

}  // namespace forma::text
