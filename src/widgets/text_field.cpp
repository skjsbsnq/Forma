#include "mayonnaise/ui_translation.hpp"
#include "mayonnaise/widgets/text_field.hpp"

#include <cmath>

#include "../core/utf8.hpp"
#include "../render/font.hpp"
#include "mayonnaise/icon.hpp"
#include "mayonnaise/app.hpp"
#include "mayonnaise/painter.hpp"

namespace mayo {

namespace {
constexpr char32_t kBullet = 0x2022;
constexpr double kBlinkPeriod = 1.0;
constexpr float kPadH = 7;
constexpr float kSearchIcon = 26;
constexpr float kClearButton = 22;

Font fieldFont() { return theme().font(TextStyle::Body); }
}  // namespace

TextFieldView::TextFieldView(std::string placeholder) : m_placeholder(uiText(placeholder)) {
    setCursor(CursorShape::IBeam);
}

void TextFieldView::setText(std::string text) {
    if (text == m_text) return;
    m_text = std::move(text);
    m_caret = m_anchor = m_text.size();
    m_cacheDirty = true;
    invalidate();
}

void TextFieldView::setPlaceholder(std::string p) {
    m_placeholder = uiText(p);
    invalidate();
}

void TextFieldView::setStyle(TextFieldStyle style) {
    m_style = style;
    invalidateLayout();
}

void TextFieldView::setSecure(bool secure) {
    m_secure = secure;
    m_cacheDirty = true;
    invalidate();
}

void TextFieldView::selectAll() {
    m_anchor = 0;
    m_caret = m_text.size();
    invalidate();
}

std::string TextFieldView::displayText() const {
    if (!m_secure) return m_text;
    std::string out;
    size_t i = 0;
    while (i < m_text.size()) {
        utf8::next(m_text, i);
        utf8::append(out, kBullet);
    }
    return out;
}

void TextFieldView::updateLayoutCache() {
    if (!m_cacheDirty) return;
    m_cacheDirty = false;
    auto& lib = render::FontLibrary::instance();
    const Font f = fieldFont();
    m_boundaries.assign(1, 0);
    m_positions.assign(1, 0.0f);
    float x = 0;
    size_t i = 0;
    while (i < m_text.size()) {
        char32_t cp = utf8::next(m_text, i);
        x += lib.advance(m_secure ? kBullet : cp, f);
        m_boundaries.push_back(i);
        m_positions.push_back(x);
    }
}

Size TextFieldView::measureContent(Size available) {
    const float h = controlHeight();
    float w = std::isfinite(available.width) ? std::max(available.width, 60.0f) : 180.0f;
    return {w, h};
}

Rect TextFieldView::textRect() const {
    Rect c = contentRect();
    float left = kPadH;
    float right = kPadH;
    if (m_style == TextFieldStyle::Search) {
        left = kSearchIcon;
        right = kClearButton;
    } else if (m_style == TextFieldStyle::Plain) {
        left = right = 2;
    }
    return {c.x + left, c.y, std::max(0.0f, c.width - left - right), c.height};
}

Rect TextFieldView::clearButtonRect() const {
    Rect c = contentRect();
    return {c.right() - kClearButton + 3, c.center().y - 8, 16, 16};
}

void TextFieldView::focusRingShape(Rect& rect, CornerRadii& radii) const {
    rect = frame();
    radii = CornerRadii(m_style == TextFieldStyle::Search ? rect.height * 0.5f : theme().metrics.controlRadius);
}

float TextFieldView::xForIndex(size_t index) {
    updateLayoutCache();
    for (size_t k = 0; k < m_boundaries.size(); ++k) {
        if (m_boundaries[k] >= index) return m_positions[k];
    }
    return m_positions.back();
}

size_t TextFieldView::indexAt(float x) {
    updateLayoutCache();
    float local = x - textRect().x + m_scrollX;
    size_t best = 0;
    float bestDist = 1e9f;
    for (size_t k = 0; k < m_boundaries.size(); ++k) {
        float d = std::abs(m_positions[k] - local);
        if (d < bestDist) {
            bestDist = d;
            best = m_boundaries[k];
        }
    }
    return best;
}

size_t TextFieldView::wordStart(size_t from) const {
    size_t i = from;
    // Skip separators, then the word.
    while (i > 0) {
        size_t p = utf8::prevBoundary(m_text, i);
        size_t k = p;
        if (utf8::isWordChar(utf8::next(m_text, k))) break;
        i = p;
    }
    while (i > 0) {
        size_t p = utf8::prevBoundary(m_text, i);
        size_t k = p;
        if (!utf8::isWordChar(utf8::next(m_text, k))) break;
        i = p;
    }
    return i;
}

size_t TextFieldView::wordEnd(size_t from) const {
    size_t i = from;
    while (i < m_text.size()) {
        size_t k = i;
        if (utf8::isWordChar(utf8::next(m_text, k))) break;
        i = k;
    }
    while (i < m_text.size()) {
        size_t k = i;
        if (!utf8::isWordChar(utf8::next(m_text, k))) break;
        i = k;
    }
    return i;
}

void TextFieldView::resetBlink() {
    m_blinkStart = now();
    invalidate();
}

void TextFieldView::ensureCaretVisible() {
    const float w = textRect().width;
    float x = xForIndex(m_caret);
    if (x - m_scrollX > w - 2) m_scrollX = x - w + 2;
    if (x - m_scrollX < 0) m_scrollX = x;
    float total = m_positions.empty() ? 0.0f : m_positions.back();
    m_scrollX = std::clamp(m_scrollX, 0.0f, std::max(0.0f, total - w + 2));
}

void TextFieldView::changed() {
    m_cacheDirty = true;
    ensureCaretVisible();
    resetBlink();
    if (m_onChange) m_onChange(m_text);
}

void TextFieldView::moveCaret(size_t to, bool extend) {
    m_caret = std::min(to, m_text.size());
    if (!extend) m_anchor = m_caret;
    ensureCaretVisible();
    resetBlink();
}

void TextFieldView::deleteSelection() {
    size_t a = std::min(m_anchor, m_caret);
    size_t b = std::max(m_anchor, m_caret);
    m_text.erase(a, b - a);
    m_caret = m_anchor = a;
}

void TextFieldView::insert(std::string_view s) {
    std::string clean;
    for (char c : s) {
        if (c != '\n' && c != '\r' && c != '\t') clean += c;
    }
    if (clean.empty() && !hasSelection()) return;
    if (hasSelection()) deleteSelection();
    m_text.insert(m_caret, clean);
    m_caret += clean.size();
    m_anchor = m_caret;
    changed();
}

void TextFieldView::deleteBackward(bool word) {
    if (hasSelection()) {
        deleteSelection();
    } else if (m_caret > 0) {
        size_t from = word ? wordStart(m_caret) : utf8::prevBoundary(m_text, m_caret);
        m_text.erase(from, m_caret - from);
        m_caret = m_anchor = from;
    } else {
        return;
    }
    changed();
}

void TextFieldView::deleteForward(bool word) {
    if (hasSelection()) {
        deleteSelection();
    } else if (m_caret < m_text.size()) {
        size_t to = word ? wordEnd(m_caret) : utf8::nextBoundary(m_text, m_caret);
        m_text.erase(m_caret, to - m_caret);
    } else {
        return;
    }
    changed();
}

void TextFieldView::draw(Painter& p) {
    const Theme& t = theme();
    const auto& c = t.colors;
    const bool dark = t.isDark();
    const Rect r = frame();
    const bool focused = isFocused() && host() && host()->isKeyWindow();
    const Font f = fieldFont();

    // Bezel.
    if (m_style == TextFieldStyle::Rounded) {
        const float radius = t.metrics.controlRadius;
        Color border = dark ? Color::white(0.12f) : Color::black(0.13f);
        p.fillRoundedRect(r, radius, c.controlBackground, border, 0.5f);
        // Faint inner shadow along the top edge gives the field its inset look.
        if (!dark) {
            p.pushClip({r.x, r.y, r.width, 3});
            p.strokeRoundedRect(r.inset(0.5f), radius, Color::black(0.04f), 1.0f);
            p.popClip();
        }
    } else if (m_style == TextFieldStyle::Search) {
        const float radius = r.height * 0.5f;
        Color fill = dark ? Color::white(0.08f + 0.03f * hoverAmount()) : Color::black(0.05f + 0.015f * hoverAmount());
        if (focused) fill = c.controlBackground;
        p.fillRoundedRect(r, radius, fill, focused ? c.separator : Color::transparent(), 0.5f);
        p.drawIcon(Icon::Magnifier, {r.x + 9, r.center().y - 6.5f, 13, 13}, c.secondaryLabel);
        if (!m_text.empty()) {
            Rect cb = clearButtonRect();
            p.fillCircle(cb.center(), 7, c.tertiaryLabel.mix(c.secondaryLabel, m_clearPressed ? 1.0f : 0.0f));
            p.drawIcon(Icon::Xmark, cb.inset(4.2f), dark ? Color::black(0.8f) : Color::white(), 1.4f);
        }
    }

    const Rect tr = textRect();
    p.pushClip({tr.x - 1, tr.y, tr.width + 2, tr.height});
    updateLayoutCache();
    const auto m = render::FontLibrary::instance().metrics(f);
    const float baseline = r.center().y + m.capHeight * 0.5f;
    const float x0 = tr.x - m_scrollX;

    if (m_text.empty()) {
        if (!m_placeholder.empty()) p.drawText(m_placeholder, {x0, baseline}, f, c.placeholder);
    } else {
        if (hasSelection()) {
            float a = xForIndex(std::min(m_anchor, m_caret));
            float b = xForIndex(std::max(m_anchor, m_caret));
            Color sel = focused ? c.textSelection : c.unemphasizedSelection;
            p.fillRect({x0 + a, r.center().y - f.lineHeight * 0.5f, b - a, f.lineHeight}, sel);
        }
        p.drawText(displayText(), {x0, baseline}, f, isEnabled() ? c.label : c.tertiaryLabel);
    }

    if (focused && !hasSelection()) {
        const double phase = std::fmod(now() - m_blinkStart, kBlinkPeriod);
        if (phase < kBlinkPeriod * 0.5) {
            float cx = p.snap(x0 + xForIndex(m_caret));
            p.fillRect({cx - 0.75f, baseline - m.ascender + 1, 1.5f, m.ascender + m.descender - 1}, c.accent);
        }
        // Wake up for the next blink edge only; idle CPU stays near zero.
        double next = now() + (phase < kBlinkPeriod * 0.5 ? kBlinkPeriod * 0.5 - phase : kBlinkPeriod - phase);
        if (auto* h = host()) {
            h->scheduleFrame(next);
            Rect caretRect{x0 + xForIndex(m_caret), r.y, 2, r.height};
            h->setTextInput(true, caretRect);
        }
    }
    p.popClip();

    if (focused && m_style != TextFieldStyle::Plain) {
        Rect fr;
        CornerRadii radii;
        focusRingShape(fr, radii);
        p.drawFocusRing(fr, radii, 1.0f);
    }
}

bool TextFieldView::onPointerDown(const PointerEvent& e) {
    if (e.button != MouseButton::Left) return false;
    if (m_style == TextFieldStyle::Search && !m_text.empty() && clearButtonRect().outset(3).contains(e.position)) {
        m_clearPressed = true;
        invalidate();
        return true;
    }
    size_t i = indexAt(e.position.x);
    if (e.clickCount >= 3) {
        selectAll();
    } else if (e.clickCount == 2) {
        // Select the word under the pointer (or the single separator character).
        auto isWordAt = [&](size_t at) {
            size_t k = at;
            return at < m_text.size() && utf8::isWordChar(utf8::next(m_text, k));
        };
        size_t a = i;
        size_t b = i;
        if (isWordAt(i) || (i > 0 && isWordAt(utf8::prevBoundary(m_text, i)))) {
            while (a > 0 && isWordAt(utf8::prevBoundary(m_text, a))) a = utf8::prevBoundary(m_text, a);
            while (isWordAt(b)) b = utf8::nextBoundary(m_text, b);
        } else {
            b = utf8::nextBoundary(m_text, i);
        }
        m_anchor = a;
        m_caret = b;
    } else {
        moveCaret(i, hasModifier(e.modifiers, Modifiers::Shift));
        m_dragging = true;
    }
    resetBlink();
    return true;
}

void TextFieldView::onPointerMove(const PointerEvent& e) {
    if (!m_dragging) return;
    // Auto-scroll when dragging past the edges.
    moveCaret(indexAt(e.position.x), true);
}

void TextFieldView::onPointerUp(const PointerEvent& e) {
    m_dragging = false;
    if (m_clearPressed) {
        m_clearPressed = false;
        if (clearButtonRect().outset(3).contains(e.position)) {
            m_text.clear();
            m_caret = m_anchor = 0;
            m_scrollX = 0;
            changed();
        }
        invalidate();
    }
}

bool TextFieldView::onKeyDown(const KeyEvent& e) {
    const bool shift = e.shift();
    const bool word = e.primary() || e.alt();
    switch (e.key) {
        case Key::Left:
            if (hasSelection() && !shift)
                moveCaret(std::min(m_anchor, m_caret), false);
            else
                moveCaret(word ? wordStart(m_caret) : utf8::prevBoundary(m_text, m_caret), shift);
            return true;
        case Key::Right:
            if (hasSelection() && !shift)
                moveCaret(std::max(m_anchor, m_caret), false);
            else
                moveCaret(word ? wordEnd(m_caret) : utf8::nextBoundary(m_text, m_caret), shift);
            return true;
        case Key::Home:
        case Key::Up: moveCaret(0, shift); return true;
        case Key::End:
        case Key::Down: moveCaret(m_text.size(), shift); return true;
        case Key::Backspace: deleteBackward(word); return true;
        case Key::Delete: deleteForward(word); return true;
        case Key::Enter:
            if (m_onSubmit) m_onSubmit(m_text);
            return true;
        case Key::Escape:
            if (m_style == TextFieldStyle::Search && !m_text.empty()) {
                m_text.clear();
                m_caret = m_anchor = 0;
                changed();
                return true;
            }
            return false;
        default: break;
    }
    if (e.primary()) {
        auto selected = [&] {
            size_t a = std::min(m_anchor, m_caret);
            return m_text.substr(a, std::max(m_anchor, m_caret) - a);
        };
        switch (e.key) {
            case Key::A: selectAll(); return true;
            case Key::C:
                if (hasSelection() && !m_secure && host()) host()->setClipboardText(selected());
                return true;
            case Key::X:
                if (hasSelection() && !m_secure && host()) {
                    host()->setClipboardText(selected());
                    deleteSelection();
                    changed();
                }
                return true;
            case Key::V:
                if (host()) insert(host()->clipboardText());
                return true;
            default: break;
        }
    }
    // Printable keys arrive as TextInput; claim them so they don't trigger shortcuts elsewhere.
    return !e.primary() && e.key != Key::Tab && e.key != Key::Unknown;
}

bool TextFieldView::onTextInput(const TextInputEvent& e) {
    insert(e.text);
    return true;
}

void TextFieldView::onFocusChanged(bool focused) {
    ControlView::onFocusChanged(focused);
    if (auto* h = host()) h->setTextInput(focused, frame());
    if (focused) {
        resetBlink();
    } else {
        m_anchor = m_caret;
        m_dragging = false;
    }
    invalidate();
}

}  // namespace mayo
