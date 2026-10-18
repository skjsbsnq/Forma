#pragma once

#include <functional>
#include <string>

#include "../control.hpp"

namespace mayo {

/// Shared base for two-state controls with an optional text label (switch, checkbox).
class BooleanControlView : public ControlView {
public:
    BooleanControlView(std::string label, bool on);
    void setOn(bool on, bool animated = true);
    [[nodiscard]] bool isOn() const { return m_on; }
    [[nodiscard]] const std::string& label() const { return m_label; }
    void setLabel(std::string label);
    void setOnChange(std::function<void(bool)> fn) { m_onChange = std::move(fn); }

    bool onPointerDown(const PointerEvent& e) override;
    void onPointerUp(const PointerEvent& e) override;
    bool onKeyDown(const KeyEvent& e) override;

protected:
    void toggle();
    [[nodiscard]] float progress() { return sample(m_progress); }
    virtual Transition transition() const { return Transition::spring(); }

    std::string m_label;
    bool m_on;
    Animated<float> m_progress;
    std::function<void(bool)> m_onChange;
};

/// macOS switch.
class ToggleView : public BooleanControlView {
public:
    using BooleanControlView::BooleanControlView;
    Size measureContent(Size available) override;
    void draw(Painter& painter) override;

protected:
    void focusRingShape(Rect& rect, CornerRadii& radii) const override;

private:
    [[nodiscard]] Rect trackRect() const;
};

class Toggle : public WidgetHandle<Toggle, ToggleView> {
public:
    explicit Toggle(std::string label = {}, bool on = false)
        : WidgetHandle(std::make_shared<ToggleView>(std::move(label), on)) {}
    Toggle& isOn(bool on) { return apply([&](ToggleView& n) { n.setOn(on, false); }); }
    Toggle& onChange(std::function<void(bool)> fn) {
        return apply([&](ToggleView& n) { n.setOnChange(std::move(fn)); });
    }
    void setOn(bool on) { m_node->setOn(on); }
    [[nodiscard]] bool on() const { return m_node->isOn(); }
};

}  // namespace mayo
