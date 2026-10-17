// Building blocks shared by the panels: rows bound to GSettings keys, cards whose rows are rebuilt from
// live data, clickable rows, icon tiles and a searchable chooser popover.
#pragma once

#include <mayonnaise/mayonnaise.hpp>
#include <mayonnaise/ui_translation.hpp>

#include <functional>
#include <string>
#include <vector>

#include "glib_support.hpp"

namespace ui {

struct Option {
    std::string label;
    std::string value;  ///< in Key::getText() form
};

// Rows bound to a key: they write on change and follow changes made elsewhere. A key the system doesn't
// have gives a disabled row.
mayo::FormRow toggleRow(const std::string& title, const gx::Key& key, const std::string& subtitle = {},
                        bool invert = false);
mayo::FormRow pickerRow(const std::string& title, const gx::Key& key, std::vector<Option> options,
                        const std::string& subtitle = {});
/// `scale` maps the key's value to the slider (e.g. 1/60 for seconds shown as minutes).
mayo::FormRow sliderRow(const std::string& title, const gx::Key& key, float min, float max, float step = 0,
                        const std::string& subtitle = {}, float scale = 1);
/// Slider with a value label (e.g. "1.25×", "500 ms") formatted by `format`.
mayo::FormRow sliderValueRow(const std::string& title, const gx::Key& key, float min, float max, float step,
                             std::function<std::string(float)> format, const std::string& subtitle = {});

/// Text field bound to a string (or numeric) key; writes as you type.
mayo::FormRow textRow(const std::string& title, const gx::Key& key, const std::string& placeholder = {},
                      float width = 220);

/// Title on the left, secondary text on the right.
mayo::FormRow infoRow(const std::string& title, const std::string& value);
mayo::FormRow infoRow(const std::string& title, const mayo::Label& value);

/// A titled card of rows (any elements), like mayo::FormSection but built from a vector.
mayo::Element section(const std::string& title, const std::vector<mayo::Element>& rows,
                      const std::string& footer = {});

/// A card whose rows are replaced as data changes (networks, devices). Shows `emptyText` when empty.
class DynamicSection {
public:
    DynamicSection(const std::string& title, std::string emptyText, const std::string& footer = {});
    void setRows(const std::vector<mayo::Element>& rows);
    void setTitle(const std::string& title);
    /// Optional control drawn on the title line (a spinner, a "Scan" button).
    void setAccessory(const mayo::Element& accessory);
    void setHidden(bool hidden) { m_root.setHidden(hidden); }
    [[nodiscard]] const mayo::VStack& root() const { return m_root; }
    [[nodiscard]] mayo::Element element() const { return m_root; }

private:
    mayo::VStack m_root;
    mayo::Label m_title;
    mayo::HStack m_titleLine;
    std::shared_ptr<mayo::FormCardView> m_card;
    std::string m_emptyText;
};

/// Clickable row with hover / press feedback. Controls inside it keep their own clicks.
class ClickRowView : public mayo::StackView {
public:
    ClickRowView();
    void setOnClick(std::function<void()> fn) { m_onClick = std::move(fn); }
    void setChevron(bool chevron);
    void setSelected(bool selected);

    void draw(mayo::Painter& p) override;
    void drawOverlay(mayo::Painter& p) override;
    [[nodiscard]] bool isInteractive() const override { return true; }
    [[nodiscard]] bool acceptsFocus() const override { return true; }
    bool onPointerDown(const mayo::PointerEvent& e) override;
    void onPointerUp(const mayo::PointerEvent& e) override;
    bool onKeyDown(const mayo::KeyEvent& e) override;
    void onHoverChanged(bool /*hovered*/) override { invalidate(); }
    void onPressedChanged(bool /*pressed*/) override { invalidate(); }
    void onFocusChanged(bool /*focused*/) override { invalidate(); }

private:
    std::function<void()> m_onClick;
    bool m_chevron = false;
    bool m_selected = false;
};

class ClickRow : public mayo::detail::ContainerHandle<ClickRow, ClickRowView> {
public:
    ClickRow(std::initializer_list<mayo::Element> children = {});  // NOLINT(google-explicit-constructor)
    explicit ClickRow(const std::vector<mayo::Element>& children);
    ClickRow& onClick(std::function<void()> fn) {
        return apply([&](ClickRowView& n) { n.setOnClick(std::move(fn)); });
    }
    ClickRow& chevron(bool c = true) { return apply([&](ClickRowView& n) { n.setChevron(c); }); }
    ClickRow& selected(bool s = true) { return apply([&](ClickRowView& n) { n.setSelected(s); }); }
};

/// Grid of custom-drawn choices (style previews, colour dots, wallpapers) that wraps to the width.
class ChoiceGridView : public mayo::ControlView {
public:
    using DrawFn = std::function<void(mayo::Painter&, const mayo::Rect& cell, int index, bool selected, bool hovered)>;
    ChoiceGridView(int count, mayo::Size cell, float gap, DrawFn draw);
    void setCount(int count);
    void setSelected(int index);
    [[nodiscard]] int selected() const { return m_selected; }
    [[nodiscard]] int hovered() const { return m_hover; }
    void setOnChoose(std::function<void(int)> fn) { m_onChoose = std::move(fn); }
    /// Text under the grid naming the hovered (or selected) item, like the accent colour picker.
    void setCaption(std::function<std::string(int)> fn) { m_caption = std::move(fn); }
    /// Wrapping grids fill the available width; otherwise all items stay on one row.
    void setWraps(bool wraps);

