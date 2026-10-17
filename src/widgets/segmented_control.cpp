#include "mayonnaise/ui_translation.hpp"
#include "mayonnaise/widgets/segmented_control.hpp"

#include <cmath>

#include "../render/font.hpp"
#include "mayonnaise/app.hpp"
#include "mayonnaise/painter.hpp"

namespace mayo {

namespace {
constexpr float kInset = 2;
constexpr float kSegPad = 14;
}  // namespace

SegmentedControlView::SegmentedControlView(std::vector<Segment> segments, int selected)
    : m_segments(std::move(segments)), m_selected(selected), m_knob(static_cast<float>(selected)) {
    for (auto& segment : m_segments) segment.title = uiText(segment.title);
}

void SegmentedControlView::setSelected(int index, bool notify) {
    if (m_segments.empty()) return;
    index = std::clamp(index, 0, static_cast<int>(m_segments.size()) - 1);
    if (index == m_selected) return;
    m_selected = index;
    m_knob.animateTo(static_cast<float>(index), now(), Transition::spring(0.32f));
    invalidate();
    if (notify && m_onChange) m_onChange(index);
}

Size SegmentedControlView::measureContent(Size /*available*/) {
    const Font f = theme().font(TextStyle::Body);
    float w = 0;
    for (const auto& s : m_segments) {
        float sw = s.title.empty() ? 16.0f : render::measureText(s.title, f);
        if (!s.title.empty() && s.icon != Icon::None) sw += 19;
        w = std::max(w, sw);
    }
    // Equal-width segments, like NSSegmentedControl's "fill equally".
    float segW = std::ceil(w) + kSegPad * 2;
    return {segW * static_cast<float>(m_segments.size()) + kInset * 2, controlHeight()};
}

Rect SegmentedControlView::segmentRect(int index) const {
    Rect r = frame().inset(kInset);
    float w = r.width / static_cast<float>(std::max<size_t>(1, m_segments.size()));
    return {r.x + w * static_cast<float>(index), r.y, w, r.height};
}

int SegmentedControlView::segmentAt(Point p) const {
    for (int i = 0; i < static_cast<int>(m_segments.size()); ++i) {
        if (segmentRect(i).contains(p)) return i;
    }
    return -1;
}

void SegmentedControlView::draw(Painter& p) {
    const Theme& t = theme();
    const auto& c = t.colors;
    const bool dark = t.isDark();
    const Rect r = frame();
    const float radius = r.height * 0.5f;

    p.fillRoundedRect(r, radius, dark ? Color::white(0.08f) : Color::black(0.06f));

    // Hover wash on non-selected segments.
    if (m_hoverSegment >= 0 && m_hoverSegment != m_selected) {
        Rect h = segmentRect(m_hoverSegment);
        p.fillRoundedRect(h, h.height * 0.5f, c.hover.multipliedAlpha(hoverAmount()));
    }

    // Sliding knob between segments (springs, slight stretch while moving).
    float k = sample(m_knob);
    Rect a = segmentRect(static_cast<int>(std::floor(k)));
    Rect b = segmentRect(std::min(static_cast<int>(std::floor(k)) + 1, static_cast<int>(m_segments.size()) - 1));
    Rect knob = lerp(a, b, k - std::floor(k));
    const float press = m_pressedSegment == m_selected ? pressAmount() : 0.0f;
    knob = knob.inset(press * 1.0f, press * 0.5f);
    p.drawShadow(knob, knob.height * 0.5f, Color::black(dark ? 0.35f : 0.14f), 3, {0, 1});
    p.fillRoundedRect(knob, knob.height * 0.5f, dark ? Color::white(0.22f) : Color::white(), 
                      dark ? Color::white(0.08f) : Color::black(0.04f), 0.5f);

    const Font f = t.font(TextStyle::Body);
    for (int i = 0; i < static_cast<int>(m_segments.size()); ++i) {
        const auto& s = m_segments[static_cast<size_t>(i)];
        Rect sr = segmentRect(i);
        Color text = c.label;
        Font sf = i == m_selected ? f.withWeight(FontWeight::Medium) : f;
        float tw = s.title.empty() ? 0.0f : p.measureText(s.title, sf);
        float iw = s.icon != Icon::None ? 14.0f : 0.0f;
        float total = tw + iw + (tw > 0 && iw > 0 ? 5.0f : 0.0f);
        float x = sr.center().x - total * 0.5f;
        if (iw > 0) {
            p.drawIcon(s.icon, {x, sr.center().y - 7, 14, 14}, text);
            x += iw + 5;
        }
        if (tw > 0) p.drawText(s.title, {p.snap(x), sr.y, tw, sr.height}, sf, text);
        // Hairline separators between unselected neighbours (hidden next to the knob).
        if (i > 0 && i != m_selected && i - 1 != m_selected) {
            float px = 1.0f / p.scale();
            p.fillRect({p.snap(sr.x), sr.y + 6, px, sr.height - 12}, c.separator);
        }
    }
}

bool SegmentedControlView::onPointerDown(const PointerEvent& e) {
    if (e.button != MouseButton::Left) return false;
    m_pressedSegment = segmentAt(e.position);
    return m_pressedSegment >= 0;
}

void SegmentedControlView::onPointerMove(const PointerEvent& e) {
    int s = segmentAt(e.position);
    if (s != m_hoverSegment) {
        m_hoverSegment = s;
        invalidate();
    }
}

void SegmentedControlView::onPointerUp(const PointerEvent& e) {
    int s = segmentAt(e.position);
    if (s >= 0 && s == m_pressedSegment) setSelected(s, true);
    m_pressedSegment = -1;
}

bool SegmentedControlView::onKeyDown(const KeyEvent& e) {
    if (e.key == Key::Left) {
        setSelected(m_selected - 1, true);
        return true;
    }
    if (e.key == Key::Right) {
        setSelected(m_selected + 1, true);
        return true;
    }
    return false;
}

}  // namespace mayo
