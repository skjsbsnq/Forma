#pragma once

#include "../layout.hpp"
#include "menu.hpp"

namespace mayo {

class ButtonView;

/// Row of toolbar items living in the window's unified title bar. Empty space stays draggable.
class ToolbarView : public StackView {
public:
    ToolbarView();
    void setOverflowEnabled(bool enabled);
    [[nodiscard]] std::vector<MenuItem> overflowItems() const;
    Size measureContent(Size available) override;
    void arrangeContent(const Rect& content) override;
    void draw(Painter& painter) override;
    bool onKeyDown(const KeyEvent& e) override;
    [[nodiscard]] bool expandsContent(Axis axis) const override { return axis == Axis::Horizontal; }

private:
    bool appendActions(const std::shared_ptr<Widget>& widget, std::vector<MenuItem>& items) const;
    void updateOverflow(Size available);
    void showOverflow();
    void restoreFocus();
    bool m_overflowEnabled = true;
    std::shared_ptr<ButtonView> m_more;
    std::shared_ptr<MenuView> m_menu;
    Rect m_menuAnchor;
};

/// Items grouped on one Liquid Glass capsule (macOS 26 toolbar style).
class ToolbarGroupView : public StackView {
public:
    ToolbarGroupView();
    void draw(Painter& painter) override;
};

class Toolbar : public detail::ContainerHandle<Toolbar, ToolbarView> {
public:
    Toolbar(std::initializer_list<Element> items = {})  // NOLINT(google-explicit-constructor)
        : ContainerHandle(std::make_shared<ToolbarView>()) {
        addAll(items);
    }
    Toolbar& spacing(float s) {
        return apply([&](ToolbarView& n) { n.setSpacing(s); });
    }
    /// Move trailing actions into a menu when space runs out; custom inputs stay in the toolbar.
    Toolbar& overflow(bool enabled = true) {
        return apply([&](ToolbarView& n) { n.setOverflowEnabled(enabled); });
    }
};

class ToolbarGroup : public detail::ContainerHandle<ToolbarGroup, ToolbarGroupView> {
public:
    ToolbarGroup(std::initializer_list<Element> items = {})  // NOLINT(google-explicit-constructor)
        : ContainerHandle(std::make_shared<ToolbarGroupView>()) {
        addAll(items);
    }
};

}  // namespace mayo