    mayo::Size measureContent(mayo::Size available) override;
    [[nodiscard]] bool expandsContent(mayo::Axis axis) const override {
        return m_wraps && axis == mayo::Axis::Horizontal;
    }
    void draw(mayo::Painter& p) override;
    void drawOverlay(mayo::Painter& p) override;
    bool onPointerDown(const mayo::PointerEvent& e) override;
    void onPointerUp(const mayo::PointerEvent& e) override;
    void onPointerMove(const mayo::PointerEvent& e) override;
    void onPointerLeave() override;
    bool onKeyDown(const mayo::KeyEvent& e) override;

private:
    [[nodiscard]] int columns(float width) const;
    [[nodiscard]] mayo::Rect cellRect(int index) const;
    [[nodiscard]] int indexAt(mayo::Point p) const;
    void choose(int index);

    int m_count;
    mayo::Size m_cell;
    float m_gap;
    DrawFn m_draw;
    int m_selected = -1;
    int m_hover = -1;
    int m_pressed = -1;
    bool m_wraps = false;
    std::function<void(int)> m_onChoose;
    std::function<std::string(int)> m_caption;
};

class ChoiceGrid : public mayo::WidgetHandle<ChoiceGrid, ChoiceGridView> {
public:
    ChoiceGrid(int count, mayo::Size cell, float gap, ChoiceGridView::DrawFn draw)
        : WidgetHandle(std::make_shared<ChoiceGridView>(count, cell, gap, std::move(draw))) {}
    ChoiceGrid& onChoose(std::function<void(int)> fn) {
        return apply([&](ChoiceGridView& n) { n.setOnChoose(std::move(fn)); });
    }
    ChoiceGrid& wraps(bool w = true) { return apply([&](ChoiceGridView& n) { n.setWraps(w); }); }
    ChoiceGrid& caption(std::function<std::string(int)> fn) {
        return apply([&](ChoiceGridView& n) { n.setCaption(std::move(fn)); });
    }
};

/// Navigation-style row: icon tile, title, optional value, chevron.
ClickRow linkRow(mayo::Icon icon, mayo::Color tile, const std::string& title, const std::string& value,
                 std::function<void()> onClick);

/// White glyph on a coloured rounded square (sidebar / header style).
mayo::Canvas tile(mayo::Icon icon, mayo::Color color, float size = 22);
/// First letter of `name` on a tile coloured by a hash of the name (apps without a drawable icon).
mayo::Canvas letterTile(const std::string& name, float size = 22);
/// Plain glyph in a label colour.
mayo::Canvas glyph(mayo::Icon icon, float size = 16, bool secondary = true);
/// Large header card: tile, title and a description (System Settings pane header).
mayo::Element hero(mayo::Icon icon, mayo::Color color, const std::string& title, const std::string& description);
/// Scrolling, width-limited column of sections: the body of every panel. `model` (the panel's state object)
/// is owned by the page, so callbacks inside it can capture a plain pointer to the model without creating
/// reference cycles.
mayo::Element page(const std::vector<mayo::Element>& sections, std::shared_ptr<void> model = {});
/// Centred message for a panel whose service is missing.
mayo::Element unavailable(mayo::Icon icon, const std::string& title, const std::string& message);

/// Wi-Fi strength (0..100) as four bars of arcs.
mayo::Canvas signalIcon(int strength, bool secure);
/// Battery outline filled to `fraction`.
mayo::Canvas batteryIcon(double fraction, bool charging, float width = 28);

/// Weak reference to a widget, for anchoring a popover from inside the widget's own callback (capturing the
/// handle itself would make a reference cycle).
class Anchor {
public:
    template <class H>
    explicit Anchor(const H& handle) : m_widget(handle.node()) {}
    operator mayo::Element() const { return mayo::Element(m_widget.lock()); }  // NOLINT(google-explicit-constructor)

private:
    std::weak_ptr<mayo::Widget> m_widget;
};

/// Searchable list in a popover; `onPick` gets the chosen value.
void chooseFromList(const mayo::Element& anchor, const std::vector<Option>& items,
                    std::function<void(const std::string&)> onPick, const std::string& placeholder = "Search");

/// Small popover with a message and an OK button (errors from services).
void message(const mayo::Element& anchor, const std::string& title, const std::string& text);
/// Asks for confirmation; `onConfirm` runs if the destructive/primary button is pressed.
void confirm(const mayo::Element& anchor, const std::string& title, const std::string& text,
             const std::string& action, std::function<void()> onConfirm, bool destructive = true);

/// "5 minutes", "1 hour", "Never" (0).
std::string formatDuration(std::int64_t seconds);
/// "1.5 GB" with binary units.
std::string formatBytes(double bytes);
/// Lower-cased ASCII copy, for search.
std::string lower(std::string s);
std::string trim(const std::string& s);
std::vector<std::string> split(const std::string& s, char sep);

}  // namespace ui
