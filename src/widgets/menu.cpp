#include "mayonnaise/ui_translation.hpp"
#include "mayonnaise/widgets/menu.hpp"

#include <cmath>

#include "../render/font.hpp"
#include "mayonnaise/app.hpp"
#include "mayonnaise/painter.hpp"
#include "mayonnaise/widgets/image.hpp"

namespace mayo {

namespace {

constexpr float kPanelPad = 5;
constexpr float kSeparatorH = 11;
constexpr float kTextPad = 10;
constexpr float kColumn = 18;
constexpr float kDetailGap = 8;  // space between a title and its detail ("推荐")
constexpr float kSubmenuWidth = 13;  // room for the submenu chevron

/// Shared bezel for pop-up / pull-down buttons (matches ButtonStyle::Default).
void drawBezel(Painter& p, const Rect& r, float hover, float press) {
    const Theme& t = theme();
    const auto& c = t.colors;
    const float radius = r.height * 0.5f;
    Color fill = t.isDark() ? c.control.mix(c.controlHover, hover) : c.control;
    fill = fill.mix(c.controlPressed, press);
    if (!t.isDark()) p.drawShadow(r, radius, c.controlShadow, 2.0f, {0, 0.5f});
    p.fillRoundedRect(r, radius, fill, c.controlBorder, 0.5f);
}
}  // namespace

// --- MenuView --------------------------------------------------------------------------------

MenuView::MenuView(std::vector<MenuItem> items) : PopupView(Material::Menu), m_items(std::move(items)) {
    setGap(4);
}

void MenuView::setItems(std::vector<MenuItem> items) {
    m_items = std::move(items);
    m_highlighted = -1;
    invalidateLayout();
}

void MenuView::setHighlighted(int index) {
    if (index == m_highlighted) return;
    m_highlighted = index;
    // Hovering a row with a submenu opens it beside the row; anything else closes the open one.
    if (index >= 0 && index < static_cast<int>(m_items.size()) && m_items[static_cast<size_t>(index)].hasSubmenu()) {
        showSubmenu(index);
    } else {
        closeSubmenu();
    }
    invalidate();
}

void MenuView::closeSubmenu() {
    if (m_submenu) m_submenu->dismiss();
}

void MenuView::showSubmenu(int index) {
    if (!host()) return;
    if (m_submenu && m_submenuIndex == index && m_submenu->isShown()) return;
    if (m_submenu) m_submenu->dismiss();
    m_submenuIndex = index;
    m_submenu = std::make_shared<MenuView>(m_items[static_cast<size_t>(index)].subitems);
    m_submenu->setGap(0);
    m_submenu->setOnDismiss([this] { invalidate(); });
    // Hung from the row itself (not the whole parent menu) and overlapping it by the panel padding, as
    // macOS's submenus do.
    m_submenu->setAnchorRect(rowRect(index));
    m_submenu->showFrom(*this);
    // Keyboard focus stays with the parent menu so Up/Down keep moving there; Escape closes both.
    host()->setFocus(this, false);
}

Size MenuView::measureContent(Size /*available*/) {
    const Theme& t = theme();
    const Font f = t.font(TextStyle::Body);
    const Font sf = t.font(TextStyle::Body);
    float textW = 0;
    float shortcutW = 0;
    float detailW = 0;
    float h = kPanelPad * 2;
    for (const auto& item : m_items) {
        if (item.isSeparator) {
            h += kSeparatorH;
            continue;
        }
        h += t.metrics.menuItemHeight;
        float titleW = render::measureText(item.title, f);
        if (item.hasSubmenu()) titleW += kSubmenuWidth;
        textW = std::max(textW, titleW);
        if (!item.detail.empty()) detailW = std::max(detailW, titleW + kDetailGap + render::measureText(item.detail, sf));
        if (!item.shortcut.empty()) shortcutW = std::max(shortcutW, render::measureText(item.shortcut, sf));
    }
    float w = kPanelPad * 2 + kTextPad * 2 + std::max(textW, detailW) + kColumn +
              (kColumn + 4) + (shortcutW > 0 ? shortcutW + 28 : 0.0f);
    return {std::max(std::ceil(w), 140.0f), h};
}

Rect MenuView::rowRect(int index) const {
    const Theme& t = theme();
    Rect r = frame();
    float y = r.y + kPanelPad;
    for (int i = 0; i < static_cast<int>(m_items.size()); ++i) {
        float h = m_items[static_cast<size_t>(i)].isSeparator ? kSeparatorH : t.metrics.menuItemHeight;
        if (i == index) return {r.x + kPanelPad, y, r.width - kPanelPad * 2, h};
        y += h;
    }
    return {};
}

int MenuView::itemAt(Point p) const {
    for (int i = 0; i < static_cast<int>(m_items.size()); ++i) {
        const auto& item = m_items[static_cast<size_t>(i)];
        if (item.isSeparator || !item.enabled) continue;
        if (rowRect(i).contains(p)) return i;
    }
    return -1;
}

bool MenuView::onPointerDown(const PointerEvent& e) {
    m_pressed = itemAt(e.position);
    return true;
}

void MenuView::onPointerMove(const PointerEvent& e) { setHighlighted(itemAt(e.position)); }

void MenuView::onPointerUp(const PointerEvent& e) {
    int i = itemAt(e.position);
    if (i >= 0 && i == m_pressed) activate(i);
    m_pressed = -1;
}

void MenuView::onPointerLeave() { setHighlighted(-1); }

void MenuView::activate(int index) {
    if (index < 0 || index >= static_cast<int>(m_items.size())) return;
    if (!m_items[static_cast<size_t>(index)].enabled || m_items[static_cast<size_t>(index)].isSeparator) return;
    if (m_items[static_cast<size_t>(index)].hasSubmenu()) {
        showSubmenu(index);
        return;
    }
    // A submenu choice closes this menu too.
    if (m_submenu) m_submenu->dismiss();
    auto action = m_items[static_cast<size_t>(index)].action;
    auto keep = shared_from_this();  // the action may drop the last outside reference
    // Brief flash like macOS, then close and run the action.
    dismiss();
    if (action) action();
}

void MenuView::moveHighlight(int delta) {
    const int n = static_cast<int>(m_items.size());
    if (n == 0) return;
    int i = m_highlighted;
    for (int step = 0; step < n; ++step) {
        i = (i + delta + n) % n;
        if (i < 0) i = delta > 0 ? 0 : n - 1;
        const auto& item = m_items[static_cast<size_t>(i)];
        if (!item.isSeparator && item.enabled) {
            setHighlighted(i);
            return;
        }
    }
}

bool MenuView::onKeyDown(const KeyEvent& e) {
    switch (e.key) {
        case Key::Down: moveHighlight(1); return true;
        case Key::Up: moveHighlight(m_highlighted < 0 ? 0 : -1); return true;
        case Key::Enter:
        case Key::Space:
            if (m_highlighted >= 0) activate(m_highlighted);
            return true;
        case Key::Right:
            if (m_highlighted >= 0) showSubmenu(m_highlighted);
            return true;
        case Key::Escape:
            if (m_submenu) m_submenu->dismiss();
            dismiss();
            return true;
        default: return false;
    }
}

void MenuView::drawPanel(Painter& p) {
    const Theme& t = theme();
    const auto& c = t.colors;
    const Font f = t.font(TextStyle::Body);
    const Font sf = t.font(TextStyle::Subheadline);
    for (int i = 0; i < static_cast<int>(m_items.size()); ++i) {
        const auto& item = m_items[static_cast<size_t>(i)];
        Rect row = rowRect(i);
        if (item.isSeparator) {
            float px = 1.0f / p.scale();
            p.fillRect({row.x + kTextPad, p.snap(row.center().y), row.width - kTextPad * 2, px}, c.separator);
            continue;
        }
        const bool hi = i == m_highlighted && item.enabled;
        if (hi) p.fillRoundedRect(row, t.metrics.smallRadius + 1, item.destructive ? c.destructive : c.accent);
        Color text = !item.enabled ? c.tertiaryLabel : hi ? c.onAccent : item.destructive ? c.destructive : c.label;
        float x = row.x + kTextPad;
        {  // Stable check column, even when this menu has no checked rows.
            if (item.checked) p.drawIcon(Icon::Checkmark, {x - 2, row.center().y - 6, 12, 12}, text, 1.1f);
            x += kColumn;
        }
        {  // Stable icon column for item, background and overflow menus.
            if (item.image && item.image->width() > 0) {
                p.drawImage({x, row.center().y - 8, 16, 16}, *item.image);
            } else if (item.icon != Icon::None) {
                p.drawIcon(item.icon, {x, row.center().y - 7, 14, 14}, text);
            }
            x += kColumn + 4;
        }
        if (item.hasSubmenu()) {
            p.drawText(item.title, {x, row.y, row.right() - x - kSubmenuWidth, row.height}, f, text);
            p.drawIcon(Icon::ChevronRight, {row.right() - kTextPad - 4, row.center().y - 4.5f, 9, 9},
                       item.enabled ? (hi ? c.onAccent : c.secondaryLabel) : c.tertiaryLabel, 1.3f);
        } else {
            p.drawText(item.title, {x, row.y, row.right() - x, row.height}, f, text);
        }
        if (!item.detail.empty()) {
            // The detail follows the title on its baseline instead of going to the far right.
            const float titleW = render::measureText(item.title, f);
            Color dc = hi ? c.onAccent.withAlpha(0.75f) : c.tertiaryLabel;
            p.drawText(item.detail, {x + titleW + kDetailGap, row.y, row.right() - x, row.height}, sf, dc);
        }
        if (!item.shortcut.empty()) {
            Color sc = hi ? c.onAccent.withAlpha(0.8f) : c.tertiaryLabel;
            p.drawText(item.shortcut, {row.x, row.y, row.width - kTextPad, row.height}, f, sc, TextAlign::Trailing);
        }
    }
}

// --- MenuButtonView ----------------------------------------------------------------------------

MenuButtonView::MenuButtonView(std::string title, std::vector<MenuItem> items)
    : m_title(uiText(title)), m_items(std::move(items)) {}

void MenuButtonView::setBorderless(bool b) {
    m_borderless = b;
    invalidate();
}

void MenuButtonView::setIcon(Icon icon) {
    m_icon = icon;
    invalidateLayout();
}

void MenuButtonView::open() {
    if (!host()) return;
    if (!m_menu) m_menu = std::make_shared<MenuView>(m_items);
    m_menu->setItems(m_items);
    m_menu->setMinimumWidth(frame().width);
    m_menu->setOnDismiss([this] { invalidate(); });
    m_menu->showFrom(*this);
    host()->setFocus(m_menu.get(), false);
}

Size MenuButtonView::measureContent(Size /*available*/) {
    const Font f = theme().font(TextStyle::Body);
    float w = std::ceil(render::measureText(m_title, f)) + theme().metrics.controlPaddingH * 2 + 14;
    if (m_icon != Icon::None) w += 19;
    return {w, controlHeight()};
}

void MenuButtonView::draw(Painter& p) {
    const Theme& t = theme();
    const auto& c = t.colors;
    const Rect r = frame();
    const bool open = m_menu && m_menu->isShown();
    const float hover = hoverAmount();
    const float press = std::max(pressAmount(), open ? 1.0f : 0.0f);
    if (m_borderless) {
        Color fill = c.hover.multipliedAlpha(hover).mix(c.fill, press);
        if (fill.a > 0.001f) p.fillRoundedRect(r, r.height * 0.5f, fill);
    } else {
        drawBezel(p, r, hover, press);
    }
    const Font f = t.font(TextStyle::Body);
    float x = r.x + t.metrics.controlPaddingH;
    if (m_icon != Icon::None) {
        p.drawIcon(m_icon, {x, r.center().y - 7, 14, 14}, c.label);
        x += 19;
    }
    p.drawText(m_title, {x, r.y, r.width, r.height}, f, foreground().value_or(c.label));
    p.drawIcon(Icon::ChevronDown, {r.right() - t.metrics.controlPaddingH - 8, r.center().y - 4.5f, 9, 9},
               c.secondaryLabel, 1.3f);
}

bool MenuButtonView::onPointerDown(const PointerEvent& e) {
    if (e.button != MouseButton::Left) return false;
    open();
    return true;  // capture so the press doesn't bubble; the menu handles the rest
}

bool MenuButtonView::onKeyDown(const KeyEvent& e) {
    if (e.key == Key::Space || e.key == Key::Enter || e.key == Key::Down) {
        open();
        if (m_menu) m_menu->setHighlighted(0);
        return true;
    }
    return false;
}

// --- PickerView ------------------------------------------------------------------------------

PickerView::PickerView(std::vector<std::string> options, int selected)
    : m_options(std::move(options)), m_selected(selected) {
    for (auto& option : m_options) option = uiText(option);
}

void PickerView::setSelected(int index, bool notify) {
    if (m_options.empty()) return;
    index = std::clamp(index, 0, static_cast<int>(m_options.size()) - 1);
    if (index == m_selected) return;
    m_selected = index;
    invalidate();
    if (notify && m_onChange) m_onChange(m_selected);
}

void PickerView::setOptions(std::vector<std::string> options) {
    m_options = std::move(options);
    for (auto& option : m_options) option = uiText(option);
    m_selected = std::clamp(m_selected, 0, std::max(0, static_cast<int>(m_options.size()) - 1));
    invalidateLayout();
}

void PickerView::open() {
    if (!host()) return;
    std::vector<MenuItem> items;
    for (int i = 0; i < static_cast<int>(m_options.size()); ++i) {
        MenuItem item(m_options[static_cast<size_t>(i)], [this, i] { setSelected(i, true); });
        item.checked = i == m_selected;
        items.push_back(std::move(item));
    }
    if (!m_menu) m_menu = std::make_shared<MenuView>(std::vector<MenuItem>{});
    m_menu->setItems(std::move(items));
    m_menu->setMinimumWidth(frame().width);
    m_menu->setOnDismiss([this] { invalidate(); });
    m_menu->showFrom(*this);
    m_menu->setHighlighted(m_selected);
    host()->setFocus(m_menu.get(), false);
}

Size PickerView::measureContent(Size /*available*/) {
    const Font f = theme().font(TextStyle::Body);
    float w = 0;
    for (const auto& o : m_options) w = std::max(w, render::measureText(o, f));
    return {std::ceil(w) + theme().metrics.controlPaddingH * 2 + 18, controlHeight()};
}

void PickerView::draw(Painter& p) {
    const Theme& t = theme();
    const auto& c = t.colors;
    const Rect r = frame();
    const bool open = m_menu && m_menu->isShown();
    drawBezel(p, r, hoverAmount(), std::max(pressAmount(), open ? 1.0f : 0.0f));
    if (!m_options.empty()) {
        p.drawText(m_options[static_cast<size_t>(m_selected)],
                   {r.x + t.metrics.controlPaddingH, r.y, r.width, r.height}, t.font(TextStyle::Body), c.label);
    }
    p.drawIcon(Icon::ChevronUpDown, {r.right() - t.metrics.controlPaddingH - 9, r.center().y - 6, 11, 12},
               c.secondaryLabel, 1.2f);
}

bool PickerView::onPointerDown(const PointerEvent& e) {
    if (e.button != MouseButton::Left) return false;
    open();
    return true;
}

bool PickerView::onKeyDown(const KeyEvent& e) {
    if (e.key == Key::Space || e.key == Key::Enter) {
        open();
        return true;
    }
    if (e.key == Key::Up) {
        setSelected(m_selected - 1, true);
        return true;
    }
    if (e.key == Key::Down) {
        setSelected(m_selected + 1, true);
        return true;
    }
    return false;
}

}  // namespace mayo
