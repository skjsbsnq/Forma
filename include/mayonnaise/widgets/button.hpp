#pragma once

#include <functional>
#include <string>

#include "../control.hpp"
#include "../icon.hpp"

namespace mayo {

enum class ButtonStyle : std::uint8_t {
    Default,      ///< white/gray push button
    Prominent,    ///< accent-filled (default action)
    Borderless,   ///< no bezel until hovered (toolbars, inline actions)
    Destructive,  ///< red-filled
    Glass,        ///< Liquid Glass capsule (floating controls over content)
};

class ButtonView : public ControlView {
public:
    explicit ButtonView(std::string title);

    void setTitle(std::string title);
    [[nodiscard]] const std::string& title() const { return m_title; }
    void setIcon(Icon icon);
    [[nodiscard]] Icon icon() const { return m_icon; }
    [[nodiscard]] ButtonStyle style() const { return m_style; }
    void setStyle(ButtonStyle style);
    void setOnClick(std::function<void()> fn) { m_onClick = std::move(fn); }
    void click();

    Size measureContent(Size available) override;
    void draw(Painter& painter) override;
    bool onPointerDown(const PointerEvent& e) override;
    void onPointerUp(const PointerEvent& e) override;
    bool onKeyDown(const KeyEvent& e) override;

private:
    std::string m_title;
    Icon m_icon = Icon::None;
    ButtonStyle m_style = ButtonStyle::Default;
    std::function<void()> m_onClick;
};

class Button : public WidgetHandle<Button, ButtonView> {
public:
    explicit Button(std::string title = {}) : WidgetHandle(std::make_shared<ButtonView>(std::move(title))) {}
    Button(Icon icon, std::string title = {}) : Button(std::move(title)) { m_node->setIcon(icon); }

    Button& style(ButtonStyle s) { return apply([&](ButtonView& n) { n.setStyle(s); }); }
    Button& icon(Icon i) { return apply([&](ButtonView& n) { n.setIcon(i); }); }
    Button& controlSize(ControlSize s) { return apply([&](ButtonView& n) { n.setControlSize(s); }); }
    Button& onClick(std::function<void()> fn) { return apply([&](ButtonView& n) { n.setOnClick(std::move(fn)); }); }

    void setTitle(std::string title) { m_node->setTitle(std::move(title)); }
};

}  // namespace mayo
