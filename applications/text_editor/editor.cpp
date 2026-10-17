#include "editor.hpp"
#include <algorithm>
#include <cctype>
#include <cmath>
#include "../../src/render/font.hpp"

using namespace mayo;
namespace forma::text {
TextEditor::TextEditor(std::string value, bool editable) : LabelView(""), m_editable(editable) {
    setTextTranslationEnabled(false);
    setText(std::move(value));
    setEditorFont(13);
    setCursor(CursorShape::IBeam);
    setPadding(Insets(7, 40, 20, 16));
}
void TextEditor::setValue(std::string value) {
    setText(std::move(value));
    m_caret = std::min(m_caret, text().size());
    m_anchor = m_caret;
    m_undo.clear();
    m_redo.clear();
}
void TextEditor::setOnChange(std::function<void(const std::string&)> callback) { m_changed = std::move(callback); }
Size TextEditor::measureContent(Size available) {
    auto size = LabelView::measureContent(m_wrap ? available : Size{kInfinity, available.height});
    if (!m_wrap && std::isfinite(available.width)) size.width = std::max(size.width, available.width);
    return {m_wrap && std::isfinite(available.width) ? available.width : size.width,
            std::max(size.height, m_editable ? 200.0f : 20.0f)};
}
std::vector<TextEditor::Stop> TextEditor::stops() const {
    const auto rect = contentRect();
    const auto lines = render::wrapText(text(), font(), m_wrap ? rect.width + .5f : kInfinity);
    std::vector<Stop> result;
    float y = rect.y;
    for (const auto& line : lines) {
        std::vector<size_t> boundaries;
        std::vector<float> positions;
        render::caretPositions(std::string_view(text()).substr(line.begin, line.end - line.begin), font(), boundaries,
                               positions);
        for (size_t i = 0; i < boundaries.size(); ++i)
            result.push_back({line.begin + boundaries[i], {rect.x + positions[i], y}});
        y += font().lineHeight;
    }
    if (result.empty()) result.push_back({0, {rect.x, rect.y}});
    return result;
}
size_t TextEditor::at(Point point) const {
    size_t index = 0;
    float nearest = kInfinity;
    for (const auto& stop : stops()) {
        float dy = std::abs(stop.position.y + font().lineHeight * .5f - point.y);
        float score = dy * 10000 + std::abs(stop.position.x - point.x);
        if (score < nearest) {
            nearest = score;
            index = stop.index;
        }
    }
    return index;
}
size_t TextEditor::before(size_t index, bool word) const {
    if (!index) return 0;
    auto step = [&](size_t i) {
        --i;
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
    const auto points = stops();
    const auto lo = std::min(m_caret, m_anchor), hi = std::max(m_caret, m_anchor);
    if (lo == hi) {
        for (auto it = points.rbegin(); it != points.rend(); ++it)
            if (it->index == m_caret) {
                p.fillRect({contentRect().x, it->position.y, contentRect().width, font().lineHeight},
                           colors.label.withAlpha(.035f));
                break;
            }
    }
    for (size_t i = 0; i < points.size(); ++i) {
        auto& s = points[i];
        if (s.index < lo || s.index >= hi) continue;
        float width = 5;
        if (i + 1 < points.size() && points[i + 1].position.y == s.position.y)
            width = points[i + 1].position.x - s.position.x;
        p.fillRect({s.position.x, s.position.y, std::max(2.0f, width), font().lineHeight}, colors.textSelection);
    }
    const auto rect = contentRect();
    const auto lines = render::wrapText(text(), font(), m_wrap ? rect.width + .5f : kInfinity);
    const float baseline = (font().lineHeight - fontMetrics(font()).ascender - fontMetrics(font()).descender) * .5f +
                           fontMetrics(font()).ascender;
    float y = rect.y;
    size_t lineNumber = 1;
    size_t previousEnd = 0;
    for (const auto& line : lines) {
        bool continuation = line.begin > 0 && text()[line.begin - 1] != '\n';
        if (line.begin > previousEnd && text()[line.begin - 1] == '\n') ++lineNumber;
        if (y + font().lineHeight >= p.clipBounds().y && y <= p.clipBounds().bottom()) {
            if (m_lineNumbers && !continuation)
                p.drawText(std::to_string(lineNumber), Rect{frame().x + 4, y, 26, font().lineHeight}, font(),
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
                    while (end < value.size() && (std::isalnum(static_cast<unsigned char>(value[end])) ||
                                                  value[end] == '-' || value[end] == '_'))
                        ++end;
                    color = colors.green.mix(colors.yellow, .3f).mix(colors.label, .2f);
                } else if (m_highlight &&
                           (std::isalpha(static_cast<unsigned char>(value[start])) || value[start] == '_')) {
                    while (end < value.size() && (std::isalnum(static_cast<unsigned char>(value[end])) ||
                                                  value[end] == '-' || value[end] == ':' || value[end] == '_'))
                        ++end;
                    auto next = end;
                    while (next < value.size() && value[next] == ' ') ++next;
                    if (next < value.size() && value[next] == '=')
                        color = colors.pink.mix(colors.label, .3f);
                    else if (start && (value[start - 1] == '<' ||
                                       (value[start - 1] == '/' && start > 1 && value[start - 2] == '<')))
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
        previousEnd = line.end;
        y += font().lineHeight;
    }
    if (isFocused() && m_editable && lo == hi) {
        for (auto it = points.rbegin(); it != points.rend(); ++it)
            if (it->index == m_caret) {
                if (std::fmod(std::max(0.0, now() - m_blink), 1.0) < .55)
                    p.fillRect({it->position.x, it->position.y + 2, 1, font().lineHeight - 3}, colors.accent);
                if (host()) host()->setTextInput(true, {it->position.x, it->position.y, 1, font().lineHeight});
                break;
            }
        if (host()) host()->scheduleFrame(now() + .5);
    }
    if (m_reveal) {
        m_reveal = false;
        revealCaret();
    }
}
void TextEditor::move(size_t index, bool extending) {
    m_caret = std::min(index, text().size());
    if (!extending) m_anchor = m_caret;
    m_blink = now();
    m_reveal = true;
    if (m_changed) m_changed(text());
    invalidate();
}
void TextEditor::selectAll() {
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
void TextEditor::remember() {
    m_undo.push_back({text(), m_caret, m_anchor});
    if (m_undo.size() > 100) m_undo.erase(m_undo.begin());
    m_redo.clear();
}
void TextEditor::replaceSelection(std::string value) {
    const size_t lo = std::min(m_caret, m_anchor), hi = std::max(m_caret, m_anchor);
    auto content = text();
    content.replace(lo, hi - lo, value);
    setText(std::move(content));
    m_caret = lo + value.size();
    m_anchor = m_caret;
    changed();
}
void TextEditor::insert(std::string value) {
    if (!m_editable) return;
    remember();
    replaceSelection(std::move(value));
}
void TextEditor::changed() {
    m_blink = now();
    m_reveal = true;
    if (m_changed) m_changed(text());
    invalidateLayout();
}
void TextEditor::restore(std::vector<Snapshot>& from, std::vector<Snapshot>& to) {
    if (from.empty()) return;
    to.push_back({text(), m_caret, m_anchor});
    auto s = std::move(from.back());
    from.pop_back();
    setText(std::move(s.value));
    m_caret = s.caret;
    m_anchor = s.anchor;
    changed();
}
bool TextEditor::onTextInput(const TextInputEvent& e) {
    if (!m_editable) return false;
    insert(e.text);
    return true;
}
bool TextEditor::onKeyDown(const KeyEvent& e) {
    if (e.primary() && e.key == Key::A) {
        selectAll();
        return true;
    }
    if (e.primary() && (e.key == Key::C || e.key == Key::X)) {
        if (m_caret != m_anchor && host()) {
            host()->setClipboardText(
                text().substr(std::min(m_caret, m_anchor), std::max(m_caret, m_anchor) - std::min(m_caret, m_anchor)));
            if (e.key == Key::X && m_editable) {
                remember();
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
            restore(m_redo, m_undo);
        else
            restore(m_undo, m_redo);
        return true;
    }
    if (e.primary() && e.key == Key::Y && m_editable) {
        restore(m_redo, m_undo);
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
            const auto points = stops();
            float y = 0;
            for (auto& s : points)
                if (s.index == m_caret) y = s.position.y;
            for (auto& s : points)
                if (s.position.y == y) {
                    if (e.key == Key::Home) {
                        target = s.index;
                        break;
                    }
                    target = s.index;
                }
        }
        move(target, e.shift());
        return true;
    }
    if (e.key == Key::Up || e.key == Key::Down) {
        const auto points = stops();
        Point here{};
        for (auto& s : points)
            if (s.index == m_caret) here = s.position;
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
        remember();
        if (m_caret == m_anchor) {
            m_anchor = e.key == Key::Backspace ? before(m_caret, e.primary()) : after(m_caret, e.primary());
        }
        replaceSelection("");
        return true;
    }
    return false;
}
void TextEditor::onFocusChanged(bool focused) {
    m_blink = now();
    if (host()) host()->setTextInput(focused && m_editable, frame());
    invalidate();
}
void TextEditor::revealCaret() {
    ScrollViewView* scroll = nullptr;
    for (auto* p = parent(); p; p = p->parent())
        if ((scroll = dynamic_cast<ScrollViewView*>(p))) break;
    if (!scroll) return;
    auto points = stops();
    for (auto it = points.rbegin(); it != points.rend(); ++it)
        if (it->index == m_caret) {
            auto v = scroll->frame();
            float dy = 0;
            if (it->position.y < v.y + 6)
                dy = it->position.y - v.y - 6;
            else if (it->position.y + font().lineHeight > v.bottom() - 6)
                dy = it->position.y + font().lineHeight - v.bottom() + 6;
            if (dy) scroll->scrollTo({scroll->offset().x, std::max(0.0f, scroll->offset().y + dy)}, false);
            break;
        }
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
    if (query.empty()) return 0;
    auto value = text();
    size_t count = 0, index = 0;
    while ((index = value.find(query, index)) != std::string::npos) {
        value.replace(index, query.size(), replacement);
        index += replacement.size();
        ++count;
    }
    if (count) {
        remember();
        setText(std::move(value));
        m_caret = m_anchor = 0;
        changed();
    }
    return count;
}
void TextEditor::setLineNumbers(bool on) {
    m_lineNumbers = on;
    setPadding(Insets(7, on ? 40 : 16, 20, 16));
    invalidateLayout();
}
void TextEditor::setWrap(bool on) {
    m_wrap = on;
    invalidateLayout();
}
void TextEditor::setEditorFont(float size) {
    setFont(Font{size, FontWeight::Regular, size * 1.5f, systemFontFamily("monospace").value_or(0)});
}
size_t TextEditor::lineCount() const { return 1 + static_cast<size_t>(std::count(text().begin(), text().end(), '\n')); }
size_t TextEditor::characterCount() const {
    return static_cast<size_t>(
        std::count_if(text().begin(), text().end(), [](unsigned char c) { return (c & 192) != 128; }));
}
std::pair<size_t, size_t> TextEditor::location() const {
    size_t line = 1, column = 1;
    for (size_t i = 0; i < m_caret; ++i) {
        if (text()[i] == '\n') {
            ++line;
            column = 1;
        } else if ((static_cast<unsigned char>(text()[i]) & 192) != 128)
            ++column;
    }
    return {line, column};
}
std::string TextEditor::selection() const {
    return text().substr(std::min(m_caret, m_anchor), std::max(m_caret, m_anchor) - std::min(m_caret, m_anchor));
}
}  // namespace forma::text
