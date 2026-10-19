#include "capture_view.hpp"
#include <algorithm>
#include <array>
#include <cmath>

using namespace mayo;
namespace shot {
namespace {
Segment segment(Icon icon, const char* title) {
    Segment s(title);
    s.icon = icon;
    return s;
}
std::shared_ptr<ButtonView> button(Icon icon, const char* title, float width, ButtonStyle style,
                                   std::function<void()> action) {
    auto b = std::make_shared<ButtonView>(title);
    b->setIcon(icon);
    b->setStyle(style);
    b->setControlSize(ControlSize::Large);
    b->setFixedWidth(width);
    b->setFixedHeight(40);
    b->setOnClick(std::move(action));
    return b;
}
}  // namespace
CaptureView::CaptureView() {
    m_hud = std::make_shared<ToolbarGroupView>();
    m_hud->setPadding(Insets(10, 12));
    m_hud->setSpacing(9);
    m_modes = std::make_shared<SegmentedControlView>(
        std::vector<Segment>{segment(Icon::Square, "区域"), segment(Icon::Display, "全屏"),
                             segment(Icon::Windows, "窗口")},
        0);
    m_modes->setFixedWidth(258);
    m_modes->setFixedHeight(40);
    m_modes->setOnChange([this](int index) {
        if (onMode) onMode(static_cast<Mode>(index));
    });
    m_modes->setTooltip("切换截图模式 · 1 / 2 / 3");
    m_hud->addChild(m_modes);
    auto divider = std::make_shared<DividerView>();
    divider->setFixedWidth(1);
    divider->setFixedHeight(24);
    m_hud->addChild(divider);
    m_dimensions = std::make_shared<LabelView>("选择区域");
    m_dimensions->setFont(Font{11, FontWeight::Medium});
    m_dimensions->setTextAlign(TextAlign::Center);
    m_dimensions->setColorRole(LabelColor::Secondary);
    m_dimensions->setFixedWidth(88);
    m_dimensions->setLineLimit(1);
    m_hud->addChild(m_dimensions);
    m_save = button(Icon::Import, "保存", 82, ButtonStyle::Borderless, [this] { requestOutput(false); });
    m_save->setTooltip("保存到截图文件夹 · S / Ctrl+S");
    m_copy = button(Icon::DocOnDoc, "复制", 92, ButtonStyle::Prominent, [this] { requestOutput(true); });
    m_copy->setTooltip("复制图片 · Enter / Ctrl+C");
    m_cancel = button(Icon::Xmark, "", 36, ButtonStyle::Borderless, [this] {
        if (onCancel) onCancel();
    });
    m_cancel->setTooltip("取消截图 · Esc");
    m_hud->addChild(m_save);
    m_hud->addChild(m_copy);
    m_hud->addChild(m_cancel);
    addChild(m_hud);
    setCursor(CursorShape::Crosshair);
}
void CaptureView::present(Image image, Mode mode) {
    m_image = std::move(image);
    // Retain a dynamic GPU texture between activations whenever dimensions match.
    if (!m_texture || m_texture->width() != m_image.width || m_texture->height() != m_image.height)
        m_texture = ImageData::create(m_image.width, m_image.height);
    m_texture->update(m_image.rgba, mode != Mode::Window);
    m_mode = mode;
    m_selection = {};
    m_drag = None;
    m_message.clear();
    m_busy = false;
    m_exiting = false;
    m_pointerKnown = false;
    m_reveal.set(0);
    m_reveal.animateTo(1, now(), {0.18f, Curve::EaseOut});
    m_exit.set(1);
    m_modes->setSelected(static_cast<int>(mode));
    m_hudPosition.set({});
    updateControls();
    invalidateLayout();
}
void CaptureView::setMode(Mode mode) {
    m_mode = mode;
    m_drag = None;
    m_selection = {};
    m_modes->setSelected(static_cast<int>(mode));
    updateControls();
    invalidateLayout();
}
void CaptureView::disappear() {
    m_exiting = true;
    m_drag = None;
    m_exit.animateTo(0, now(), {0.11f, Curve::EaseOut});
    m_hud->setDisabled(true);
    invalidate();
}
void CaptureView::resume() {
    m_exiting = false;
    m_exit.set(1);
    m_busy = false;
    updateControls();
    invalidate();
}
void CaptureView::setBusy(bool busy) {
    m_busy = busy;
    updateControls();
    invalidate();
}
void CaptureView::setMessage(std::string message) {
    m_message = std::move(message);
    invalidate();
}
bool CaptureView::hasSelection() const {
    return m_mode != Mode::Region || (m_selection.width >= 1 && m_selection.height >= 1);
}
void CaptureView::updateControls() {
    m_save->setDisabled(m_busy || !hasSelection());
    m_copy->setDisabled(m_busy || !hasSelection());
    m_copy->setStyle(hasSelection() ? ButtonStyle::Prominent : ButtonStyle::Borderless);
    m_modes->setDisabled(m_busy);
    m_hud->setDisabled(false);
    m_dimensions->setText(m_busy ? "正在处理…" : dimensions());
}
Rect CaptureView::imageRect() const {
    auto b = frame();
    if (m_mode != Mode::Window) return b;
    const float ratio = std::min((b.width - 120) / m_image.width, (b.height - 200) / m_image.height);
    Size size{m_image.width * ratio, m_image.height * ratio};
    return {(b.width - size.width) / 2, (b.height - size.height) / 2 - 22, size.width, size.height};
}
Rect CaptureView::hudRect() const {
    auto b = frame();
    float width = std::min(690.f, b.width - 32);
    float x = (b.width - width) / 2;
    float y = b.height - 106;
    if (m_mode == Mode::Region && hasSelection()) {
        x = std::clamp(m_selection.center().x - width / 2, 16.f, b.width - width - 16);
        if (m_selection.bottom() + 94 < b.height)
            y = m_selection.bottom() + 20;
        else if (m_selection.top() > 104)
            y = m_selection.top() - 82;
    }
    return {x, y, width, 60};
}
std::string CaptureView::dimensions() const {
    if (!m_image.width) return "";
    if (!hasSelection()) return "选择区域";
    int width = m_image.width, height = m_image.height;
    if (m_mode == Mode::Region && frame().width > 0) {
        width = static_cast<int>(std::round(m_selection.right() * m_image.width / frame().width) -
                                 std::round(m_selection.x * m_image.width / frame().width));
        height = static_cast<int>(std::round(m_selection.bottom() * m_image.height / frame().height) -
                                  std::round(m_selection.y * m_image.height / frame().height));
    }
    return std::to_string(width) + " × " + std::to_string(height);
}
Image CaptureView::result() const {
    if (m_mode != Mode::Region) return m_image;
    return cropLogical(m_image, m_selection.x, m_selection.y, m_selection.width, m_selection.height, frame().width,
                       frame().height);
}
void CaptureView::arrangeContent(const Rect&) {
    auto target = hudRect();
    if (m_hudPosition.target().isEmpty())
        m_hudPosition.set(target);
    else
        m_hudPosition.animateTo(target, now(), {0.20f, Curve::EaseOut});
    m_toolbar = m_hudPosition.value(now());
    m_hud->measure(m_toolbar.size());
    m_hud->arrange(m_toolbar);
}
void CaptureView::draw(Painter& p) {
    if (!m_texture) return;
    auto b = frame();
    float reveal = sample(m_reveal), exit = sample(m_exit);
    if (m_mode == Mode::Window) {
        // A softly blurred desktop-style surround keeps a window preview legible.
        p.fillRect(b, theme().colors.contentBackground);
        auto preview = imageRect();
        p.drawShadow(preview, 16, Color::black(.22f), 36, {0, 12});
        p.drawImage(preview, *m_texture, 12);
        p.strokeRoundedRect(preview, 12, Color::white(.45f), .5f);
    } else
        p.drawImage(b, *m_texture);
    if (m_mode == Mode::Region)
        drawSelection(p, reveal * exit);
    else if (m_mode == Mode::Fullscreen)
        p.fillRect(b, Color::black(.07f * reveal * exit));

    const std::string hint = !m_message.empty()       ? m_message
                             : m_busy                 ? "正在处理截图…"
                             : m_mode == Mode::Region ? (hasSelection() ? "拖动调整位置 · 边缘调整大小 · 双击复制"
                                                                        : "拖动选择区域  ·  按住 ⇧ 选择正方形")
                             : m_mode == Mode::Window ? "窗口截图  ·  保留原始分辨率"
                                                      : "全屏截图  ·  保留原始分辨率";
    Font hintFont{12, FontWeight::Medium};
    float hintWidth = p.measureText(hint, hintFont) + 44;
    Rect hintBox{(b.width - hintWidth) / 2, 28 - (1 - reveal) * 6, hintWidth, 36};
    p.pushOpacity(reveal * exit);
    p.drawMaterial(hintBox, 18, Material::GlassClear);
    p.drawShadow(hintBox, 18, Color::black(.12f), 16, {0, 4}, 0, true);
    p.drawText(hint, hintBox, hintFont, theme().colors.label, TextAlign::Center);
    p.popOpacity();

    auto target = hudRect();
    if (target != m_hudPosition.target()) m_hudPosition.animateTo(target, now(), {0.20f, Curve::EaseOut});
    m_toolbar = sample(m_hudPosition).translated({0, (1 - reveal) * 12 + (1 - exit) * 8});
    m_hud->arrange(m_toolbar);
    m_hud->setOpacity(reveal * exit * (m_drag != None ? .30f : 1.f));
    m_hud->setHidden(m_drag != None);
    if (m_drag == None) p.drawShadow(m_toolbar, 30, Color::black(.16f * reveal * exit), 28, {0, 8}, 0, true);
    paintChildren(p);
    if (!m_busy && m_drag == None) {
        p.pushOpacity(reveal * exit);
        std::string help = "Enter 复制      S 保存      Esc 取消";
        Rect footer{m_toolbar.center().x - 125, m_toolbar.bottom() + 10, 250, 24};
        p.fillRoundedRect(footer, 12, Color::black(.30f));
        p.drawText(help, footer, Font{10}, Color::white(.94f), TextAlign::Center);
        p.popOpacity();
    }
}
void CaptureView::drawSelection(Painter& p, float reveal) {
    auto b = frame();
    Color dim = Color::black(.30f * reveal);
    if (!hasSelection())
        p.fillRect(b, dim);
    else {
        auto r = m_selection;
        p.fillRect({0, 0, b.width, r.y}, dim);
        p.fillRect({0, r.bottom(), b.width, b.height - r.bottom()}, dim);
        p.fillRect({0, r.y, r.x, r.height}, dim);
        p.fillRect({r.right(), r.y, b.width - r.right(), r.height}, dim);
        p.strokeRoundedRect(r.outset(1), 1, Color::black(.38f), 1);
        p.strokeRoundedRect(r, 1, Color::white(.96f), 1);
        if (m_drag == None) {
            std::array<Point, 8> handles{{r.origin(),
                                          {r.center().x, r.y},
                                          {r.right(), r.y},
                                          {r.right(), r.center().y},
                                          {r.right(), r.bottom()},
                                          {r.center().x, r.bottom()},
                                          {r.x, r.bottom()},
                                          {r.x, r.center().y}}};
            for (auto q : handles) {
                p.drawShadow({q.x - 3, q.y - 3, 6, 6}, 3, Color::black(.3f), 3, {0, 1});
                p.fillCircle(q, 3, Color::white());
            }
        }
        auto text = dimensions();
        float w = p.measureText(text, Font{11, FontWeight::Medium}) + 28;
        Rect badge{std::clamp(r.center().x - w / 2, 8.f, b.width - w - 8), std::max(8.f, r.y - 34), w, 25};
        p.drawMaterial(badge, 12.5f, Material::GlassClear);
        p.drawText(text, badge, Font{11, FontWeight::Medium}, theme().colors.label, TextAlign::Center);
    }
    if (m_pointerKnown && !hasSelection() && !m_toolbar.outset(10).contains(m_pointer)) {
        p.drawLine({m_pointer.x, 0}, {m_pointer.x, b.height}, Color::white(.22f), .5f);
        p.drawLine({0, m_pointer.y}, {b.width, m_pointer.y}, Color::white(.22f), .5f);
        p.strokeCircle(m_pointer, 5, Color::white(.85f), 1);
    }
}
CaptureView::Drag CaptureView::hitHandle(Point q) const {
    if (!hasSelection() || m_mode != Mode::Region) return None;
    auto r = m_selection;
    bool l = std::abs(q.x - r.x) < 7, rr = std::abs(q.x - r.right()) < 7;
    bool t = std::abs(q.y - r.y) < 7, bb = std::abs(q.y - r.bottom()) < 7;
    if (l && t) return NW;
    if (rr && t) return NE;
    if (rr && bb) return SE;
    if (l && bb) return SW;
    if (q.x >= r.x && q.x <= r.right()) {
        if (t) return N;
        if (bb) return S;
    }
    if (q.y >= r.y && q.y <= r.bottom()) {
        if (l) return W;
        if (rr) return E;
    }
    return r.contains(q) ? Move : None;
}
void CaptureView::updateCursor(Point q) {
    CursorShape cursor = CursorShape::Crosshair;
    if (m_mode != Mode::Region || m_toolbar.contains(q))
        cursor = CursorShape::Arrow;
    else
        switch (hitHandle(q)) {
            case NW:
            case SE: cursor = CursorShape::ResizeNWSE; break;
            case NE:
            case SW: cursor = CursorShape::ResizeNESW; break;
            case N:
            case S: cursor = CursorShape::ResizeVertical; break;
            case W:
            case E: cursor = CursorShape::ResizeHorizontal; break;
            case Move: cursor = CursorShape::Move; break;
            default: break;
        }
    setCursor(cursor);
    if (host()) host()->setCursor(cursor);
}
bool CaptureView::onPointerDown(const PointerEvent& e) {
    if (e.button == MouseButton::Right) {
        if (onCancel) onCancel();
        return true;
    }
    if (e.button != MouseButton::Left || m_busy || m_exiting || m_toolbar.contains(e.position)) return false;
    if (m_mode != Mode::Region) return true;
    if (e.clickCount == 2 && hasSelection() && m_selection.contains(e.position)) {
        requestOutput(true);
        return true;
    }
    m_anchor = e.position;
    m_before = m_selection;
    m_drag = hitHandle(e.position);
    if (m_drag == None) {
        m_drag = Create;
        m_selection = {};
    }
    invalidate();
    return true;
}
void CaptureView::onPointerMove(const PointerEvent& e) {
    if (m_busy || m_exiting) return;
    m_pointer = {std::clamp(e.position.x, 0.f, frame().width), std::clamp(e.position.y, 0.f, frame().height)};
    m_pointerKnown = true;
    if (m_drag == Create) {
        auto delta = m_pointer - m_anchor;
        if (hasModifier(e.modifiers, Modifiers::Shift)) {
            float side = std::min(std::abs(delta.x), std::abs(delta.y));
            delta = {std::copysign(side, delta.x), std::copysign(side, delta.y)};
        }
        auto end = m_anchor + delta;
        m_selection = Rect::fromLTRB(std::min(m_anchor.x, end.x), std::min(m_anchor.y, end.y),
                                     std::max(m_anchor.x, end.x), std::max(m_anchor.y, end.y));
    } else if (m_drag == Move) {
        auto d = m_pointer - m_anchor;
        m_selection = {std::clamp(m_before.x + d.x, 0.f, frame().width - m_before.width),
                       std::clamp(m_before.y + d.y, 0.f, frame().height - m_before.height), m_before.width,
                       m_before.height};
    } else if (m_drag != None) {
        auto r = m_before;
        auto d = m_pointer - m_anchor;
        float l = r.x, t = r.y, rr = r.right(), bb = r.bottom();
        if (m_drag == NW || m_drag == W || m_drag == SW) l = std::clamp(l + d.x, 0.f, std::max(0.f, rr - 2));
        if (m_drag == NE || m_drag == E || m_drag == SE)
            rr = std::clamp(rr + d.x, std::min(l + 2, frame().width), frame().width);
        if (m_drag == NW || m_drag == N || m_drag == NE) t = std::clamp(t + d.y, 0.f, std::max(0.f, bb - 2));
        if (m_drag == SW || m_drag == S || m_drag == SE)
            bb = std::clamp(bb + d.y, std::min(t + 2, frame().height), frame().height);
        m_selection = Rect::fromLTRB(l, t, rr, bb);
    }
    updateCursor(m_pointer);
    if (m_drag != None) updateControls();
    if (m_mode == Mode::Region) invalidate();
}
void CaptureView::onPointerUp(const PointerEvent& event) {
    bool created = m_drag == Create;
    m_drag = None;
    m_hud->setHidden(false);
    updateControls();
    invalidate();
    if (created && frame().contains(event.position) && hasSelection() && onSelected) onSelected();
}
void CaptureView::requestOutput(bool copy) {
    if (hasSelection() && !m_busy && onOutput) onOutput(copy);
}
bool CaptureView::key(const KeyEvent& e) {
    if (m_exiting) return true;
    if (e.key == Key::Escape) {
        if (onCancel) onCancel();
        return true;
    }
    if (m_busy) return true;
    if (e.key == Key::Enter || e.key == Key::C) {
        requestOutput(true);
        return true;
    }
    if (e.key == Key::S) {
        requestOutput(false);
        return true;
    }
    if (e.key >= Key::Num1 && e.key <= Key::Num3) {
        if (onMode) onMode(static_cast<Mode>(static_cast<int>(e.key) - static_cast<int>(Key::Num1)));
        return true;
    }
    if (m_mode == Mode::Region && hasSelection()) {
        float delta = (e.shift() ? 10.f : 1.f) * frame().width / m_image.width;
        Point d;
        if (e.key == Key::Left)
            d.x = -delta;
        else if (e.key == Key::Right)
            d.x = delta;
        else if (e.key == Key::Up)
            d.y = -delta;
        else if (e.key == Key::Down)
            d.y = delta;
        else
            return false;
        m_selection.x = std::clamp(m_selection.x + d.x, 0.f, frame().width - m_selection.width);
        m_selection.y = std::clamp(m_selection.y + d.y, 0.f, frame().height - m_selection.height);
        invalidate();
        return true;
    }
    return false;
}
void CaptureView::demoSelection() {
    auto b = frame();
    m_selection = {b.width * .20f, b.height * .20f, b.width * .60f, b.height * .50f};
    updateControls();
    invalidate();
}
}  // namespace shot
