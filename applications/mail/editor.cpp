#include "editor.hpp"
#include <algorithm>
#include <cctype>
#include <cmath>
#include "../../src/render/font.hpp"

using namespace mayo;
namespace forma::mail {
TextEditor::TextEditor(std::string value, bool editable) : LabelView(std::move(value)), m_editable(editable) {
    setFont(Font{13, FontWeight::Regular, 20});
    setCursor(CursorShape::IBeam);
    setPadding(Insets(3, 1));
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
    auto size = LabelView::measureContent(available);
    return {std::isfinite(available.width) ? available.width : size.width,
            std::max(size.height, m_editable ? 200.0f : 20.0f)};
}
std::vector<TextEditor::Stop> TextEditor::stops() const {
    const auto rect = contentRect();
    const auto lines = render::wrapText(text(), font(), rect.width + .5f);
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
    for (size_t i = 0; i < points.size(); ++i) {
        auto& s = points[i];
        if (s.index < lo || s.index >= hi) continue;
        float width = 5;
        if (i + 1 < points.size() && points[i + 1].position.y == s.position.y)
            width = points[i + 1].position.x - s.position.x;
        p.fillRect({s.position.x, s.position.y, std::max(2.0f, width), font().lineHeight}, colors.textSelection);
    }
    LabelView::draw(p);
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
}  // namespace forma::mail
