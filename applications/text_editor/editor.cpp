#include "editor.hpp"
#include <algorithm>
#include <cctype>
#include <cmath>
#include "../../src/render/font.hpp"

using namespace mayo;
namespace forma::text {
TextEditor::TextEditor(std::string value, bool editable) : m_editable(editable), m_value(std::move(value)) {
    setEditorFont(13);
    setCursor(CursorShape::IBeam);
    setPadding(Insets(7, 40, 20, 16));
}
void TextEditor::setValue(std::string value) {
    cancelComposition();
    m_value = std::move(value);
    ++m_version;
    m_caret = std::min(m_caret, text().size());
    while (m_caret && m_caret < text().size() && (static_cast<unsigned char>(text()[m_caret]) & 192) == 128) --m_caret;
    m_anchor = m_caret;
    m_undo.clear();
    m_redo.clear();
    m_historyBytes = 0;
    invalidateLayout();
}
void TextEditor::setOnChange(std::function<void(const std::string&)> callback) { m_changed = std::move(callback); }
void TextEditor::layout(float width) const {
    width = m_wrap ? width + .5f : kInfinity;
    if (m_layoutVersion == m_version && m_layoutWidth == width && m_layoutFont == font()) return;
    m_layoutVersion = m_version;
    m_layoutWidth = width;
    m_layoutFont = font();
    ++m_layoutBuilds;
    m_lines.clear();
    m_maxWidth = 0;
    size_t number = 1, start = 0;
    const auto value = std::string_view(text());
    for (;;) {
        auto end = value.find_first_of("\r\n", start);
        if (end == std::string_view::npos) end = value.size();
        for (const auto& line : render::wrapText(value.substr(start, end - start), font(), width)) {
            m_lines.push_back({start + line.begin, start + line.end, number, line.width, line.begin > 0, {}, {}});
            m_maxWidth = std::max(m_maxWidth, line.width);
        }
        if (end == value.size()) break;
        start = end + 1;
        if (value[end] == '\r' && start < value.size() && value[start] == '\n') ++start;
        ++number;
    }
    if (m_lines.empty()) m_lines.push_back({0, 0, 1, 0, false, {}, {}});
}
void TextEditor::layout() const { layout(contentRect().width); }
void TextEditor::caretLayout(size_t row) const {
    auto& line = m_lines[row];
    if (!line.boundaries.empty()) return;
    ++m_caretBuilds;
    render::caretPositions(std::string_view(text()).substr(line.begin, line.end - line.begin), font(), line.boundaries,
                           line.positions);
}
size_t TextEditor::lineAt(size_t index) const {
    layout();
    auto it = std::upper_bound(m_lines.begin(), m_lines.end(), index,
                               [](size_t offset, const Line& line) { return offset < line.begin; });
    return it == m_lines.begin() ? 0 : static_cast<size_t>(it - m_lines.begin() - 1);
}
size_t TextEditor::caretLine() const { return lineAt(m_caret); }
Point TextEditor::pointAt(size_t index) const {
    const auto row = lineAt(index);
    caretLayout(row);
    const auto& line = m_lines[row];
    const auto it = std::lower_bound(line.boundaries.begin(), line.boundaries.end(), index - line.begin);
    size_t column = std::min(static_cast<size_t>(it - line.boundaries.begin()), line.positions.size() - 1);
    auto rect = contentRect();
    return {rect.x + line.positions[column], rect.y + static_cast<float>(row) * font().lineHeight};
}
Point TextEditor::caretPosition() const { return pointAt(m_caret); }
void TextEditor::preeditLayout() const {
    if (!isComposing()) return;
    const auto origin = pointAt(m_preeditOffset);
    const auto rect = contentRect();
    const float offset = origin.x - rect.x;
    if (!m_preeditDirty && m_preeditWidth == rect.width && m_preeditFirstOffset == offset && m_preeditFont == font())
        return;
    m_preeditDirty = false;
    m_preeditWidth = rect.width;
    m_preeditFirstOffset = offset;
    m_preeditFont = font();
    m_preeditLines.clear();
    const auto value = std::string_view(m_preedit);
    // The first inline row starts at the document caret; following rows use the
    // full content width. No document-wide layout is dirtied by a preedit update.
    const auto first = render::wrapText(value, font(), m_wrap ? std::max(1.0f, rect.width - offset) : kInfinity);
    const auto add = [&](size_t begin, size_t end, float width) {
        Line line{begin, end, 0, width, false, {}, {}};
        render::caretPositions(value.substr(begin, end - begin), font(), line.boundaries, line.positions);
        m_preeditLines.push_back(std::move(line));
    };
    add(first.front().begin, first.front().end, first.front().width);
    if (first.size() > 1) {
        const size_t start = first[1].begin;
        for (const auto& line :
             render::wrapText(value.substr(start), font(), m_wrap ? std::max(1.0f, rect.width) : kInfinity))
            add(start + line.begin, start + line.end, line.width);
    }
}
Point TextEditor::inputPosition() const {
    if (!isComposing()) return caretPosition();
    preeditLayout();
    auto it = std::upper_bound(m_preeditLines.begin(), m_preeditLines.end(), m_preeditEnd,
                               [](size_t index, const Line& line) { return index < line.begin; });
    const auto row = it == m_preeditLines.begin() ? 0 : static_cast<size_t>(it - m_preeditLines.begin() - 1);
    const auto& line = m_preeditLines[row];
    const auto boundary = std::lower_bound(line.boundaries.begin(), line.boundaries.end(), m_preeditEnd - line.begin);
    const auto column = std::min(static_cast<size_t>(boundary - line.boundaries.begin()), line.positions.size() - 1);
    const auto origin = pointAt(m_preeditOffset);
    return {(row == 0 ? origin.x : contentRect().x) + line.positions[column],
            origin.y + static_cast<float>(row) * font().lineHeight};
}
void TextEditor::drawPreedit(Painter& painter) {
    preeditLayout();
    const auto& colors = theme().colors;
    const auto origin = pointAt(m_preeditOffset);
    const auto metrics = fontMetrics(font());
    const float baseline = (font().lineHeight - metrics.ascender - metrics.descender) * .5f + metrics.ascender;
    for (size_t row = 0; row < m_preeditLines.size(); ++row) {
        const auto& line = m_preeditLines[row];
        const float x = row == 0 ? origin.x : contentRect().x;
        const float y = origin.y + static_cast<float>(row) * font().lineHeight;
        if (y + font().lineHeight < painter.clipBounds().y || y > painter.clipBounds().bottom()) continue;
        // Opaque inline presentation keeps preedit distinct from underlying
        // document glyphs and selection without changing the saved buffer.
        painter.fillRect({x, y, std::max(2.0f, line.width), font().lineHeight}, colors.windowBackground);
        painter.drawText(std::string_view(m_preedit).substr(line.begin, line.end - line.begin), Point{x, y + baseline},
                         font(), colors.label);
        painter.fillRect({x, y + font().lineHeight - 2, std::max(2.0f, line.width), 1}, colors.secondaryLabel);
        if (m_preeditStart < m_preeditEnd && m_preeditStart <= line.end && m_preeditEnd > line.begin) {
            const auto xFor = [&](size_t index) {
                const auto it = std::lower_bound(line.boundaries.begin(), line.boundaries.end(),
                                                 std::clamp(index, line.begin, line.end) - line.begin);
                return line.positions[static_cast<size_t>(it - line.boundaries.begin())];
            };
            const float start = xFor(m_preeditStart), end = xFor(m_preeditEnd);
            painter.fillRect({x + start, y + font().lineHeight - 3, std::max(2.0f, end - start), 2}, colors.accent);
        }
    }
    const auto cursor = inputPosition();
    painter.fillRect({cursor.x, cursor.y + 2, 1, font().lineHeight - 3}, colors.accent);
}
Size TextEditor::measureContent(Size available) {
    layout(available.width);
    const float width = m_wrap && std::isfinite(available.width) ? available.width
                        : std::isfinite(available.width)         ? std::max(std::ceil(m_maxWidth), available.width)
                                                                 : std::ceil(m_maxWidth);
    return {width, std::max(static_cast<float>(m_lines.size()) * font().lineHeight, m_editable ? 200.0f : 20.0f)};
}
size_t TextEditor::at(Point point) const {
    layout();
    const auto rect = contentRect();
    const auto row = static_cast<size_t>(
        std::clamp(std::floor((point.y - rect.y) / font().lineHeight), 0.0f, static_cast<float>(m_lines.size() - 1)));
    caretLayout(row);
    const auto& line = m_lines[row];
    const float x = point.x - rect.x;
    auto it = std::lower_bound(line.positions.begin(), line.positions.end(), x);
    size_t column = std::min(static_cast<size_t>(it - line.positions.begin()), line.positions.size() - 1);
    if (column && std::abs(line.positions[column - 1] - x) < std::abs(line.positions[column] - x)) --column;
    return line.begin + line.boundaries[column];
}
size_t TextEditor::before(size_t index, bool word) const {
    if (!index) return 0;
    auto step = [&](size_t i) {
        --i;
        if (i && text()[i] == '\n' && text()[i - 1] == '\r') --i;
        while (i > 0 && (static_cast<unsigned char>(text()[i]) & 192) == 128) --i;
        return i;
    };
    index = step(index);
    if (word) {
        while (index && std::isspace(static_cast<unsigned char>(text()[index]))) index = step(index);
        while (index) {
            auto prev = step(index);
            if (std::isspace(static_cast<unsigned char>(text()[prev]))) break;
            index = prev;
        }
    }
    return index;
}
size_t TextEditor::after(size_t index, bool word) const {
    auto step = [&](size_t i) {
        if (i >= text().size()) return text().size();
        if (text()[i] == '\r' && i + 1 < text().size() && text()[i + 1] == '\n') ++i;
        ++i;
        while (i < text().size() && (static_cast<unsigned char>(text()[i]) & 192) == 128) ++i;
        return i;
    };
    index = step(index);
    if (word) {
        while (index < text().size() && !std::isspace(static_cast<unsigned char>(text()[index]))) index = step(index);
        while (index < text().size() && std::isspace(static_cast<unsigned char>(text()[index]))) index = step(index);
    }
    return index;
}
void TextEditor::draw(Painter& p) {
    const auto& colors = theme().colors;
    layout();
    const auto lo = std::min(m_caret, m_anchor), hi = std::max(m_caret, m_anchor);
    const auto rect = contentRect();
    const auto clip = p.clipBounds();
    const auto first = static_cast<size_t>(
        std::clamp(std::floor((clip.y - rect.y) / font().lineHeight), 0.0f, static_cast<float>(m_lines.size())));
    const auto last = static_cast<size_t>(
        std::clamp(std::ceil((clip.bottom() - rect.y) / font().lineHeight), 0.0f, static_cast<float>(m_lines.size())));
    const auto metrics = fontMetrics(font());
    const float baseline = (font().lineHeight - metrics.ascender - metrics.descender) * .5f + metrics.ascender;
    const auto currentRow = caretLine();
    for (size_t row = first; row < last; ++row) {
        const auto& line = m_lines[row];
        const float y = rect.y + static_cast<float>(row) * font().lineHeight;
        if (lo == hi && row == currentRow)
            p.fillRect({rect.x, y, rect.width, font().lineHeight}, colors.label.withAlpha(.035f));
        if (lo < hi && lo <= line.end && hi > line.begin) {
            caretLayout(row);
            const auto xFor = [&](size_t index) {
                auto it = std::lower_bound(line.boundaries.begin(), line.boundaries.end(),
                                           std::clamp(index, line.begin, line.end) - line.begin);
                return line.positions[static_cast<size_t>(it - line.boundaries.begin())];
            };
            const float x = xFor(lo);
            const float end = xFor(hi) + (hi > line.end ? 5.0f : 0.0f);
            p.fillRect({rect.x + x, y, std::max(2.0f, end - x), font().lineHeight}, colors.textSelection);
        }
        if (m_lineNumbers && !line.continuation)
            p.drawText(std::to_string(line.number), Rect{frame().x + 4, y, 26, font().lineHeight}, font(),
                       colors.tertiaryLabel, TextAlign::Trailing);
        auto value = std::string_view(text()).substr(line.begin, line.end - line.begin);
        float x = rect.x;
        size_t start = 0;
        while (start < value.size()) {
            size_t end = start + 1;
            Color color = colors.label;
            if (m_highlight && value[start] == '"') {
                end = value.find('"', start + 1);
                end = end == std::string_view::npos ? value.size() : end + 1;
                color = colors.green.mix(colors.label, .45f);
            } else if (m_highlight && (value[start] == '<' || value[start] == '>')) {
                color = colors.teal.mix(colors.label, .35f);
            } else if (m_highlight && (value[start] == '#' || value[start] == '.')) {
                while (end < value.size() &&
                       (std::isalnum(static_cast<unsigned char>(value[end])) || value[end] == '-' || value[end] == '_'))
                    ++end;
                color = colors.green.mix(colors.yellow, .3f).mix(colors.label, .2f);
            } else if (m_highlight && (std::isalpha(static_cast<unsigned char>(value[start])) || value[start] == '_')) {
                while (end < value.size() && (std::isalnum(static_cast<unsigned char>(value[end])) ||
                                              value[end] == '-' || value[end] == ':' || value[end] == '_'))
                    ++end;
                auto next = end;
                while (next < value.size() && value[next] == ' ') ++next;
                if (next < value.size() && value[next] == '=')
                    color = colors.pink.mix(colors.label, .3f);
                else if (start &&
                         (value[start - 1] == '<' || (value[start - 1] == '/' && start > 1 && value[start - 2] == '<')))
                    color = colors.teal.mix(colors.label, .35f);
                if (value.find("<!DOCTYPE") != std::string_view::npos && start < value.find('"'))
                    color = colors.orange.mix(colors.label, .2f);
            } else {
                if (!m_highlight)
                    end = value.size();
                else
                    while (end < value.size() && (static_cast<unsigned char>(value[end]) & 192) == 128) ++end;
            }
            const auto token = value.substr(start, end - start);
            p.drawText(token, Point{x, y + baseline}, font(), color);
            x += p.measureText(token, font());
            start = end;
        }
    }
    if (isFocused() && m_editable && isComposing()) {
        drawPreedit(p);
        const auto point = inputPosition();
        if (host()) host()->setTextInput(true, {point.x, point.y, 1, font().lineHeight});
    } else if (isFocused() && m_editable && lo == hi) {
        const auto point = caretPosition();
        if (std::fmod(std::max(0.0, now() - m_blink), 1.0) < .55)
            p.fillRect({point.x, point.y + 2, 1, font().lineHeight - 3}, colors.accent);
        if (host()) host()->setTextInput(true, {point.x, point.y, 1, font().lineHeight});
        if (host()) host()->scheduleFrame(now() + .5);
    }
    if (m_reveal) {
        m_reveal = false;
        revealCaret();
    }
}
void TextEditor::move(size_t index, bool extending) {
    cancelComposition();
    m_caret = std::min(index, text().size());
    if (!extending) m_anchor = m_caret;
    m_blink = now();
    m_reveal = true;
    if (m_changed) m_changed(text());
    invalidate();
}
void TextEditor::selectAll() {
    cancelComposition();
    m_anchor = 0;
    m_caret = text().size();
    invalidate();
}
bool TextEditor::onPointerDown(const PointerEvent& e) {
    if (e.button != MouseButton::Left) return false;
    const auto index = at(e.position);
    move(index, e.modifiers == Modifiers::Shift);
    m_dragging = true;
    if (e.clickCount >= 3) {
        selectAll();
        m_dragging = false;
    } else if (e.clickCount == 2) {
        m_anchor = before(after(index), true);
        m_caret = after(index, true);
        m_dragging = false;
    }
    return true;
}
void TextEditor::onPointerMove(const PointerEvent& e) {
    if (m_dragging) move(at(e.position), true);
}
void TextEditor::onPointerUp(const PointerEvent&) { m_dragging = false; }
void TextEditor::apply(const UndoRecord& record, bool undo) {
    if (record.edits.size() == 1) {
        const auto& edit = record.edits.front();
        m_value.replace(edit.offset, undo ? edit.inserted.size() : edit.removed.size(),
                        undo ? edit.removed : edit.inserted);
    } else {
        // A replace-all transaction is linear, not one tail shift per match.
        // Only the resulting buffer is built; history retains the changed spans.
        std::string result;
        result.reserve(m_value.size());
        size_t cursor = 0, originalCursor = 0;
        for (const auto& edit : record.edits) {
            if (undo) {
                const auto unchanged = edit.offset - originalCursor;
                result.append(m_value, cursor, unchanged);
                result += edit.removed;
                cursor += unchanged + edit.inserted.size();
                originalCursor = edit.offset + edit.removed.size();
            } else {
                result.append(m_value, cursor, edit.offset - cursor);
                result += edit.inserted;
                cursor = edit.offset + edit.removed.size();
            }
        }
        result.append(m_value, cursor, std::string::npos);
        m_value = std::move(result);
    }
    m_caret = undo ? record.beforeCaret : record.afterCaret;
    m_anchor = undo ? record.beforeAnchor : record.afterAnchor;
}
void TextEditor::commit(UndoRecord record) {
    for (const auto& old : m_redo) m_historyBytes -= old.bytes;
    m_redo.clear();
    for (const auto& edit : record.edits) record.bytes += sizeof(Edit) + edit.removed.size() + edit.inserted.size();
    m_historyBytes += record.bytes;
    apply(record, false);
    m_undo.push_back(std::move(record));
    // Keep a single large transaction undoable, but never accumulate many full
    // document replacements. Small keystrokes retain much deeper history.
    constexpr size_t kHistoryBudget = 8 * 1024 * 1024, kHistoryRecords = 1000;
    while (m_undo.size() > 1 && (m_historyBytes > kHistoryBudget || m_undo.size() > kHistoryRecords)) {
        m_historyBytes -= m_undo.front().bytes;
        m_undo.erase(m_undo.begin());
    }
    changed();
}
void TextEditor::replaceSelection(std::string value) {
    const size_t lo = std::min(m_caret, m_anchor), hi = std::max(m_caret, m_anchor);
    if (lo == hi && value.empty()) return;
    UndoRecord record{{{lo, text().substr(lo, hi - lo), std::move(value)}}, m_caret, m_anchor, 0, 0};
    record.afterCaret = record.afterAnchor = lo + record.edits.front().inserted.size();
    commit(std::move(record));
}
void TextEditor::insert(std::string value) {
    cancelComposition();
    if (m_editable) replaceSelection(std::move(value));
}
void TextEditor::changed() {
    ++m_version;
    m_blink = now();
    m_reveal = true;
    if (m_changed) m_changed(text());
    invalidateLayout();
}
void TextEditor::restore(bool undo) {
    auto& from = undo ? m_undo : m_redo;
    auto& to = undo ? m_redo : m_undo;
    if (from.empty()) return;
    auto record = std::move(from.back());
    from.pop_back();
    apply(record, undo);
    to.push_back(std::move(record));
    changed();
}
bool TextEditor::onTextInput(const TextInputEvent& e) {
    if (!m_editable) return false;
    cancelComposition();
    if (!e.text.empty()) insert(e.text);
    return true;
}
bool TextEditor::onTextEditing(const TextEditingEvent& e) {
    if (!m_editable) return false;
    if (e.text.empty()) {
        cancelComposition();
        return true;
    }
    if (!isComposing()) m_preeditOffset = std::min(m_caret, m_anchor);
    m_preedit = e.text;
    const auto byteOffset = [&](size_t characters) {
        size_t i = 0;
        while (characters && i < m_preedit.size()) {
            ++i;
            while (i < m_preedit.size() && (static_cast<unsigned char>(m_preedit[i]) & 192) == 128) ++i;
            --characters;
        }
        return i;
    };
    const auto start = static_cast<size_t>(std::max(0, e.start));
    const auto length = static_cast<size_t>(std::max(0, e.length));
    m_preeditStart = byteOffset(start);
    m_preeditEnd = byteOffset(start + length);
    m_preeditDirty = true;
    m_reveal = true;
    if (host()) {
        const auto point = inputPosition();
        host()->setTextInput(true, {point.x, point.y, 1, font().lineHeight});
    }
    invalidate();
    return true;
}
void TextEditor::cancelComposition() {
    if (!isComposing()) return;
    m_preedit.clear();
    m_preeditLines.clear();
    m_preeditStart = m_preeditEnd = 0;
    m_preeditDirty = true;
    invalidate();
}
bool TextEditor::onKeyDown(const KeyEvent& e) {
    if (isComposing()) return true;
    if (e.primary() && e.key == Key::A) {
        selectAll();
        return true;
    }
    if (e.primary() && (e.key == Key::C || e.key == Key::X)) {
        if (m_caret != m_anchor && host()) {
            host()->setClipboardText(
                text().substr(std::min(m_caret, m_anchor), std::max(m_caret, m_anchor) - std::min(m_caret, m_anchor)));
            if (e.key == Key::X && m_editable) {
                replaceSelection("");
            }
        }
        return true;
    }
    if (e.primary() && e.key == Key::V && m_editable) {
        if (host()) insert(host()->clipboardText());
        return true;
    }
    if (e.primary() && e.key == Key::Z && m_editable) {
        if (e.shift())
            restore(false);
        else
            restore(true);
        return true;
    }
    if (e.primary() && e.key == Key::Y && m_editable) {
        restore(false);
        return true;
    }
    if (e.key == Key::Left || e.key == Key::Right) {
        size_t target = e.key == Key::Left ? before(m_caret, e.primary()) : after(m_caret, e.primary());
        if (!e.shift() && m_caret != m_anchor)
            target = e.key == Key::Left ? std::min(m_caret, m_anchor) : std::max(m_caret, m_anchor);
        move(target, e.shift());
        return true;
    }
    if (e.key == Key::Home || e.key == Key::End) {
        size_t target = e.key == Key::Home ? 0 : text().size();
        if (!e.primary()) {
            const auto row = caretLine();
            target = e.key == Key::Home ? m_lines[row].begin : m_lines[row].end;
        }
        move(target, e.shift());
        return true;
    }
    if (e.key == Key::Up || e.key == Key::Down) {
        auto here = caretPosition();
        float targetY = here.y + (e.key == Key::Down ? font().lineHeight : -font().lineHeight);
        move(at({here.x, targetY + font().lineHeight * .5f}), e.shift());
        return true;
    }
    if (!m_editable) return false;
    if (e.key == Key::Tab) {
        insert("    ");
        return true;
    }
    if (e.key == Key::Enter) {
        insert("\n");
        return true;
    }
    if (e.key == Key::Backspace || e.key == Key::Delete) {
        const auto oldCaret = m_caret, oldAnchor = m_anchor;
        if (m_caret == m_anchor) {
            m_anchor = e.key == Key::Backspace ? before(m_caret, e.primary()) : after(m_caret, e.primary());
        }
        const size_t lo = std::min(m_caret, m_anchor), hi = std::max(m_caret, m_anchor);
        if (lo != hi)
            commit({{{lo, text().substr(lo, hi - lo), ""}}, oldCaret, oldAnchor, lo, lo});
        else
            m_anchor = oldAnchor;
        return true;
    }
    return false;
}
void TextEditor::onFocusChanged(bool focused) {
    if (!focused) cancelComposition();
    m_blink = now();
    if (host()) host()->setTextInput(focused && m_editable, frame());
    invalidate();
}
void TextEditor::revealCaret() {
    ScrollViewView* scroll = nullptr;
    for (auto* p = parent(); p; p = p->parent())
        if ((scroll = dynamic_cast<ScrollViewView*>(p))) break;
    if (!scroll) return;
    const auto point = inputPosition();
    const auto v = scroll->frame();
    float dy = 0, dx = 0;
    if (point.y < v.y + 6)
        dy = point.y - v.y - 6;
    else if (point.y + font().lineHeight > v.bottom() - 6)
        dy = point.y + font().lineHeight - v.bottom() + 6;
    if (!m_wrap) {
        if (point.x < v.x + 6)
            dx = point.x - v.x - 6;
        else if (point.x > v.right() - 6)
            dx = point.x - v.right() + 6;
    }
    if (dy || dx)
        scroll->scrollTo({std::max(0.0f, scroll->offset().x + dx), std::max(0.0f, scroll->offset().y + dy)}, false);
}
void TextEditor::setSelection(size_t anchor, size_t caret) {
    auto boundary = [&](size_t i) {
        i = std::min(i, text().size());
        while (i && i < text().size() && (static_cast<unsigned char>(text()[i]) & 192) == 128) --i;
        return i;
    };
    m_anchor = boundary(anchor);
    move(boundary(caret), true);
}
bool TextEditor::find(std::string_view query, bool backwards) {
    if (query.empty()) return false;
    size_t index = backwards ? text().rfind(query, std::min(m_anchor, m_caret) ? std::min(m_anchor, m_caret) - 1
                                                                               : std::string::npos)
                             : text().find(query, std::max(m_anchor, m_caret));
    if (index == std::string::npos) index = backwards ? text().rfind(query) : text().find(query);
    if (index == std::string::npos) return false;
    setSelection(index, index + query.size());
    return true;
}
size_t TextEditor::replaceAll(std::string_view query, std::string_view replacement) {
    if (!m_editable || query.empty()) return 0;
    cancelComposition();
    // Copy potentially aliased views before mutating the buffer.
    const std::string needle(query), inserted(replacement);
    UndoRecord record{{}, m_caret, m_anchor, 0, 0};
    size_t index = 0;
    while ((index = text().find(needle, index)) != std::string::npos) {
        record.edits.push_back({index, needle, inserted});
        index += needle.size();
    }
    const auto count = record.edits.size();
    if (count) commit(std::move(record));
    return count;
}
void TextEditor::setLineNumbers(bool on) {
    if (m_lineNumbers == on) return;
    m_lineNumbers = on;
    setPadding(Insets(7, on ? 40 : 16, 20, 16));
    invalidateLayout();
}
void TextEditor::setWrap(bool on) {
    if (m_wrap == on) return;
    m_wrap = on;
    m_preeditDirty = true;
    invalidateLayout();
}
void TextEditor::setEditorFont(float size) {
    m_font = Font{size, FontWeight::Regular, size * 1.5f, systemFontFamily("monospace").value_or(0)};
    invalidateLayout();
}
void TextEditor::statistics() const {
    if (m_statisticsVersion == m_version) return;
    m_statisticsVersion = m_version;
    m_characters = 0;
    m_lineStarts = {0};
    for (size_t i = 0; i < text().size(); ++i) {
        if ((static_cast<unsigned char>(text()[i]) & 192) != 128) ++m_characters;
        if (text()[i] == '\n' || (text()[i] == '\r' && (i + 1 == text().size() || text()[i + 1] != '\n')))
            m_lineStarts.push_back(i + 1);
    }
}
size_t TextEditor::lineCount() const {
    statistics();
    return m_lineStarts.size();
}
size_t TextEditor::characterCount() const {
    statistics();
    return m_characters;
}
std::pair<size_t, size_t> TextEditor::location() const {
    statistics();
    auto it = std::upper_bound(m_lineStarts.begin(), m_lineStarts.end(), m_caret);
    const auto line = static_cast<size_t>(it - m_lineStarts.begin());
    size_t column = 1;
    for (size_t i = m_lineStarts[line - 1]; i < m_caret; ++i)
        if ((static_cast<unsigned char>(text()[i]) & 192) != 128) ++column;
    return {line, column};
}
std::string TextEditor::selection() const {
    return text().substr(std::min(m_caret, m_anchor), std::max(m_caret, m_anchor) - std::min(m_caret, m_anchor));
}
}  // namespace forma::text
