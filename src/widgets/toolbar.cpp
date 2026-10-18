#include "mayonnaise/widgets/toolbar.hpp"

#include <algorithm>
#include <cmath>

#include "mayonnaise/app.hpp"
#include "mayonnaise/painter.hpp"
#include "mayonnaise/widgets/button.hpp"
#include "mayonnaise/widgets/segmented_control.hpp"
#include "mayonnaise/widgets/toggle.hpp"

namespace mayo {

ToolbarView::ToolbarView() : StackView(Axis::Horizontal) {
    setDragRegion(true);
    setSpacing(theme().metrics.spacingSmall + 4);
    setAlignment(Alignment::Leading);
}

void ToolbarView::setOverflowEnabled(bool enabled) {
    if (m_overflowEnabled == enabled) return;
    m_overflowEnabled = enabled;
    invalidateLayout();
}

bool ToolbarView::appendActions(const std::shared_ptr<Widget>& widget, std::vector<MenuItem>& items) const {
    if (widget->m_hidden) return true;
    const bool enabled = widget->isEnabled();
    const auto owner = weak_from_this();
    auto canInvoke = [owner](const Widget* source) {
        auto toolbar = owner.lock();
        if (!toolbar || !toolbar->isAncestorOf(source) || !source->isEnabled()) return false;
        for (auto* node = source; node != toolbar.get(); node = node->parent()) {
            if (node->m_hidden) return false;
        }
        return !toolbar->m_hidden;
    };
    if (auto button = std::dynamic_pointer_cast<ButtonView>(widget)) {
        std::string label = button->title().empty() ? button->tooltip() : button->title();
        if (label.empty()) return false;
        std::weak_ptr<ButtonView> weak = button;
        MenuItem item(label, [weak, canInvoke] {
            if (auto b = weak.lock(); b && canInvoke(b.get())) b->click();
        });
        item.icon = button->icon();
        item.enabled = enabled;
        item.destructive = button->style() == ButtonStyle::Destructive;
        items.push_back(std::move(item));
        return true;
    }
    if (auto menu = std::dynamic_pointer_cast<MenuButtonView>(widget)) {
        std::string label = menu->title().empty() ? menu->tooltip() : menu->title();
        if (label.empty()) return false;
        MenuItem item(label);
        item.icon = menu->icon();
        item.enabled = enabled;
        item.subitems = menu->items();
        // Keep the source control alive only through a weak reference. A command
        // removed or disabled while the menu is open must not be invoked.
        std::weak_ptr<Widget> weak = widget;
        auto guard = [&](auto&& self, std::vector<MenuItem>& entries) -> void {
            for (auto& entry : entries) {
                if (entry.action)
                    entry.action = [weak, canInvoke, action = entry.action] {
                        if (auto source = weak.lock(); source && canInvoke(source.get())) action();
                    };
                self(self, entry.subitems);
            }
        };
        guard(guard, item.subitems);
        items.push_back(std::move(item));
        return true;
    }
    if (auto control = std::dynamic_pointer_cast<SegmentedControlView>(widget)) {
        const auto& segments = control->segments();
        if (segments.empty() ||
            std::any_of(segments.begin(), segments.end(), [](const auto& s) { return s.title.empty(); }))
            return false;  // Icon-only segments need application-supplied names; don't invent commands.
        for (size_t i = 0; i < segments.size(); ++i) {
            std::weak_ptr<SegmentedControlView> weak = control;
            MenuItem item(segments[i].title, [weak, canInvoke, index = static_cast<int>(i)] {
                if (auto c = weak.lock(); c && canInvoke(c.get())) c->setSelected(index, true);
            });
            item.icon = segments[i].icon;
            item.checked = control->selected() == static_cast<int>(i);
            item.enabled = enabled;
            items.push_back(std::move(item));
        }
        return true;
    }
    if (auto picker = std::dynamic_pointer_cast<PickerView>(widget)) {
        for (size_t i = 0; i < picker->options().size(); ++i) {
            std::weak_ptr<PickerView> weak = picker;
            MenuItem item(picker->options()[i], [weak, canInvoke, index = static_cast<int>(i)] {
                if (auto c = weak.lock(); c && canInvoke(c.get())) c->setSelected(index, true);
            });
            item.checked = picker->selected() == static_cast<int>(i);
            item.enabled = enabled;
            items.push_back(std::move(item));
        }
        return !picker->options().empty();
    }
    if (auto toggle = std::dynamic_pointer_cast<BooleanControlView>(widget)) {
        const std::string label = toggle->label().empty() ? toggle->tooltip() : toggle->label();
        if (label.empty()) return false;
        std::weak_ptr<BooleanControlView> weak = toggle;
        MenuItem item(label, [weak, canInvoke] {
            if (auto c = weak.lock(); c && canInvoke(c.get())) c->onKeyDown({Key::Space});
        });
        item.checked = toggle->isOn();
        item.enabled = enabled;
        items.push_back(std::move(item));
        return true;
    }
    if (std::dynamic_pointer_cast<ToolbarGroupView>(widget)) {
        const size_t before = items.size();
        for (const auto& child : widget->children()) {
            if (!appendActions(child, items)) {
                items.resize(before);
                return false;
            }
        }
        return items.size() > before;
    }
    return false;  // Labels, search fields and custom controls remain in the layout.
}

std::vector<MenuItem> ToolbarView::overflowItems() const {
    std::vector<MenuItem> items;
    for (const auto& child : children()) {
        if (child == m_more || !child->m_layoutSuppressed || child->m_hidden) continue;
        std::vector<MenuItem> group;
        if (!appendActions(child, group) || group.empty()) continue;
        if (!items.empty()) items.push_back(MenuItem::separator());
        items.insert(items.end(), group.begin(), group.end());
    }
    return items;
}

void ToolbarView::updateOverflow(Size available) {
    // Restore only container-owned visibility. Application-hidden controls stay hidden.
    for (const auto& child : children()) child->m_layoutSuppressed = false;
    if (!m_more) {
        m_more = std::make_shared<ButtonView>(std::string{});
        m_more->setIcon(Icon::Ellipsis);
        m_more->setStyle(ButtonStyle::Glass);
        m_more->setTooltip("更多操作");
        const auto weak = weak_from_this();
        m_more->setOnClick([weak] {
            if (auto toolbar = std::dynamic_pointer_cast<ToolbarView>(weak.lock())) toolbar->showOverflow();
        });
        addChild(m_more);
    } else if (m_more->parent() != this) {
        addChild(m_more);
    } else if (children().back() != m_more) {
        removeChild(m_more.get());
        addChild(m_more);
    }
    m_more->m_layoutSuppressed = true;
    auto usedWidth = [&] {
        float width = 0;
        int count = 0;
        for (const auto& child : children()) {
            if (!child->isVisible()) continue;
            width += child->measure({kInfinity, available.height}).width;
            ++count;
        }
        return width + spacing() * static_cast<float>(std::max(0, count - 1));
    };
    float width = usedWidth();
    for (auto it = children().rbegin(); m_overflowEnabled && it != children().rend() && width > available.width; ++it) {
        const auto& child = *it;
        if (child == m_more || !child->isVisible()) continue;
        std::vector<MenuItem> actions;
        if (!appendActions(child, actions) || actions.empty()) continue;
        child->m_layoutSuppressed = true;
        m_more->m_layoutSuppressed = false;
        width = usedWidth();
    }
}

Size ToolbarView::measureContent(Size available) {
    updateOverflow(available);
    Size measured = StackView::measureContent(available);
    if (std::isfinite(available.width)) measured.width = std::min(measured.width, available.width);
    return measured;
}

void ToolbarView::arrangeContent(const Rect& content) {
    updateOverflow(content.size());
    // Resizing can hide the focused action or one of a group's descendants.
    if (auto* h = host(); h && h->focusedWidget()) {
        for (const auto& child : children()) {
            if (child->m_layoutSuppressed && child->isAncestorOf(h->focusedWidget())) {
                h->setFocus(m_more.get(), true);
                break;
            }
        }
        if (h->focusedWidget() == m_more.get() && !m_more->isVisible()) restoreFocus();
    }
    StackView::arrangeContent(content);
    if (m_more->isVisible() && m_more->frame().right() > content.right()) {
        Rect more = m_more->frame();
        more.width = std::min(more.width, content.width);
        more.x = content.right() - more.width;
        m_more->arrange(more);
    }
    if (m_menu && m_menu->isShown() && (!m_more->isVisible() || m_more->frame() != m_menuAnchor)) m_menu->dismiss();
}

void ToolbarView::draw(Painter& p) {
    Rect clip = frame();
    // Retain vertical space for glass shadows, but protect neighbouring window chrome.
    clip.y -= theme().metrics.spacing;
    clip.height += theme().metrics.spacing * 2;
    p.pushClip(clip);
    if (m_more && m_more->isVisible()) {
        Rect leading = clip;
        leading.width = std::max(0.0f, m_more->frame().x - clip.x - spacing());
        p.pushClip(leading);
        for (const auto& child : children())
            if (child != m_more) child->paint(p);
        p.popClip();
        m_more->paint(p);
    } else {
        paintChildren(p);
    }
    p.popClip();
}

void ToolbarView::showOverflow() {
    if (!host()) return;
    auto items = overflowItems();
    if (items.empty()) return;
    if (!m_menu) m_menu = std::make_shared<MenuView>(std::vector<MenuItem>{});
    m_menu->setItems(std::move(items));
    const auto weak = weak_from_this();
    m_menu->setOnDismiss([weak] {
        if (auto toolbar = std::dynamic_pointer_cast<ToolbarView>(weak.lock()); toolbar && toolbar->host()) {
            toolbar->restoreFocus();
        }
    });
    m_menu->showFrom(*m_more);
    m_menuAnchor = m_more->frame();
    m_menu->onKeyDown({Key::Down});
    host()->setFocus(m_menu.get(), true);
}

void ToolbarView::restoreFocus() {
    auto* h = host();
    if (!h) return;
    if (m_more && m_more->isVisible()) {
        h->setFocus(m_more.get(), true);
        return;
    }
    // Resizing may remove the overflow button while its menu still owns focus.
    auto first = [&](auto&& self, Widget* widget) -> Widget* {
        if (!widget->isVisible() || !widget->isEnabled()) return nullptr;
        if (widget->acceptsFocus()) return widget;
        for (const auto& child : widget->children())
            if (auto* found = self(self, child.get())) return found;
        return nullptr;
    };
    h->setFocus(first(first, this), true);
}

bool ToolbarView::onKeyDown(const KeyEvent& e) {
    if (e.primary() || e.alt() || e.shift()) return false;
    if (e.key != Key::Left && e.key != Key::Right && e.key != Key::Home && e.key != Key::End) return false;
    std::vector<Widget*> controls;
    auto collect = [&](auto&& self, Widget* widget) -> void {
        if (!widget->isVisible() || !widget->isEnabled()) return;
        if (widget->acceptsFocus()) controls.push_back(widget);
        for (const auto& child : widget->children()) self(self, child.get());
    };
    collect(collect, this);
    auto* h = host();
    if (!h || controls.empty()) return false;
    auto at = std::find(controls.begin(), controls.end(), h->focusedWidget());
    size_t index = at == controls.end() ? 0 : static_cast<size_t>(at - controls.begin());
    if (e.key == Key::Home)
        index = 0;
    else if (e.key == Key::End)
        index = controls.size() - 1;
    else if (e.key == Key::Right)
        index = (index + 1) % controls.size();
    else
        index = (index + controls.size() - 1) % controls.size();
    h->setFocus(controls[index], true);
    return true;
}

ToolbarGroupView::ToolbarGroupView() : StackView(Axis::Horizontal) {
    setSpacing(0);
    setAlignment(Alignment::Center);
    setPadding(Insets(3, 4));
}

void ToolbarGroupView::draw(Painter& p) {
    const Theme& t = theme();
    Rect r = frame();
    float radius = r.height * 0.5f;
    p.drawShadow(r, radius, t.colors.shadow.multipliedAlpha(t.isDark() ? 0.8f : 0.65f), 14, {0, 3});
    p.drawMaterial(r, radius, Material::Glass);
    paintChildren(p);
}

}  // namespace mayo
