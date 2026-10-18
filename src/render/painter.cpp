#include "mayonnaise/painter.hpp"

#include <cmath>
#include <numbers>

#include "font.hpp"
#include "mayonnaise/app.hpp"
#include "renderer2d.hpp"
#include "text_system.hpp"

namespace mayo {

Painter::Painter(render::Renderer2D& renderer, render::TextSystem& text) : m_renderer(renderer), m_text(text) {}

void Painter::fillRect(const Rect& r, const Color& color) { m_renderer.fillRoundedRect(r, {}, color); }

void Painter::fillRoundedRect(const Rect& r, const CornerRadii& radii, const Color& color) {
    m_renderer.fillRoundedRect(r, radii, color);
}

void Painter::fillRoundedRect(const Rect& r, const CornerRadii& radii, const Color& fill, const Color& border,
                              float borderWidth) {
    m_renderer.fillRoundedRect(r, radii, fill, border, borderWidth);
}

void Painter::strokeRoundedRect(const Rect& r, const CornerRadii& radii, const Color& color, float width) {
    m_renderer.strokeRoundedRect(r, radii, color, width);
}

void Painter::fillCircle(Point c, float radius, const Color& color) {
    m_renderer.fillRoundedRect({c.x - radius, c.y - radius, radius * 2, radius * 2}, radius, color);
}

void Painter::strokeCircle(Point c, float radius, const Color& color, float width) {
    m_renderer.arc(c, radius - width * 0.5f, 0, 2.0f * std::numbers::pi_v<float>, color, width);
}

void Painter::fillLinearGradient(const Rect& r, const CornerRadii& radii, const Color& from, const Color& to,
                                 Point start, Point end) {
    m_renderer.fillGradient(r, radii, from, to, start, end);
}

void Painter::fillVerticalGradient(const Rect& r, const CornerRadii& radii, const Color& top, const Color& bottom) {
    m_renderer.fillGradient(r, radii, top, bottom, {r.x, r.top()}, {r.x, r.bottom()});
}

void Painter::drawShadow(const Rect& r, float radius, const Color& color, float blur, Vec2 offset, float spread) {
    m_renderer.shadow(r, radius, color, blur, offset, spread);
}

void Painter::drawLine(Point a, Point b, const Color& color, float width) { m_renderer.line(a, b, color, width); }

void Painter::fillTriangle(Point a, Point b, Point c, const Color& color, float cornerRadius) {
    m_renderer.triangle(a, b, c, color, cornerRadius);
}

void Painter::drawArc(Point center, float radius, float startAngle, float sweep, const Color& color, float width) {
    m_renderer.arc(center, radius, startAngle, sweep, color, width);
}

void Painter::drawFocusRing(const Rect& r, const CornerRadii& radii, float progress) {
    if (progress <= 0.01f) return;
    const Theme& t = theme();
    const float w = t.metrics.focusRingWidth;
    // The ring animates in from slightly outside, like macOS.
    float grow = w + (1.0f - progress) * 6.0f;
    CornerRadii rr(radii.topLeft + grow, radii.topRight + grow, radii.bottomRight + grow, radii.bottomLeft + grow);
    m_renderer.strokeRoundedRect(r.outset(grow), rr, t.colors.focusRing.multipliedAlpha(progress), w);
}

void Painter::drawMaterial(const Rect& r, const CornerRadii& radii, Material material) {
    drawMaterial(r, radii, theme().material(material));
}

void Painter::drawMaterial(const Rect& r, const CornerRadii& radii, const MaterialStyle& style) {
    m_renderer.backdrop(r, radii, style);
    if (style.highlight.a > 0) {
        // Specular rim: bright along the top edge, fading towards the bottom.
        const float h = std::min(r.height, 24.0f);
        m_renderer.pushClip({r.x, r.y, r.width, h});
        m_renderer.strokeRoundedRect(r, radii, style.highlight, 1.0f);
        m_renderer.popClip();
    }
    if (style.border.a > 0) m_renderer.strokeRoundedRect(r, radii, style.border, 0.5f);
}

void Painter::drawScrollEdge(const Rect& r, ScrollEdgeStyle style, bool bottom, float strength) {
    strength = std::clamp(strength, 0.0f, 1.0f);
    if (style == ScrollEdgeStyle::None || strength == 0 || r.width <= 0 || r.height <= 0) return;
    const Color bg = theme().colors.contentBackground;
    if (style == ScrollEdgeStyle::Soft) {
        const Color edge = bg.multipliedAlpha(0.85f * strength);
        fillVerticalGradient(r, {}, bottom ? bg.withAlpha(0) : edge, bottom ? edge : bg.withAlpha(0));
        return;
    }
    MaterialStyle material = theme().material(Material::Titlebar);
    material.blurRadius = 6.4f;
    material.saturation = 1.25f;
    material.tint = bg.withAlpha(0.82f);
    material.highlight = Color::transparent();
    material.border = Color::transparent();
    pushOpacity(strength);
    drawMaterial(r, {}, material);
    const float hairline = 1.0f / scale();
    fillRect({r.x, bottom ? r.y : r.bottom() - hairline, r.width, hairline}, theme().colors.separator);
    popOpacity();
}

void Painter::drawText(std::string_view text, Point baseline, const Font& font, const Color& color) {
    m_text.drawLine(m_renderer, text, baseline, font, color);
}

void Painter::drawText(std::string_view text, const Rect& r, const Font& font, const Color& color, TextAlign align) {
    auto m = render::FontLibrary::instance().metrics(font);
    float width = render::measureText(text, font);
    float x = r.x;
    if (align == TextAlign::Center) x = r.x + (r.width - width) * 0.5f;
    if (align == TextAlign::Trailing) x = r.right() - width;
    // Centre on the cap height rather than the line box: that's what reads as optically centred.
    float baseline = r.center().y + m.capHeight * 0.5f;
    m_text.drawLine(m_renderer, text, {x, baseline}, font, color);
}

float Painter::measureText(std::string_view text, const Font& font) const { return render::measureText(text, font); }

void Painter::pushClip(const Rect& r, float radius) { m_renderer.pushClip(r, radius); }
void Painter::popClip() { m_renderer.popClip(); }
Rect Painter::clipBounds() const { return m_renderer.currentClip(); }
void Painter::pushOpacity(float opacity) { m_renderer.pushOpacity(opacity); }
void Painter::popOpacity() { m_renderer.popOpacity(); }
float Painter::scale() const { return m_renderer.scale(); }
float Painter::snap(float v) const { return std::round(v * m_renderer.scale()) / m_renderer.scale(); }

}  // namespace mayo
