#include "mayonnaise/ui_translation.hpp"
#include "mayonnaise/widgets/label.hpp"

#include <cmath>

#include "../render/font.hpp"
#include "mayonnaise/app.hpp"

namespace mayo {

Color resolveLabelColor(LabelColor role) {
    const auto& c = theme().colors;
    switch (role) {
        case LabelColor::Primary: return c.label;
        case LabelColor::Secondary: return c.secondaryLabel;
        case LabelColor::Tertiary: return c.tertiaryLabel;
        case LabelColor::Accent: return c.accent;
        case LabelColor::Destructive: return c.destructive;
        case LabelColor::OnAccent: return c.onAccent;
    }
    return c.label;
}

LabelView::LabelView(std::string text) : m_text(uiText(text)) {}

void LabelView::setText(std::string text) {
    if (m_translateText) text = uiText(text);
    if (text == m_text) return;
    m_text = std::move(text);
    m_linesDirty = true;
    invalidateLayout();
}

void LabelView::setTextStyle(TextStyle style) {
    m_style = style;
    m_font.reset();
    m_linesDirty = true;
    invalidateLayout();
}

void LabelView::setFont(const Font& font) {
    m_font = font;
    m_linesDirty = true;
    invalidateLayout();
}

void LabelView::setWeight(FontWeight weight) {
    m_weight = weight;
    m_linesDirty = true;
    invalidateLayout();
}

void LabelView::setTextAlign(TextAlign align) {
    m_align = align;
    invalidate();
}

void LabelView::setLineLimit(int lines) {
    m_lineLimit = lines;
    m_linesDirty = true;
    invalidateLayout();
}

void LabelView::setColorRole(LabelColor role) {
    m_role = role;
    invalidate();
}

Font LabelView::font() const {
    Font f = m_font ? *m_font : theme().font(m_style);
    if (m_weight) f.weight = *m_weight;
    return f;
}

Color LabelView::textColor() const { return foreground().value_or(resolveLabelColor(m_role)); }

void LabelView::wrap(float width) {
    if (!m_linesDirty && width == m_wrapWidth) return;
    m_linesDirty = false;
    m_wrapWidth = width;
    m_lines.clear();
    const Font f = font();
    if (m_lineLimit == 1) {
        m_lines.push_back({0, m_text.size(), render::measureText(m_text, f)});
        return;
    }
    for (const auto& l : render::wrapText(m_text, f, width)) m_lines.push_back({l.begin, l.end, l.width});
    if (m_lineLimit > 0 && static_cast<int>(m_lines.size()) > m_lineLimit) {
        m_lines.resize(static_cast<size_t>(m_lineLimit));
    }
}

float LabelView::naturalWidth() const { return std::ceil(render::measureText(m_text, font())); }

Size LabelView::measureContent(Size available) {
    wrap(available.width);
    float w = 0;
    for (const auto& l : m_lines) w = std::max(w, l.width);
    const Font f = font();
    // Round up so text never gets clipped by fractional layout.
    return {std::ceil(m_lineLimit == 1 ? w : std::min(w, available.width)),
            static_cast<float>(std::max<size_t>(1, m_lines.size())) * f.lineHeight};
}

void LabelView::draw(Painter& p) {
    const Rect r = contentRect();
    wrap(r.width + 0.5f);
    const Font f = font();
    const auto m = render::FontLibrary::instance().metrics(f);
    const Color color = textColor();
    // Centre the ascender+descender box inside each line box.
    const float baselineOffset = (f.lineHeight - (m.ascender + m.descender)) * 0.5f + m.ascender;
    const bool clip = m_lineLimit == 1 && !m_lines.empty() && m_lines[0].width > r.width + 0.5f;
    if (clip) p.pushClip(r.outset(1));
    float y = r.y;
    for (const auto& l : m_lines) {
        std::string_view line(m_text.data() + l.begin, l.end - l.begin);
        float x = r.x;
        if (m_align == TextAlign::Center) x = r.x + (r.width - l.width) * 0.5f;
        if (m_align == TextAlign::Trailing) x = r.right() - l.width;
        p.drawText(line, {x, y + baselineOffset}, f, color);
        y += f.lineHeight;
    }
    if (clip) p.popClip();
}

}  // namespace mayo
