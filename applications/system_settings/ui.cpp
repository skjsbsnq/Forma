#include "ui.hpp"
#include "mayonnaise/ui_translation.hpp"

#include <algorithm>
#include <cctype>
#include <cmath>
#include <cstdio>

using namespace mayo;

namespace ui {

namespace {

constexpr float kPageWidth = 640;

std::string subtitleFor(const gx::Key& key, const std::string& subtitle) {
    std::string state;
    if (!key.available())
        state = "Not available on this system";
    else if (!key.writable())
        state = gx::readOnly() ? "Read-only mode" : "Locked or not writable";
    else if (key.pending())
        state = "Applying…";
    return state.empty() ? subtitle : subtitle.empty() ? state : subtitle + " · " + state;
}

}  // namespace

// Observers keep weak widget references: key caches outlive pages, and a control's handler owns its Key.
FormRow boundRow(const std::string& title, const gx::Key& key, const Element& control, const std::string& subtitle,
                 std::function<void(const gx::Key&, bool)> sync) {
    auto row = FormRow(title, control, " ");
    auto detail = std::dynamic_pointer_cast<LabelView>(row->children().front()->children().back());
    std::weak_ptr<LabelView> weakDetail = detail;
    std::weak_ptr<Widget> weakControl = control.widget();
    auto status = [weakControl, weakDetail, subtitle](const gx::Key& k, const gx::WriteResult& result) {
        if (auto c = weakControl.lock()) c->setDisabled(!k.writable() || k.pending());
        if (auto label = weakDetail.lock()) {
            const std::string text = result.error.empty() ? subtitleFor(k, subtitle) : result.error;
            label->setText(text);
            label->setForeground(result.error.empty() ? std::optional<Color>{} : theme().colors.red);
            label->setHidden(text.empty());
        }
    };
    sync(key, false);
    status(key, {});
    key.watch([key, sync, status] {
        sync(key, false);
        status(key, {});
    });
    key.watchWrite([sync, status](const gx::Key& k, const gx::WriteResult& result) {
        if (!result.pending()) sync(k, true);  // failure must undo optimistic UI even in a focused text field
        status(k, result);
    });
    return row;
}

void bindControl(const gx::Key& key, const Element& control, std::function<void(const gx::Key&, bool)> sync) {
    std::weak_ptr<Widget> weakControl = control.widget();
    auto status = [weakControl](const gx::Key& k) {
        if (auto c = weakControl.lock()) c->setDisabled(!k.writable() || k.pending());
    };
    sync(key, false);
    status(key);
    key.watch([key, sync, status] {
        sync(key, false);
        status(key);
    });
    key.watchWrite([weakControl, sync, status](const gx::Key& k, const gx::WriteResult& result) {
        if (!result.pending()) sync(k, true);
        status(k);
        if (!result.error.empty()) {
            if (auto c = weakControl.lock(); c && c->host())
                message(Element(c), "Unable to change setting", result.error);
        }
    });
}

// Bound rows ---------------------------------------------------------------------------------------------

FormRow toggleRow(const std::string& title, const gx::Key& key, const std::string& subtitle, bool invert) {
    auto toggle = Toggle("", key.getBool() != invert).onChange([key, invert](bool on) { key.set(on != invert); });
    return boundRow(title, key, toggle, subtitle,
                    [weak = std::weak_ptr<BooleanControlView>(toggle.node()), invert](const gx::Key& k, bool) {
                        if (auto t = weak.lock()) t->setOn(k.getBool() != invert);
                    });
}

FormRow pickerRow(const std::string& title, const gx::Key& key, std::vector<Option> options,
                  const std::string& subtitle) {
    std::vector<std::string> labels;
    for (const auto& o : options) labels.push_back(o.label);
    auto picker = Picker(labels, 0);
    picker.onChange([key, options](int i) {
        if (i >= 0 && i < static_cast<int>(options.size())) key.setText(options[static_cast<size_t>(i)].value);
    });
    return boundRow(title, key, picker, subtitle,
                    [weak = std::weak_ptr<PickerView>(picker.node()), labels, options](const gx::Key& k, bool) {
                        if (auto p = weak.lock()) {
                            int index = -1;
                            const std::string text = k.getText();
                            for (size_t i = 0; i < options.size(); ++i) {
                                if (options[i].value == text) index = static_cast<int>(i);
                            }
                            auto visible = labels;
                            if (index < 0) {
                                visible.push_back("Custom (" + text + ")");
                                index = static_cast<int>(labels.size());
                            }
                            p->setOptions(visible);
                            p->setSelected(index);
                        }
                    });
}

FormRow sliderRow(const std::string& title, const gx::Key& key, float min, float max, float step,
                  const std::string& subtitle, float scale) {
    auto slider = Slider(static_cast<float>(key.getDouble() * scale), min, max).width(220);
    if (step > 0) slider.step(step);
    slider.onChange([key, scale](float v) { key.setDouble(static_cast<double>(v) / scale); });
    return boundRow(title, key, slider, subtitle,
                    [weak = std::weak_ptr<SliderView>(slider.node()), scale](const gx::Key& k, bool) {
                        if (auto s = weak.lock()) s->setValue(static_cast<float>(k.getDouble() * scale));
                    });
}

FormRow sliderValueRow(const std::string& title, const gx::Key& key, float min, float max, float step,
                       std::function<std::string(float)> format, const std::string& subtitle) {
    const auto v0 = static_cast<float>(key.getDouble());
    auto value = Label(format(v0)).secondary().font(TextStyle::Callout).width(64).textAlign(TextAlign::Trailing);
    auto slider = Slider(v0, min, max).width(180);
    if (step > 0) slider.step(step);
    slider.onChange([key](float v) { key.setDouble(v); });
    auto row = boundRow(title, key, slider, subtitle,
                        [weak = std::weak_ptr<SliderView>(slider.node()),
                         label = std::weak_ptr<LabelView>(value.node()), format](const gx::Key& k, bool) {
                            const auto v = static_cast<float>(k.getDouble());
                            if (auto s = weak.lock()) s->setValue(v);
                            if (auto l = label.lock()) l->setText(format(v));
                        });
    row->addChild(value.node());
    return row;
}

FormRow textRow(const std::string& title, const gx::Key& key, const std::string& placeholder, float width) {
    auto field = TextField(placeholder).text(key.getText()).width(width);
    field.onChange([key](const std::string& t) {
        if (t != key.getText()) key.setText(t);
    });
    return boundRow(title, key, field, {},
                    [weak = std::weak_ptr<TextFieldView>(field.node())](const gx::Key& k, bool force) {
                        if (auto f = weak.lock(); f && (force || !f->isFocused()) && f->text() != k.getText())
                            f->setText(k.getText());
                    });
}

FormRow infoRow(const std::string& title, const std::string& value) {
    return infoRow(title, Label(value).secondary().textAlign(TextAlign::Trailing).maxWidth(360));
}

FormRow infoRow(const std::string& title, const Label& value) { return FormRow(title, value); }

// Sections ---------------------------------------------------------------------------------------------

Element section(const std::string& title, const std::vector<Element>& rows, const std::string& footer) {
    auto s = VStack({}).spacing(8);
    if (!title.empty()) s.add(Label(title).font(TextStyle::Headline).padding(Insets(0, 4)));
    auto card = std::make_shared<FormCardView>();
    for (const auto& r : rows) card->addChild(r.widget());
    s.add(Element(card));
    if (!footer.empty()) s.add(Label(footer).font(TextStyle::Subheadline).secondary().padding(Insets(0, 4)));
    return s;
}

DynamicSection::DynamicSection(const std::string& title, std::string emptyText, const std::string& footer)
    : m_root(VStack({}).spacing(8)),
      m_title(Label(title).font(TextStyle::Headline)),
      m_titleLine(HStack({m_title, Spacer()}).spacing(8).align(Alignment::Center).padding(Insets(0, 4))),
      m_card(std::make_shared<FormCardView>()),
      m_emptyText(std::move(emptyText)) {
    m_titleLine.setHidden(title.empty());
    m_root.add(m_titleLine);
    m_root.add(Element(m_card));
    if (!footer.empty()) m_root.add(Label(footer).font(TextStyle::Subheadline).secondary().padding(Insets(0, 4)));
    setRows({});
}

void DynamicSection::setRows(const std::vector<Element>& rows) {
    m_card->clearChildren();
    for (const auto& r : rows) m_card->addChild(r.widget());
    if (rows.empty()) m_card->addChild(FormRow(Label(m_emptyText).secondary()).node());
}

void DynamicSection::setTitle(const std::string& title) {
    m_title.setText(title);
    m_titleLine.setHidden(title.empty());
}

void DynamicSection::setAccessory(const Element& accessory) { m_titleLine.add(accessory); }

// Clickable rows ------------------------------------------------------------------------------------------

ClickRowView::ClickRowView() : StackView(Axis::Horizontal) {
    setAlignment(Alignment::Leading);
    setSpacing(10);
    setPadding(Insets(7, 12));
    setMinSize(std::nullopt, 38);
}

void ClickRowView::setChevron(bool chevron) {
    m_chevron = chevron;
    setPadding(Insets(7, 12, 7, chevron ? 30 : 12));
    invalidateLayout();
}

void ClickRowView::setSelected(bool selected) {
    if (m_selected == selected) return;
    m_selected = selected;
    invalidate();
}

void ClickRowView::draw(Painter& p) {
    const auto& c = theme().colors;
    const Rect r = frame().inset(4, 3);
    if (m_selected) p.fillRoundedRect(r, 8, c.accent.withAlpha(0.14f));
    if (isEnabled() && (isPressed() || isHovered())) p.fillRoundedRect(r, 8, isPressed() ? c.fill : c.hover);
    if (m_chevron) {
        const Rect f = frame();
        p.drawIcon(Icon::ChevronRight, {f.right() - 26, f.center().y - 6, 12, 12}, c.tertiaryLabel, 1.1f);
    }
    paintChildren(p);
}

void ClickRowView::drawOverlay(Painter& p) {
    if (isFocusVisible()) p.drawFocusRing(frame().inset(4, 3), 8);
}

bool ClickRowView::onPointerDown(const PointerEvent& e) { return e.button == MouseButton::Left && m_onClick; }

void ClickRowView::onPointerUp(const PointerEvent& e) {
    if (frame().contains(e.position) && m_onClick) m_onClick();
}

bool ClickRowView::onKeyDown(const KeyEvent& e) {
    if ((e.key == Key::Space || e.key == Key::Enter) && m_onClick) {
        m_onClick();
        return true;
    }
    return false;
}

ClickRow::ClickRow(std::initializer_list<Element> children) : ContainerHandle(std::make_shared<ClickRowView>()) {
    addAll(children);
}

ClickRow::ClickRow(const std::vector<Element>& children) : ContainerHandle(std::make_shared<ClickRowView>()) {
    addAll(children);
}

// Choice grid -----------------------------------------------------------------------------------------------

namespace {
constexpr float kCaptionHeight = 20;
}

ChoiceGridView::ChoiceGridView(int count, Size cell, float gap, DrawFn draw)
    : m_count(count), m_cell(cell), m_gap(gap), m_draw(std::move(draw)) {}

void ChoiceGridView::setCount(int count) {
    m_count = count;
    if (m_selected >= count) m_selected = -1;
    invalidateLayout();
}

void ChoiceGridView::setSelected(int index) {
    if (index == m_selected) return;
    m_selected = index;
    invalidate();
}

void ChoiceGridView::setWraps(bool wraps) {
    m_wraps = wraps;
    invalidateLayout();
}

int ChoiceGridView::columns(float width) const {
    if (!m_wraps || !std::isfinite(width)) return std::max(1, m_count);
    return std::clamp(static_cast<int>((width + m_gap) / (m_cell.width + m_gap)), 1, std::max(1, m_count));
}

Size ChoiceGridView::measureContent(Size available) {
    // Room for the selection ring around the outermost cells.
    const int cols = columns(available.width - 8);
    const int rows = (m_count + cols - 1) / std::max(1, cols);
    const auto fc = static_cast<float>(cols);
    const auto fr = static_cast<float>(rows);
    return {fc * m_cell.width + (fc - 1) * m_gap + 8,
            fr * m_cell.height + std::max(0.0f, fr - 1) * m_gap + 8 + (m_caption ? kCaptionHeight : 0)};
}

Rect ChoiceGridView::cellRect(int index) const {
    const Rect c = contentRect();
    const int cols = columns(c.width - 8);
    const int row = index / cols;
    const int col = index % cols;
    return {c.x + 4 + static_cast<float>(col) * (m_cell.width + m_gap),
            c.y + 4 + static_cast<float>(row) * (m_cell.height + m_gap), m_cell.width, m_cell.height};
}

int ChoiceGridView::indexAt(Point p) const {
    for (int i = 0; i < m_count; ++i) {
        if (cellRect(i).inset(-m_gap * 0.5f).contains(p)) return i;
    }
    return -1;
}

void ChoiceGridView::choose(int index) {
    if (index < 0 || index >= m_count) return;
    m_selected = index;
    invalidate();
    if (m_onChoose) m_onChoose(index);
}

void ChoiceGridView::draw(Painter& p) {
    for (int i = 0; i < m_count; ++i) {
        p.pushOpacity(i == m_pressed && isPressed() ? 0.75f : 1.0f);
        m_draw(p, cellRect(i), i, i == m_selected, i == m_hover);
        p.popOpacity();
    }
    if (m_caption) {
        const int named = m_hover >= 0 ? m_hover : m_selected;
        if (named >= 0) {
            const Rect c = contentRect();
            const Rect last = cellRect(m_count - 1);
            const float w = static_cast<float>(columns(c.width - 8)) * (m_cell.width + m_gap) - m_gap;
            p.drawText(mayo::uiText(m_caption(named)), {c.x + 4, last.bottom() + 6, w, kCaptionHeight - 4},
                       theme().font(TextStyle::Caption), theme().colors.secondaryLabel, TextAlign::Center);
        }
    }
}

void ChoiceGridView::drawOverlay(Painter& p) {
    const float a = focusAmount();
    if (a > 0.01f && m_selected >= 0) p.drawFocusRing(cellRect(m_selected).inset(-3), 8, a);
}

bool ChoiceGridView::onPointerDown(const PointerEvent& e) {
    if (e.button != MouseButton::Left) return false;
    m_pressed = indexAt(e.position);
    invalidate();
    return m_pressed >= 0;
}

void ChoiceGridView::onPointerUp(const PointerEvent& e) {
    const int i = indexAt(e.position);
    if (i >= 0 && i == m_pressed) choose(i);
    m_pressed = -1;
    invalidate();
}

void ChoiceGridView::onPointerMove(const PointerEvent& e) {
    const int h = indexAt(e.position);
    if (h != m_hover) {
        m_hover = h;
        invalidate();
    }
}

void ChoiceGridView::onPointerLeave() {
    m_hover = -1;
    invalidate();
}

bool ChoiceGridView::onKeyDown(const KeyEvent& e) {
    const int cur = std::max(0, m_selected);
    const int cols = columns(contentRect().width - 8);
    if (e.key == Key::Left) choose(std::max(0, cur - 1));
    else if (e.key == Key::Right) choose(std::min(m_count - 1, cur + 1));
    else if (e.key == Key::Up && cur - cols >= 0) choose(cur - cols);
    else if (e.key == Key::Down && cur + cols < m_count) choose(cur + cols);
    else return false;
    return true;
}

ClickRow linkRow(Icon icon, Color color, const std::string& title, const std::string& value,
                 std::function<void()> onClick) {
    auto row = ClickRow({tile(icon, color), Label(title), Spacer()});
    if (!value.empty()) row.add(Label(value).secondary());
    row.chevron().onClick(std::move(onClick));
    return row;
}

// Icons and page chrome --------------------------------------------------------------------------------------

Canvas tile(Icon icon, Color color, float size) {
    auto c = Canvas([icon, color](Painter& p, const Rect& r) {
        p.fillVerticalGradient(r, r.width * 0.26f, color.mix(Color::white(), 0.12f), color);
        p.strokeRoundedRect(r, r.width * 0.26f, Color::black(0.08f), 0.5f);
        p.drawIcon(icon, r.inset(r.width * 0.2f), Color::white(), 1.0f);
    });
    c.frame(size, size);
    return c;
}

Canvas letterTile(const std::string& name, float size) {
    std::uint32_t h = 2166136261u;
    for (unsigned char ch : name) h = (h ^ ch) * 16777619u;
    std::string letter = name.empty() ? "?" : name.substr(0, 1);
    if (!letter.empty() && static_cast<unsigned char>(letter[0]) >= 0x80) {
        // Keep the whole UTF-8 sequence of a non-ASCII first letter.
        size_t n = 1;
        while (n < name.size() && (static_cast<unsigned char>(name[n]) & 0xC0) == 0x80) ++n;
        letter = name.substr(0, n);
    } else if (!letter.empty()) {
        letter[0] = static_cast<char>(std::toupper(static_cast<unsigned char>(letter[0])));
    }
    auto c = Canvas([h, letter](Painter& p, const Rect& r) {
        const auto& col = theme().colors;
        const Color palette[] = {col.blue, col.purple, col.pink, col.red, col.orange, col.green, col.teal, col.indigo};
        const Color base = palette[h % 8];
        p.fillVerticalGradient(r, r.width * 0.26f, base.mix(Color::white(), 0.15f), base);
        p.drawText(letter, r, theme().font(TextStyle::Callout).withWeight(FontWeight::Semibold).withSize(r.height * 0.5f),
                   Color::white(), TextAlign::Center);
    });
    c.frame(size, size);
    return c;
}

Canvas glyph(Icon icon, float size, bool secondary) {
    auto c = Canvas([icon, secondary](Painter& p, const Rect& r) {
        p.drawIcon(icon, r, secondary ? theme().colors.secondaryLabel : theme().colors.label);
    });
    c.frame(size, size);
    return c;
}

Element hero(Icon icon, Color color, const std::string& title, const std::string& description) {
    auto text = VStack({Label(title).font(TextStyle::Title2).weight(FontWeight::Semibold),
                        Label(description).font(TextStyle::Callout).secondary()})
                    .spacing(3);
    auto card = std::make_shared<FormCardView>();
    card->addChild(HStack({tile(icon, color, 44), text}).spacing(14).align(Alignment::Center).padding(14).node());
    return Element(card);
}

namespace {
class PageView : public ScrollViewView {
public:
    PageView(std::shared_ptr<Widget> content, std::shared_ptr<void> model)
        : ScrollViewView(std::move(content)), m_model(std::move(model)) {}

private:
    std::shared_ptr<void> m_model;
};
}  // namespace

Element page(const std::vector<Element>& sections, std::shared_ptr<void> model) {
    std::vector<Element> visible;
    if (gx::readOnly())
        visible.push_back(FormRow(
            Label("Read-only mode — system settings will not be changed").font(TextStyle::Callout).secondary()));
    for (const auto& s : sections) {
        if (s) visible.push_back(s);
    }
    auto column = VStack(visible).spacing(22).maxWidth(kPageWidth);
    // Leading-aligned column, centred in wide windows by a flexible margin on both sides.
    auto body = HStack({Spacer(), column, Spacer()}).spacing(0).align(Alignment::TopLeading).padding(Insets(8, 28, 32, 28));
    return Element(std::make_shared<PageView>(body.node(), std::move(model)));
}

Element unavailable(Icon icon, const std::string& title, const std::string& text) {
    auto icn = glyph(icon, 48);
    return VStack({icn, Label(title).font(TextStyle::Title3), Label(text).secondary().textAlign(TextAlign::Center).maxWidth(380)})
        .spacing(10)
        .align(Alignment::Center);
}

Canvas signalIcon(int strength, bool secure) {
    auto c = Canvas([strength, secure](Painter& p, const Rect& r) {
        const auto& col = theme().colors;
        const int bars = strength > 75 ? 4 : strength > 50 ? 3 : strength > 25 ? 2 : 1;
        const float s = std::min(r.width, r.height);
        const Point base{r.x + s * 0.5f, r.y + s * 0.82f};
        constexpr float kPi = 3.14159265f;
        for (int i = 1; i <= 3; ++i) {
            const float radius = s * (0.2f + 0.2f * static_cast<float>(i));
            p.drawArc(base, radius, kPi * 1.25f, kPi * 0.5f, i < bars ? col.label : col.quaternaryLabel, s * 0.1f);
        }
        p.fillCircle(base, s * 0.07f, col.label);
        if (secure) p.drawIcon(Icon::Lock, {r.right() - s * 0.42f, r.bottom() - s * 0.42f, s * 0.42f, s * 0.42f}, col.secondaryLabel, 1.2f);
    });
    c.frame(18, 18);
    return c;
}

Canvas batteryIcon(double fraction, bool charging, float width) {
    auto c = Canvas([fraction, charging](Painter& p, const Rect& r) {
        const auto& col = theme().colors;
        const Rect body{r.x, r.y + r.height * 0.18f, r.width - 3, r.height * 0.64f};
        p.strokeRoundedRect(body, 3.5f, col.secondaryLabel, 1);
        p.fillRoundedRect({body.right() + 0.5f, body.center().y - 2.5f, 2, 5}, 1, col.secondaryLabel);
        const float f = static_cast<float>(std::clamp(fraction, 0.0, 1.0));
        const Color fill = charging ? col.green : f < 0.2f ? col.red : col.label;
        const Rect inner = body.inset(2);
        p.fillRoundedRect({inner.x, inner.y, std::max(2.0f, inner.width * f), inner.height}, 2, fill);
    });
    c.frame(width, width * 0.55f);
    return c;
}

// Popovers ------------------------------------------------------------------------------------------------------

void chooseFromList(const Element& anchor, const std::vector<Option>& items,
                    std::function<void(const std::string&)> onPick, const std::string& placeholder) {
    auto list = VStack({}).spacing(0);
    auto field = SearchField(placeholder);
    auto pop = Popover(VStack({field, ScrollView(list).frame(340, 300)}).spacing(8));
    // Rows only hold a weak reference: the window's overlay list keeps the popover alive while it is shown.
    std::weak_ptr<PopoverView> weak = pop.node();
    auto fill = [items, list, weak, onPick](const std::string& query) mutable {
        list.clear();
        const std::string q = lower(query);
        int shown = 0;
        for (const auto& it : items) {
            if (!q.empty() && lower(it.label).find(q) == std::string::npos && lower(it.value).find(q) == std::string::npos)
                continue;
            if (++shown > 200) break;  // the rest is reachable by typing
            list.add(ClickRow({Label(it.label).lineLimit(1)}).onClick([weak, onPick, v = it.value] {
                if (auto p = weak.lock()) p->dismiss();
                onPick(v);
            }));
        }
        if (shown == 0) list.add(Label("No Results").secondary().padding(10));
    };
    fill({});
    field.onChange([fill](const std::string& q) mutable { fill(q); });
    pop.show(anchor);
    if (auto* host = field->host()) host->setFocus(field.node().get(), false);
}

void message(const Element& anchor, const std::string& title, const std::string& text) {
    auto weak = std::make_shared<std::weak_ptr<PopoverView>>();
    auto ok = Button("OK").style(ButtonStyle::Prominent).onClick([weak] {
        if (auto p = weak->lock()) p->dismiss();
    });
    auto pop = Popover(VStack({Label(title).font(TextStyle::Headline).maxWidth(320),
                               Label(text).secondary().font(TextStyle::Subheadline).maxWidth(320), HStack({Spacer(), ok})})
                           .spacing(8)
                           .width(330));
    *weak = pop.node();
    pop.show(anchor);
}

void confirm(const Element& anchor, const std::string& title, const std::string& text, const std::string& action,
             std::function<void()> onConfirm, bool destructive) {
    auto weak = std::make_shared<std::weak_ptr<PopoverView>>();
    auto cancel = Button("Cancel").onClick([weak] {
        if (auto p = weak->lock()) p->dismiss();
    });
    auto go = Button(action)
                  .style(destructive ? ButtonStyle::Destructive : ButtonStyle::Prominent)
                  .onClick([weak, onConfirm] {
                      if (auto p = weak->lock()) p->dismiss();
                      onConfirm();
                  });
    auto pop = Popover(VStack({Label(title).font(TextStyle::Headline).maxWidth(320),
                               Label(text).secondary().font(TextStyle::Subheadline).maxWidth(320),
                               HStack({Spacer(), cancel, go}).spacing(8)})
                           .spacing(8)
                           .width(330));
    *weak = pop.node();
    pop.show(anchor);
}

// Text --------------------------------------------------------------------------------------------------------

std::string formatDuration(std::int64_t seconds) {
    if (seconds <= 0) return "Never";
    auto plural = [](std::int64_t n, const char* unit) {
        return std::to_string(n) + " " + unit + (n == 1 ? "" : "s");
    };
    if (seconds % 3600 == 0) return plural(seconds / 3600, "hour");
    if (seconds % 60 == 0) return plural(seconds / 60, "minute");
    return plural(seconds, "second");
}

std::string formatBytes(double bytes) {
    const char* units[] = {"B", "KB", "MB", "GB", "TB", "PB"};
    int u = 0;
    while (bytes >= 1024 && u < 5) {
        bytes /= 1024;
        ++u;
    }
    char buf[32];
    std::snprintf(buf, sizeof buf, u == 0 || bytes >= 100 ? "%.0f %s" : "%.1f %s", bytes, units[u]);
    return buf;
}

std::string lower(std::string s) {
    std::transform(s.begin(), s.end(), s.begin(), [](unsigned char c) { return static_cast<char>(std::tolower(c)); });
    return s;
}

std::string trim(const std::string& s) {
    size_t b = s.find_first_not_of(" \t\r\n");
    if (b == std::string::npos) return {};
    size_t e = s.find_last_not_of(" \t\r\n");
    return s.substr(b, e - b + 1);
}

std::vector<std::string> split(const std::string& s, char sep) {
    std::vector<std::string> out;
    size_t start = 0;
    while (true) {
        size_t i = s.find(sep, start);
        out.push_back(s.substr(start, i - start));
        if (i == std::string::npos) break;
        start = i + 1;
    }
    return out;
}

}  // namespace ui
