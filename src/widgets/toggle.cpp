#include "mayonnaise/ui_translation.hpp"
#include "mayonnaise/widgets/toggle.hpp"

#include <cmath>

#include "../render/font.hpp"
#include "mayonnaise/app.hpp"
#include "mayonnaise/painter.hpp"
#include "mayonnaise/widgets/checkbox.hpp"

namespace mayo {

namespace {
constexpr float kLabelGap = 8;
}

// --- BooleanControlView ----------------------------------------------------------------------

BooleanControlView::BooleanControlView(std::string label, bool on)
    : m_label(uiText(label)), m_on(on), m_progress(on ? 1.0f : 0.0f) {}

void BooleanControlView::setOn(bool on, bool animated) {
    if (on == m_on) return;
    m_on = on;
    if (animated)
        m_progress.animateTo(on ? 1.0f : 0.0f, now(), transition());
    else
        m_progress.set(on ? 1.0f : 0.0f);
    invalidate();
}

void BooleanControlView::setLabel(std::string label) {
    m_label = uiText(label);
    invalidateLayout();
}

void BooleanControlView::toggle() {
    if (!isEnabled()) return;
    setOn(!m_on);
    if (m_onChange) m_onChange(m_on);
}

bool BooleanControlView::onPointerDown(const PointerEvent& e) { return e.button == MouseButton::Left; }

void BooleanControlView::onPointerUp(const PointerEvent& e) {
    if (frame().contains(e.position)) toggle();
}

bool BooleanControlView::onKeyDown(const KeyEvent& e) {
    if (e.key == Key::Space && !e.repeat) {
        toggle();
        return true;
    }
    return false;
}

// --- ToggleView ------------------------------------------------------------------------------

Size ToggleView::measureContent(Size /*available*/) {
    const auto& m = theme().metrics;
    float w = m.toggleWidth;
    float h = m.toggleHeight;
    if (!m_label.empty()) {
        Font f = theme().font(TextStyle::Body);
        w += kLabelGap + std::ceil(render::measureText(m_label, f));
        h = std::max(h, f.lineHeight);
    }
    return {w, h};
}

Rect ToggleView::trackRect() const {
    const auto& m = theme().metrics;
    Rect c = contentRect();
    float x = m_label.empty() ? c.x : c.right() - m.toggleWidth;
    return {x, c.center().y - m.toggleHeight * 0.5f, m.toggleWidth, m.toggleHeight};
}

void ToggleView::focusRingShape(Rect& rect, CornerRadii& radii) const {
    rect = trackRect();
    radii = CornerRadii(rect.height * 0.5f);
}

void ToggleView::draw(Painter& p) {
    const Theme& t = theme();
    const auto& c = t.colors;
    const float on = progress();
    const float hover = hoverAmount();
    const float press = pressAmount();
    const Rect track = trackRect();
    const float radius = track.height * 0.5f;

    Color off = c.toggleOff.mix(c.fill, hover * 0.6f);
    Color fill = off.mix(c.accent, std::clamp(on, 0.0f, 1.0f));
    p.fillRoundedRect(track, radius, fill);
    // Inner shadow hint along the top edge of the well.
    p.strokeRoundedRect(track, radius, Color::black(t.isDark() ? 0.15f : 0.05f), 0.5f);

    // Capsule knob that stretches while pressed (Liquid Glass switch).
    const float inset = 2;
    const float knobH = track.height - inset * 2;
    const float knobW = knobH * 1.35f + press * 4.0f;
    const float travel = track.width - inset * 2 - knobW;
    Rect knob{track.x + inset + travel * on, track.y + inset, knobW, knobH};
    p.drawShadow(knob, knobH * 0.5f, Color::black(0.22f), 3.0f, {0, 1});
    p.fillRoundedRect(knob, knobH * 0.5f, c.knob, c.knobBorder, 0.5f);

    if (!m_label.empty()) {
        Rect cr = contentRect();
        Font f = t.font(TextStyle::Body);
        p.drawText(m_label, {cr.x, cr.y, track.x - cr.x - kLabelGap, cr.height}, f,
                   foreground().value_or(c.label));
    }
}

// --- CheckboxView ----------------------------------------------------------------------------

Size CheckboxView::measureContent(Size /*available*/) {
    const auto& m = theme().metrics;
    float w = m.checkboxSize;
    float h = m.checkboxSize;
    if (!m_label.empty()) {
        Font f = theme().font(TextStyle::Body);
        w += 7 + std::ceil(render::measureText(m_label, f));
        h = std::max(h, f.lineHeight);
    }
    return {w, h};
}

Rect CheckboxView::boxRect() const {
    const float s = theme().metrics.checkboxSize;
    Rect c = contentRect();
    return {c.x, std::round(c.center().y - s * 0.5f), s, s};
}

void CheckboxView::focusRingShape(Rect& rect, CornerRadii& radii) const {
    rect = boxRect();
    radii = CornerRadii(4);
}

void CheckboxView::draw(Painter& p) {
    const Theme& t = theme();
    const auto& c = t.colors;
    const bool dark = t.isDark();
    const float on = std::clamp(progress(), 0.0f, 1.0f);
    const float hover = hoverAmount();
    const float press = pressAmount();
    const Rect box = boxRect();
    const float radius = 4;

    Color offFill = dark ? Color::white(0.08f + 0.04f * hover) : Color::white();
    offFill = offFill.mix(c.controlPressed, press * (1 - on));
    Color onFill = c.accent.mix(c.accent.lighter(0.08f), hover).mix(c.accentPressed, press);
    if (!dark) p.drawShadow(box, radius, Color::black(0.10f * (1 - on)), 1.5f, {0, 0.5f});
    p.fillRoundedRect(box, radius, offFill.mix(onFill, on), Color::black(dark ? 0.0f : 0.22f).mix(onFill, on),
                      0.5f);
    if (dark) p.strokeRoundedRect(box, radius, Color::white(0.16f * (1 - on)), 0.5f);

    if (on > 0.01f) {
        // Draw the check progressively: short stroke, then the long one.
        const float u = box.width / 20.0f;
        Point a{box.x + 5.0f * u, box.y + 10.5f * u};
        Point b{box.x + 8.5f * u, box.y + 14.0f * u};
        Point e{box.x + 15.0f * u, box.y + 6.0f * u};
        float l1 = (b - a).length();
        float l2 = (e - b).length();
        float d = on * (l1 + l2);
        const float w = 1.9f * u;
        p.drawLine(a, lerp(a, b, std::min(1.0f, d / l1)), c.onAccent, w);
        if (d > l1) p.drawLine(b, lerp(b, e, std::min(1.0f, (d - l1) / l2)), c.onAccent, w);
    }

    if (!m_label.empty()) {
        Rect cr = contentRect();
        Font f = t.font(TextStyle::Body);
        float x = box.right() + 7;
        p.drawText(m_label, {x, cr.y, cr.right() - x, cr.height}, f, foreground().value_or(c.label));
    }
}

}  // namespace mayo
