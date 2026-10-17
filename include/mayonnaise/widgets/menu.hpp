#include "mayonnaise/ui_translation.hpp"
#pragma once

#include <functional>
#include <string>
#include <vector>

#include "../control.hpp"
#include "../icon.hpp"
#include "../popup.hpp"

namespace mayo {

class ImageData;

struct MenuItem {
    std::string title;
    std::function<void()> action;
    Icon icon = Icon::None;
    std::string shortcut;  ///< display only, e.g. "Ctrl+C"
    std::string detail;    ///< second text column after the title ("推荐"), unlike the shortcut it isn't pushed right
    std::vector<MenuItem> subitems;  ///< submenu on hover; the item's action is ignored
    std::shared_ptr<ImageData> image;  ///< drawn like `icon`, 16 pt, when set
    bool enabled = true;
    bool checked = false;
    bool destructive = false;
    bool isSeparator = false;

    MenuItem() = default;
    MenuItem(std::string t, std::function<void()> a = {}) : title(uiText(t)), action(std::move(a)) {}  // NOLINT
    static MenuItem separator() {
        MenuItem m;
        m.isSeparator = true;
        return m;
    }
    MenuItem& withIcon(Icon i) {
        icon = i;
        return *this;
    }
    MenuItem& withShortcut(std::string s) {
        shortcut = std::move(s);
        return *this;
    }
    MenuItem& withDetail(std::string d) {
        detail = std::move(d);
        return *this;
    }
    /// A themed icon image shown before the title (an application's icon, as macOS does in "Open With").
    MenuItem& withImage(std::shared_ptr<ImageData> i) {
        image = std::move(i);
        return *this;
    }
    MenuItem& withSubmenu(std::vector<MenuItem> items) {
        subitems = std::move(items);
        return *this;
    }
    [[nodiscard]] bool hasSubmenu() const { return !subitems.empty(); }
    MenuItem& disabled(bool d = true) {
        enabled = !d;
        return *this;
    }
    MenuItem& asDestructive() {
        destructive = true;
        return *this;
    }
    MenuItem& setChecked(bool c = true) {
        checked = c;
        return *this;
    }
};

/// The floating menu panel itself.
class MenuView : public PopupView {
public:
    explicit MenuView(std::vector<MenuItem> items);
    void setItems(std::vector<MenuItem> items);
    [[nodiscard]] const std::vector<MenuItem>& items() const { return m_items; }
    void setHighlighted(int index);

    Size measureContent(Size available) override;
    [[nodiscard]] bool acceptsFocus() const override { return true; }
    [[nodiscard]] bool isInteractive() const override { return true; }
    bool onPointerDown(const PointerEvent& e) override;
    void onPointerMove(const PointerEvent& e) override;
    void onPointerUp(const PointerEvent& e) override;
    void onPointerLeave() override;
    bool onKeyDown(const KeyEvent& e) override;

protected:
    void drawPanel(Painter& painter) override;

private:
    [[nodiscard]] int itemAt(Point p) const;
    [[nodiscard]] Rect rowRect(int index) const;
    void activate(int index);
    void moveHighlight(int delta);
    void showSubmenu(int index);
    void closeSubmenu();

    std::vector<MenuItem> m_items;
    int m_highlighted = -1;
    int m_pressed = -1;
    std::shared_ptr<MenuView> m_submenu;
    int m_submenuIndex = -1;
};

/// Pull-down menu button: a button with a chevron that opens a list of actions.
class MenuButtonView : public ControlView {
public:
    MenuButtonView(std::string title, std::vector<MenuItem> items);
    void setItems(std::vector<MenuItem> items) { m_items = std::move(items); }
    void setBorderless(bool b);
    void setIcon(Icon icon);
    void open();

    Size measureContent(Size available) override;
    void draw(Painter& painter) override;
    bool onPointerDown(const PointerEvent& e) override;
    bool onKeyDown(const KeyEvent& e) override;

private:
    std::string m_title;
    Icon m_icon = Icon::None;
    std::vector<MenuItem> m_items;
    bool m_borderless = false;
    std::shared_ptr<MenuView> m_menu;
};

/// Pop-up button: shows the current choice and a checkmarked menu of alternatives.
class PickerView : public ControlView {
public:
    explicit PickerView(std::vector<std::string> options, int selected);
    void setSelected(int index, bool notify = false);
    [[nodiscard]] int selected() const { return m_selected; }
    void setOptions(std::vector<std::string> options);
    void setOnChange(std::function<void(int)> fn) { m_onChange = std::move(fn); }
    void open();

    Size measureContent(Size available) override;
    void draw(Painter& painter) override;
    bool onPointerDown(const PointerEvent& e) override;
    bool onKeyDown(const KeyEvent& e) override;

private:
    std::vector<std::string> m_options;
    int m_selected = 0;
    std::function<void(int)> m_onChange;
    std::shared_ptr<MenuView> m_menu;
};

class Menu : public WidgetHandle<Menu, MenuButtonView> {
public:
    Menu(std::string title, std::vector<MenuItem> items)
        : WidgetHandle(std::make_shared<MenuButtonView>(std::move(title), std::move(items))) {}
    Menu& borderless(bool b = true) { return apply([&](MenuButtonView& n) { n.setBorderless(b); }); }
    Menu& icon(Icon i) { return apply([&](MenuButtonView& n) { n.setIcon(i); }); }
    void setItems(std::vector<MenuItem> items) { m_node->setItems(std::move(items)); }
};

class Picker : public WidgetHandle<Picker, PickerView> {
public:
    explicit Picker(std::vector<std::string> options, int selected = 0)
        : WidgetHandle(std::make_shared<PickerView>(std::move(options), selected)) {}
    Picker& onChange(std::function<void(int)> fn) { return apply([&](PickerView& n) { n.setOnChange(std::move(fn)); }); }
    Picker& selected(int index) { return apply([&](PickerView& n) { n.setSelected(index); }); }
    [[nodiscard]] int selectedIndex() const { return m_node->selected(); }
};

}  // namespace mayo
