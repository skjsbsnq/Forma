#pragma once

#include <functional>
#include <string>
#include <vector>

#include "../control.hpp"
#include "../icon.hpp"

namespace mayo {

struct Segment {
    std::string title;
    Icon icon = Icon::None;
    Segment(std::string t) : title(std::move(t)) {}  // NOLINT(google-explicit-constructor)
    Segment(const char* t) : title(t) {}             // NOLINT(google-explicit-constructor)
    Segment(Icon i) : icon(i) {}                     // NOLINT(google-explicit-constructor)
};

class SegmentedControlView : public ControlView {
public:
    SegmentedControlView(std::vector<Segment> segments, int selected);
    void setSelected(int index, bool notify = false);
    [[nodiscard]] int selected() const { return m_selected; }
    [[nodiscard]] const std::vector<Segment>& segments() const { return m_segments; }
    void setOnChange(std::function<void(int)> fn) { m_onChange = std::move(fn); }

    Size measureContent(Size available) override;
    void draw(Painter& painter) override;
    bool onPointerDown(const PointerEvent& e) override;
    void onPointerMove(const PointerEvent& e) override;
    void onPointerUp(const PointerEvent& e) override;
    bool onKeyDown(const KeyEvent& e) override;

private:
    [[nodiscard]] Rect segmentRect(int index) const;
    [[nodiscard]] int segmentAt(Point p) const;

    std::vector<Segment> m_segments;
    int m_selected;
    int m_pressedSegment = -1;
    int m_hoverSegment = -1;
    Animated<float> m_knob;  // fractional segment index
    std::function<void(int)> m_onChange;
};

class SegmentedControl : public WidgetHandle<SegmentedControl, SegmentedControlView> {
public:
    SegmentedControl(std::initializer_list<Segment> segments, int selected = 0)  // NOLINT
        : WidgetHandle(std::make_shared<SegmentedControlView>(std::vector<Segment>(segments), selected)) {}
    SegmentedControl& onChange(std::function<void(int)> fn) {
        return apply([&](SegmentedControlView& n) { n.setOnChange(std::move(fn)); });
    }
    SegmentedControl& selected(int i) { return apply([&](SegmentedControlView& n) { n.setSelected(i); }); }
    [[nodiscard]] int selectedIndex() const { return m_node->selected(); }
};

}  // namespace mayo
