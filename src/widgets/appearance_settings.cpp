#include "mayonnaise/widgets/appearance_settings.hpp"

#include <algorithm>
#include <cmath>
#include <numbers>

#include "mayonnaise/painter.hpp"

namespace mayo {

namespace {

constexpr float kTileW = 68;
constexpr float kTileH = 46;
constexpr float kTileGap = 16;
constexpr float kLabelH = 20;
constexpr const char* kModeNames[3] = {"自动", "浅色", "深色"};
constexpr AppearanceMode kModes[3] = {AppearanceMode::System, AppearanceMode::Light, AppearanceMode::Dark};

constexpr float kDot = 16;
constexpr float kDotGap = 9;

/// A tiny desktop with a window on it, light or dark.
void drawPreview(Painter& p, const Rect& r, bool dark, Color accent) {
    // Wallpaper: a soft diagonal blend tinted by the accent colour.
    const Color top = dark ? Color::hex(0x24324F) : Color::hex(0x8EC5FF);
    const Color bottom = dark ? accent.mix(Color::black(), 0.55f) : accent.mix(Color::white(), 0.35f);
    p.fillLinearGradient(r, 0, top, bottom, {r.x, r.y}, {r.right(), r.bottom()});
    const Rect win{r.x + 8, r.y + 9, r.width - 16, r.height - 14};
    const Color face = dark ? Color::hex(0x2C2C2E) : Color::white();
    const Color sidebar = dark ? Color::hex(0x3A3A3C) : Color::hex(0xE9E9EE);
    p.drawShadow(win, 4, Color::black(0.25f), 4, {0, 1});
    p.fillRoundedRect(win, 4, face);
    p.fillRoundedRect({win.x, win.y, win.width * 0.32f, win.height}, CornerRadii{4, 0, 0, 4}, sidebar);
    for (int i = 0; i < 3; ++i) {
        static constexpr Color kLights[3] = {Color::hex(0xFF5F57), Color::hex(0xFEBC2E), Color::hex(0x28C840)};
        p.fillCircle({win.x + 4.5f + static_cast<float>(i) * 4.5f, win.y + 4.5f}, 1.4f, kLights[i]);
    }
    // A selected row and a couple of text lines.
    p.fillRoundedRect({win.x + 3, win.y + 10, win.width * 0.32f - 6, 4}, 2, accent);
    const Color line = dark ? Color::white(0.25f) : Color::black(0.15f);
    for (int i = 0; i < 3; ++i) {
        p.fillRoundedRect({win.x + win.width * 0.38f, win.y + 7 + static_cast<float>(i) * 6, win.width * (0.5f - 0.1f * static_cast<float>(i)), 2.5f}, 1.25f, line);
    }
}

}  // namespace

// --- AppearancePickerView ------------------------------------------------------------------------

Size AppearancePickerView::measureContent(Size /*available*/) { return {kTileW * 3 + kTileGap * 2 + 8, kTileH + kLabelH + 8}; }

Rect AppearancePickerView::tileRect(int i) const {
    const Rect c = contentRect();
    return {c.x + 4 + static_cast<float>(i) * (kTileW + kTileGap), c.y + 4, kTileW, kTileH};
}

int AppearancePickerView::tileAt(Point p) const {
    for (int i = 0; i < 3; ++i) {
        Rect r = tileRect(i);
        if (Rect{r.x - 4, r.y - 4, r.width + 8, r.height + kLabelH + 6}.contains(p)) return i;
    }
    return -1;
}

void AppearancePickerView::draw(Painter& p) {
    const Theme& t = theme();
    const auto& c = t.colors;
    const AppearanceMode mode = m_app.appearanceMode();
    for (int i = 0; i < 3; ++i) {
        const Rect r = tileRect(i);
        const bool selected = kModes[i] == mode;
        p.pushOpacity(i == m_pressed && isPressed() ? 0.75f : 1.0f);
        p.pushClip(r, 7);
        if (i == 0) {
            // Auto: light on the left, dark on the right.
            p.pushClip({r.x, r.y, r.width * 0.5f, r.height});
            drawPreview(p, r, false, c.accent);
            p.popClip();
            p.pushClip({r.x + r.width * 0.5f, r.y, r.width * 0.5f, r.height});
            drawPreview(p, r, true, c.accent);
            p.popClip();
        } else {
            drawPreview(p, r, i == 2, c.accent);
        }
        p.popClip();
        p.strokeRoundedRect(r, 7, c.separator, 1);
        if (selected) p.strokeRoundedRect(r.outset(3.5f), 10, c.accent, 3);
        else if (i == m_hover) p.strokeRoundedRect(r.outset(3.5f), 10, c.secondaryFill, 3);
        p.popOpacity();
        const Font f = t.font(TextStyle::Subheadline).withWeight(selected ? FontWeight::Semibold : FontWeight::Regular);
        p.drawText(kModeNames[i], {r.x - 8, r.bottom() + 5, r.width + 16, kLabelH - 4}, f,
                   selected ? c.label : c.secondaryLabel, TextAlign::Center);
    }
}

void AppearancePickerView::drawOverlay(Painter& p) {
    // Focus ring around the selected tile rather than the whole control.
    const float a = focusAmount();
    if (a <= 0.01f) return;
    for (int i = 0; i < 3; ++i) {
        if (kModes[i] == m_app.appearanceMode()) p.drawFocusRing(tileRect(i).outset(6), 12, a);
    }
}

bool AppearancePickerView::onPointerDown(const PointerEvent& e) {
    if (e.button != MouseButton::Left) return false;
    m_pressed = tileAt(e.position);
    invalidate();
    return m_pressed >= 0;
}

void AppearancePickerView::onPointerUp(const PointerEvent& e) {
    const int i = tileAt(e.position);
    if (i >= 0 && i == m_pressed) m_app.setAppearanceMode(kModes[i]);
    m_pressed = -1;
    invalidate();
}

void AppearancePickerView::onPointerMove(const PointerEvent& e) {
    const int h = tileAt(e.position);
    if (h != m_hover) {
        m_hover = h;
        invalidate();
    }
}

void AppearancePickerView::onPointerLeave() {
    m_hover = -1;
    invalidate();
}

bool AppearancePickerView::onKeyDown(const KeyEvent& e) {
    int cur = 0;
    for (int i = 0; i < 3; ++i) {
        if (kModes[i] == m_app.appearanceMode()) cur = i;
    }
    if (e.key == Key::Left) cur = std::max(0, cur - 1);
    else if (e.key == Key::Right) cur = std::min(2, cur + 1);
    else return false;
    m_app.setAppearanceMode(kModes[cur]);
    invalidate();
    return true;
}

// --- AccentPickerView ----------------------------------------------------------------------------

Size AccentPickerView::measureContent(Size /*available*/) {
    const auto n = static_cast<float>(accentChoices().size() + 1);
    return {n * kDot + (n - 1) * kDotGap + 8, kDot + 26};
}

Point AccentPickerView::dotCenter(int i) const {
    const Rect c = contentRect();
    return {c.x + 4 + kDot * 0.5f + static_cast<float>(i) * (kDot + kDotGap), c.y + 4 + kDot * 0.5f};
}

int AccentPickerView::dotAt(Point p) const {
    const int n = static_cast<int>(accentChoices().size()) + 1;
    for (int i = 0; i < n; ++i) {
        Point c = dotCenter(i);
        if (std::hypot(p.x - c.x, p.y - c.y) <= kDot * 0.5f + kDotGap * 0.5f) return i;
    }
    return -1;
}

int AccentPickerView::selectedIndex() const {
    if (m_app.accentFollowsSystem()) return 0;
    const auto choices = accentChoices();
    for (size_t i = 0; i < choices.size(); ++i) {
        if (choices[i].color == m_app.accentColor()) return static_cast<int>(i) + 1;
    }
    return -1;
}

void AccentPickerView::choose(int i) {
    if (i == 0) m_app.setAccentFollowsSystem(true);
    else if (i > 0 && i <= static_cast<int>(accentChoices().size())) m_app.setAccentColor(accentChoices()[static_cast<size_t>(i - 1)].color);
    invalidate();
}

void AccentPickerView::draw(Painter& p) {
    const Theme& t = theme();
    const auto& c = t.colors;
    const auto choices = accentChoices();
    const int selected = selectedIndex();
    const int n = static_cast<int>(choices.size()) + 1;
    for (int i = 0; i < n; ++i) {
        const Point ctr = dotCenter(i);
        const Rect dot{ctr.x - kDot * 0.5f, ctr.y - kDot * 0.5f, kDot, kDot};
        p.pushOpacity(i == m_pressed && isPressed() ? 0.7f : 1.0f);
        p.drawShadow(dot, kDot * 0.5f, Color::black(0.15f), 2, {0, 0.5f});
        if (i == 0) {
            // "跟随系统": a ring of every palette colour.
            const float step = 2 * std::numbers::pi_v<float> / static_cast<float>(choices.size());
            for (size_t k = 0; k < choices.size(); ++k) {
                p.drawArc(ctr, kDot * 0.25f, -std::numbers::pi_v<float> / 2 + step * static_cast<float>(k), step * 1.05f,
                          choices[k].color, kDot * 0.5f);
            }
        } else {
            p.fillCircle(ctr, kDot * 0.5f, choices[static_cast<size_t>(i - 1)].color);
        }
        p.strokeCircle(ctr, kDot * 0.5f, Color::black(0.12f), 0.5f);
        if (i == selected) p.fillCircle(ctr, 3, Color::white());
        else if (i == m_hover) p.strokeCircle(ctr, kDot * 0.5f + 2.5f, c.secondaryFill, 2);
        p.popOpacity();
    }
    // Name of the hovered (or selected) colour under the row, like System Settings.
    const int named = m_hover >= 0 ? m_hover : selected;
    if (named >= 0) {
        std::string name = named == 0 ? "跟随系统" : choices[static_cast<size_t>(named - 1)].name;
        const Rect c2 = contentRect();
        p.drawText(name, {c2.x, c2.y + kDot + 10, c2.width, 16}, t.font(TextStyle::Caption), c.secondaryLabel,
                   TextAlign::Center);
    }
}

void AccentPickerView::drawOverlay(Painter& p) {
    const float a = focusAmount();
    const int i = selectedIndex();
    if (a <= 0.01f || i < 0) return;
    const Point ctr = dotCenter(i);
    const float r = kDot * 0.5f + 2;
    p.drawFocusRing({ctr.x - r, ctr.y - r, r * 2, r * 2}, r, a);
}

bool AccentPickerView::onPointerDown(const PointerEvent& e) {
    if (e.button != MouseButton::Left) return false;
    m_pressed = dotAt(e.position);
    invalidate();
    return m_pressed >= 0;
}

void AccentPickerView::onPointerUp(const PointerEvent& e) {
    const int i = dotAt(e.position);
    if (i >= 0 && i == m_pressed) choose(i);
    m_pressed = -1;
    invalidate();
}

void AccentPickerView::onPointerMove(const PointerEvent& e) {
    const int h = dotAt(e.position);
    if (h != m_hover) {
        m_hover = h;
        invalidate();
    }
}

void AccentPickerView::onPointerLeave() {
    m_hover = -1;
    invalidate();
}

bool AccentPickerView::onKeyDown(const KeyEvent& e) {
    const int n = static_cast<int>(accentChoices().size()) + 1;
    const int cur = std::max(0, selectedIndex());
    if (e.key == Key::Left) choose(std::max(0, cur - 1));
    else if (e.key == Key::Right) choose(std::min(n - 1, cur + 1));
    else return false;
    return true;
}

// --- section -------------------------------------------------------------------------------------------

FormSection AppearanceSettings(App& app) {
    return FormSection("外观", {FormRow("外观", AppearancePicker(app)), FormRow("强调色", AccentPicker(app))});
}

}  // namespace mayo
