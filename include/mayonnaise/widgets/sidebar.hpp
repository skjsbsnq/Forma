#include "mayonnaise/ui_translation.hpp"
#pragma once

#include <functional>
#include <optional>
#include <string>
#include <vector>

#include "../icon.hpp"
#include "../scroll_physics.hpp"
#include "../widget.hpp"

namespace mayo {

struct SidebarItem {
    std::string id;
    std::string title;
    Icon icon = Icon::None;
    /// When set, the icon is drawn white on a coloured rounded tile (System Settings style).
    std::optional<Color> tile;
    std::string badge;
    std::string subtitle;
    Icon actionIcon = Icon::None;
    bool busy = false;
    bool enabled = true;
    bool isHeader = false;

    SidebarItem(std::string id_, Icon icon_, std::string title_)
        : id(std::move(id_)), title(uiText(title_)), icon(icon_) {}
    SidebarItem(std::string id_, std::string title_) : id(std::move(id_)), title(uiText(title_)) {}
    static SidebarItem header(std::string title) {
        SidebarItem s({}, std::move(title));
        s.isHeader = true;
        return s;
    }
    SidebarItem& withTile(Color c) {
        tile = c;
        return *this;
    }
    SidebarItem& withBadge(std::string b) {
        badge = std::move(b);
        return *this;
    }
};

enum class SidebarSelectionStyle : std::uint8_t { Neutral, Accent };

class SidebarView : public Widget {
public:
    explicit SidebarView(std::vector<SidebarItem> items);

    void setItems(std::vector<SidebarItem> items);
    void setSelection(const std::string& id, bool notify = false);
    [[nodiscard]] const std::string& selection() const { return m_selection; }
    void setOnSelect(std::function<void(const std::string&)> fn) { m_onSelect = std::move(fn); }
    void setOnAction(std::function<void(const std::string&)> fn) { m_onAction = std::move(fn); }
    void setSelectionStyle(SidebarSelectionStyle s);
    /// Optional widget shown above the list (search field, account card).
    void setHeader(std::shared_ptr<Widget> header);

    Size measureContent(Size available) override;
    void arrangeContent(const Rect& content) override;
    [[nodiscard]] bool expandsContent(Axis axis) const override { return axis == Axis::Vertical; }
    void draw(Painter& painter) override;
    [[nodiscard]] bool acceptsFocus() const override { return true; }
    [[nodiscard]] bool isInteractive() const override { return true; }
    bool onPointerDown(const PointerEvent& e) override;
    void onPointerMove(const PointerEvent& e) override;
    void onPointerUp(const PointerEvent& e) override;
    void onPointerLeave() override;
    bool onScroll(const ScrollEvent& e) override;
    bool onKeyDown(const KeyEvent& e) override;

private:
    [[nodiscard]] float rowHeight(const SidebarItem& item) const;
    [[nodiscard]] float listTop() const;
    [[nodiscard]] float contentHeight() const;
    [[nodiscard]] int rowAt(Point p) const;
    [[nodiscard]] Rect rowRect(int index) const;
    [[nodiscard]] Rect actionRect(int index) const;
    void moveSelection(int delta);
    void clampScroll();  // updates the scroll range; m_scroll follows m_physics
    void stepScroll();

    std::vector<SidebarItem> m_items;
    std::string m_selection;
    std::function<void(const std::string&)> m_onSelect;
    std::function<void(const std::string&)> m_onAction;
    SidebarSelectionStyle m_style = SidebarSelectionStyle::Neutral;
    std::shared_ptr<Widget> m_header;
    int m_hovered = -1;
    int m_pressed = -1;
    bool m_actionHovered = false;
    std::string m_pressedAction;
    float m_scroll = 0;
    ScrollPhysics m_physics;
    Animated<Rect> m_highlight;  // selection pill glides between rows
};

/// Source list for the leading column of a SplitView.
class Sidebar : public WidgetHandle<Sidebar, SidebarView> {
public:
    Sidebar(std::initializer_list<SidebarItem> items)  // NOLINT(google-explicit-constructor)
        : WidgetHandle(std::make_shared<SidebarView>(std::vector<SidebarItem>(items))) {}
    explicit Sidebar(std::vector<SidebarItem> items) : WidgetHandle(std::make_shared<SidebarView>(std::move(items))) {}

    Sidebar& selection(const std::string& id) { return apply([&](SidebarView& n) { n.setSelection(id); }); }
    Sidebar& onSelect(std::function<void(const std::string&)> fn) {
        return apply([&](SidebarView& n) { n.setOnSelect(std::move(fn)); });
    }
    Sidebar& selectionStyle(SidebarSelectionStyle s) { return apply([&](SidebarView& n) { n.setSelectionStyle(s); }); }
    Sidebar& header(const Element& e) { return apply([&](SidebarView& n) { n.setHeader(e.widget()); }); }
    void select(const std::string& id) { m_node->setSelection(id, true); }
    void setItems(std::vector<SidebarItem> items) { m_node->setItems(std::move(items)); }
    [[nodiscard]] const std::string& selected() const { return m_node->selection(); }
};

}  // namespace mayo
