#include "store_ui.hpp"

#include <algorithm>
#include <cmath>
#include <cstdio>
#include <ctime>
#include <map>

using namespace mayo;

namespace forma::store {

namespace {

// --- metrics -------------------------------------------------------------------------------------

constexpr float kPagePad = 32;      ///< side padding of every page
constexpr float kHeroHeight = 260;  ///< discover hero
constexpr float kCard = 150;        ///< icon cards in carousels
constexpr float kGridCard = 138;    ///< icon cards in grids
constexpr float kCardHeight = 184;  ///< fixed rhythm, independent of grid column width
constexpr float kGap = 18;
constexpr float kBlock = 30;  ///< vertical gap between blocks

Font titleFont() { return Font{30, FontWeight::Bold, 36}; }
Font headingFont() { return Font{20, FontWeight::Bold, 26}; }
Font cardTitleFont() { return Font{13, FontWeight::Semibold, 18}; }
Font cardSubFont() { return Font{11, FontWeight::Regular, 15}; }
Font smallFont() { return Font{10, FontWeight::Regular, 13}; }

double clockNow() {
    struct timespec ts {};
    clock_gettime(CLOCK_MONOTONIC, &ts);
    return static_cast<double>(ts.tv_sec) + static_cast<double>(ts.tv_nsec) / 1e9;
}

std::string cutText(Painter& p, const std::string& text, const Font& font, float width) {
    if (width <= 0 || text.empty()) return {};
    if (p.measureText(text, font) <= width) return text;
    std::vector<std::size_t> cuts;
    for (std::size_t i = 1; i < text.size(); ++i) {
        if ((static_cast<unsigned char>(text[i]) & 0xC0) != 0x80) cuts.push_back(i);
    }
    std::size_t lo = 0;
    std::size_t hi = cuts.size();
    while (lo < hi) {
        const std::size_t mid = (lo + hi + 1) / 2;
        if (p.measureText(text.substr(0, cuts[mid - 1]) + "…", font) <= width) lo = mid;
        else hi = mid - 1;
    }
    return lo == 0 ? "…" : text.substr(0, cuts[lo - 1]) + "…";
}

/// Splits text into lines of at most `maxLines`, breaking at spaces where possible.
std::vector<std::string> wrapText(Painter& p, const std::string& text, const Font& font, float width, int maxLines) {
    std::vector<std::string> lines;
    if (width <= 0 || maxLines <= 0 || text.empty()) return lines;
    std::vector<std::size_t> cuts{0};
    for (std::size_t i = 1; i < text.size(); ++i) {
        if ((static_cast<unsigned char>(text[i]) & 0xC0) != 0x80) cuts.push_back(i);
    }
    cuts.push_back(text.size());
    std::size_t start = 0;
    while (start < text.size() && static_cast<int>(lines.size()) < maxLines) {
        std::size_t lo = 0;
        std::size_t hi = cuts.size() - 1;
        while (lo < hi) {
            const std::size_t mid = (lo + hi + 1) / 2;
            if (cuts[mid] <= start) {
                lo = mid;
                continue;
            }
            const std::string_view piece(text.data() + start, cuts[mid] - start);
            if (p.measureText(piece, font) <= width) lo = mid;
            else hi = mid - 1;
        }
        std::size_t best = std::max(start + 1, cuts[lo]);
        if (static_cast<int>(lines.size()) + 1 < maxLines || best < text.size()) {
            const std::size_t space = text.rfind(' ', best);
            // Only prefer a word break when it keeps most of the line: CJK text has few spaces and a
            // break at the first one would leave a one-word line.
            if (space != std::string::npos && space > start + (best - start) / 2) best = space;
        }
        std::string line = text.substr(start, best - start);
        while (!line.empty() && line.front() == ' ') line.erase(line.begin());
        if (static_cast<int>(lines.size()) + 1 == maxLines && best < text.size()) line = cutText(p, text.substr(start), font, width);
        lines.push_back(line);
        start = best;
        while (start < text.size() && text[start] == ' ') ++start;
    }
    return lines;
}

Color accentFor(const AppEntry& app) {
    if (auto c = Color::parse(app.accent)) return *c;
    const auto& c = theme().colors;
    const Color palette[] = {c.blue, c.indigo, c.purple, c.pink, c.red, c.orange, c.green, c.teal};
    return palette[std::hash<std::string>{}(app.id) % std::size(palette)];
}

std::string appSizeText(const AppEntry& app) {
    if (app.installedSize > 0) return formatBytes(app.installedSize);
    if (app.downloadSize > 0) return formatBytes(app.downloadSize);
    return {};
}

std::string stateLabel(InstallState state) {
    switch (state) {
        case InstallState::Get: return "获取";
        case InstallState::Busy: return "正在安装…";
        case InstallState::Open: return "打开";
        case InstallState::Update: return "更新";
        case InstallState::Broken: return "重试";
        case InstallState::Unavailable: return "不可用";
    }
    return {};
}

/// Placeholder icon: the app's initial on a tinted gradient squircle.
void drawPlaceholder(Painter& p, const Rect& r, const AppEntry& app, float radius) {
    const Color tint = accentFor(app);
    p.fillVerticalGradient(r, radius, tint.lighter(0.22f), tint.lighter(-0.10f));
    std::size_t end = 1;
    while (end < app.name.size() && (static_cast<unsigned char>(app.name[end]) & 0xC0) == 0x80) ++end;
    const std::string initial = app.name.empty() ? "?" : app.name.substr(0, end);
    Font font{static_cast<float>(std::max(12.0f, r.width * 0.40f)), FontWeight::Bold, r.height * 0.6f};
    p.drawText(initial, r, font, Color::white(0.92f), TextAlign::Center);
}

/// Preserve the source icon silhouette and aspect ratio; use a tinted fallback while loading.
void drawAppIcon(Painter& p, StoreModel& model, const AppEntry& app, const Rect& r, float radius) {
    if (auto img = model.images.icon(app.id, app.iconPath, app.iconUrl)) {
        const float scale = std::min(r.width / std::max(1, img->width()), r.height / std::max(1, img->height()));
        const float w = img->width() * scale, h = img->height() * scale;
        p.drawImage({r.center().x - w * 0.5f, r.center().y - h * 0.5f, w, h}, *img, 0);
    } else {
        drawPlaceholder(p, r, app, radius);
    }
}

void cardBackground(Painter& p, const Rect& r, float radius, bool hovered) {
    const Theme& t = theme();
    if (t.isDark()) {
        p.fillRoundedRect(r, radius, Color::white(hovered ? 0.11f : 0.065f));
        p.strokeRoundedRect(r, radius, Color::white(hovered ? 0.12f : 0.06f), 0.5f);
    } else {
        p.drawShadow(r, radius, Color::black(hovered ? 0.13f : 0.06f), hovered ? 16 : 8, {0, hovered ? 4.0f : 2.0f});
        p.fillRoundedRect(r, radius, hovered ? Color::white() : Color::hex(0xFAFAFC));
        p.strokeRoundedRect(r, radius, Color::black(0.05f), 0.5f);
    }
}

Rect chipsRect(const Rect& card) {
    const float w = 70;
    const float h = 26;
    return {card.x + 13, card.y + card.height - h - 12, w, h};
}

void drawActionChip(Painter& p, StoreModel& model, const AppEntry& app, const Rect& r, bool hovered) {
    const Theme& t = theme();
    const bool dark = t.isDark();
    const InstallState state = model.stateOf(app);
    Color fill;
    Color text;
    switch (state) {
        case InstallState::Get:
            fill = dark ? Color::white(0.92f) : Color::black(0.78f);
            text = dark ? Color::black(0.92f) : Color::white(0.96f);
            break;
        case InstallState::Open:
            fill = dark ? Color::white(0.16f) : Color::hex(0xE9E9EE);
            text = dark ? Color::white(0.92f) : Color::black(0.80f);
            break;
        case InstallState::Update:
            fill = t.colors.accent;
            text = t.colors.onAccent;
            break;
        case InstallState::Busy:
            fill = dark ? Color::white(0.12f) : Color::hex(0xE9E9EE);
            text = dark ? Color::white(0.60f) : Color::black(0.45f);
            break;
        case InstallState::Broken:
            fill = dark ? Color::white(0.16f) : Color::hex(0xE9E9EE);
            text = t.colors.destructive;
            break;
        case InstallState::Unavailable:
            fill = t.colors.fill;
            text = t.colors.tertiaryLabel;
            break;
    }
    if (hovered && (state == InstallState::Get || state == InstallState::Update))
        fill = fill.mix(dark ? Color::white() : Color::black(), 0.14f);
    p.fillRoundedRect(r, r.height * 0.5f, fill);
    if (state == InstallState::Busy) {
        const float progress = model.progressOf(app.id);
        p.pushClip(r, r.height * 0.5f);
        if (progress >= 0) {
            p.fillRoundedRect({r.x, r.y, r.width * progress, r.height}, r.height * 0.5f,
                              t.colors.accent.multipliedAlpha(0.55f));
        } else {
            const float phase = static_cast<float>(std::fmod(clockNow(), 1.3) / 1.3);
            const float x = r.x - r.width + phase * r.width * 2;
            p.fillRoundedRect({x, r.y, r.width, r.height}, r.height * 0.5f, t.colors.accent.multipliedAlpha(0.4f));
        }
        p.popClip();
        std::string stage = model.stage.count(app.id) ? model.stage.at(app.id) : stateLabel(state);
        // flatpak's "1/7" counter already says where it is; only append a percentage for real progress.
        const bool hasCounter = stage.find('/') != std::string::npos;
        if (progress >= 0 && stage.find('%') == std::string::npos && !hasCounter)
            stage += " " + std::to_string(static_cast<int>(progress * 100.0f + 0.5f)) + "%";
        p.pushClip({r.x + 4, r.y, r.width - 8, r.height});
        p.drawText(cutText(p, stage, smallFont(), r.width - 8), {r.x, r.y, r.width, r.height}, smallFont(), text,
                   TextAlign::Center);
        p.popClip();
        return;
    }
    if (state == InstallState::Open) {
        p.drawText(stateLabel(state), r, Font{11, FontWeight::Semibold, 15}, text, TextAlign::Center);
        return;
    }
    p.drawText(stateLabel(state), r, Font{11, FontWeight::Semibold, 15}, text, TextAlign::Center);
}

void activate(StoreModel& model, const AppEntry& app) {
    switch (model.stateOf(app)) {
        case InstallState::Get:
        case InstallState::Broken:
            if (model.callbacks.install) model.callbacks.install(app.id);
            break;
        case InstallState::Update:
            if (model.callbacks.update) model.callbacks.update(app.id);
            break;
        case InstallState::Open:
            if (model.callbacks.launch) model.callbacks.launch(app.id);
            break;
        default: break;
    }
}

// --- generic blocks ------------------------------------------------------------------------------

/// Vertical page body with page padding; children are measured with the full content width.
class PageView : public Widget {
public:
    PageView() = default;
    void add(const Element& e) { addChild(e.widget()); }
    [[nodiscard]] bool expandsContent(Axis axis) const override { return axis == Axis::Horizontal; }
    Size measureContent(Size available) override {
        const float inner = std::max(0.0f, available.width - kPagePad * 2);
        float height = 26;
        bool first = true;
        for (const auto& c : children()) {
            if (!c->isVisible()) continue;
            const Size s = c->measure({inner, kInfinity});
            if (!first) height += kBlock;
            height += s.height;
            first = false;
        }
        return {available.width, height + 48};
    }
    void arrangeContent(const Rect& content) override {
        const float x = content.x + kPagePad;
        const float w = std::max(0.0f, content.width - kPagePad * 2);
        float y = content.y + 26;
        bool first = true;
        for (const auto& c : children()) {
            if (!c->isVisible()) continue;
            if (!first) y += kBlock;
            first = false;
            const Size s = c->measure({w, kInfinity});
            c->measure({w, kInfinity});
            c->arrange({x, y, w, s.height});
            y += s.height;
        }
    }
};

class SpaceView : public Widget {
public:
    explicit SpaceView(float height, bool rule = false) : m_height(height), m_rule(rule) {}
    Size measureContent(Size available) override { return {available.width, m_height}; }
    [[nodiscard]] bool expandsContent(Axis axis) const override { return axis == Axis::Horizontal; }
    void draw(Painter& p) override {
        if (!m_rule) return;
        const Rect r = frame();
        p.fillRect({r.x, p.snap(r.center().y), r.width, 1.0f / p.scale()}, theme().colors.separator);
    }

private:
    float m_height;
    bool m_rule;
};

/// Large page heading (title + optional subtitle and trailing action).
class HeadingView : public Widget {
public:
    HeadingView(std::string title, std::string subtitle, std::string action = {}, std::function<void()> onAction = {})
        : m_title(std::move(title)), m_subtitle(std::move(subtitle)), m_action(std::move(action)),
          m_onAction(std::move(onAction)) {}

    void setTitle(std::string title) {
        m_title = std::move(title);
        invalidateLayout();
    }
    Size measureContent(Size available) override { return {available.width, m_subtitle.empty() ? 40.0f : 62.0f}; }
    [[nodiscard]] bool expandsContent(Axis axis) const override { return axis == Axis::Horizontal; }
    bool onPointerDown(const PointerEvent& e) override {
        if (m_onAction && actionRect().contains(e.position)) return true;
        return false;
    }
    void onPointerUp(const PointerEvent& e) override {
        if (m_onAction && actionRect().contains(e.position)) m_onAction();
    }
    [[nodiscard]] bool isInteractive() const override { return static_cast<bool>(m_onAction); }
    void draw(Painter& p) override {
        const Theme& t = theme();
        const Rect r = frame();
        p.drawText(m_title, {r.x, r.y, r.width - 140, 38}, titleFont(), t.colors.label);
        if (!m_subtitle.empty())
            p.drawText(m_subtitle, {r.x, r.y + 36, r.width - 140, 18}, t.font(TextStyle::Body),
                       t.colors.secondaryLabel);
        if (!m_action.empty()) {
            const Rect a = actionRect();
            if (isHovered()) p.fillRoundedRect(a.inset(-10, -4), 8, t.colors.hover);
            p.drawText(m_action, a, t.font(TextStyle::Body).withWeight(FontWeight::Medium), t.colors.accent,
                       TextAlign::Trailing);
        }
    }
    void onPointerMove(const PointerEvent& e) override {
        if (host()) host()->setCursor(m_onAction && actionRect().contains(e.position) ? CursorShape::PointingHand
                                                                                       : CursorShape::Arrow);
    }

private:
    [[nodiscard]] Rect actionRect() const {
        const Rect r = frame();
        return {r.right() - 120, r.y + 6, 120, 26};
    }
    std::string m_title, m_subtitle, m_action;
    std::function<void()> m_onAction;
};

/// Section heading ("本周精选", "为你推荐").
class SectionHeadingView : public Widget {
public:
    SectionHeadingView(std::string title, std::string action = {}, std::function<void()> onAction = {})
        : m_title(std::move(title)), m_action(std::move(action)), m_onAction(std::move(onAction)) {}

    Size measureContent(Size available) override { return {available.width, 28}; }
    [[nodiscard]] bool expandsContent(Axis axis) const override { return axis == Axis::Horizontal; }
    void draw(Painter& p) override {
        const Theme& t = theme();
        const Rect r = frame();
        p.drawText(m_title, {r.x, r.y, r.width - 120, 28}, headingFont(), t.colors.label);
        if (!m_action.empty()) {
            const Rect a{r.right() - 120, r.y, 120, 26};
            p.drawText(m_action, a, t.font(TextStyle::Body), t.colors.accent, TextAlign::Trailing);
        }
    }
    bool onPointerDown(const PointerEvent& e) override {
        return m_onAction && Rect{frame().right() - 120, frame().y, 120, 26}.contains(e.position);
    }
    void onPointerUp(const PointerEvent& e) override {
        if (m_onAction && Rect{frame().right() - 120, frame().y, 120, 26}.contains(e.position)) m_onAction();
    }
    [[nodiscard]] bool isInteractive() const override { return static_cast<bool>(m_onAction); }

private:
    std::string m_title, m_action;
    std::function<void()> m_onAction;
};

// --- app cards -----------------------------------------------------------------------------------

class AppCardView : public Widget {
public:
    AppCardView(StoreModel& model, std::string id, float width, bool chip = true)
        : m_model(model), m_id(std::move(id)), m_width(width), m_chip(chip) {
        const auto* app = model.find(m_id);
        m_title = Label(app ? app->name : "").verbatim().font(cardTitleFont()).lineLimit(1).node();
        m_subtitle = Label(app ? (app->summary.empty() ? categoryName(app->primaryCategory()) : app->summary) : "")
                         .verbatim().font(cardSubFont()).secondary().lineLimit(1).node();
        addChild(m_title);
        addChild(m_subtitle);
        m_action = std::make_shared<ButtonView>("");
        m_action->setControlSize(ControlSize::Small);
        m_action->setOnClick([this] {
            if (const auto* entry = m_model.find(m_id)) activate(m_model, *entry);
        });
        if (m_chip) addChild(m_action);
    }

    Size measureContent(Size /*available*/) override { return {m_width, kCardHeight}; }
    [[nodiscard]] bool isInteractive() const override { return true; }
    [[nodiscard]] Rect iconRect() const {
        const Rect r = frame();
        return {r.x + 14, r.y + 14, 64, 64};
    }
    void setCardWidth(float width) { m_width = width; }
    void arrangeContent(const Rect& r) override {
        const float w = std::max(0.0f, r.width - 28);
        m_title->measure({w, 20});
        m_title->arrange({r.x + 14, r.y + 90, w, 20});
        m_subtitle->measure({w, 18});
        m_subtitle->arrange({r.x + 14, r.y + 113, w, 18});
        if (m_chip) {
            m_action->measure({w, 28});
            m_action->arrange({r.x + 14, r.bottom() - 40, std::min(100.0f, w), 28});
        }
    }
    void draw(Painter& p) override {
        const AppEntry* app = m_model.find(m_id);
        if (!app) return;
        const float textWidth = std::max(0.0f, frame().width - 28);
        const auto title = cutText(p, app->name, cardTitleFont(), textWidth);
        const auto subtitle = cutText(p, app->summary.empty() ? categoryName(app->primaryCategory()) : app->summary,
                                      cardSubFont(), textWidth);
        if (m_title->text() != title) m_title->setText(title);
        if (m_subtitle->text() != subtitle) m_subtitle->setText(subtitle);
        cardBackground(p, frame(), 15, isHovered());
        drawAppIcon(p, m_model, *app, iconRect(), 13);
        if (m_chip) {
            const auto state = m_model.stateOf(*app);
            std::string actionTitle = stateLabel(state);
            if (state == InstallState::Busy) {
                const float progress = m_model.progressOf(m_id);
                if (progress >= 0) actionTitle = std::to_string(static_cast<int>(progress * 100)) + "%";
            }
            if (m_action->title() != actionTitle) m_action->setTitle(actionTitle);
            if (m_actionState != state) {
                m_action->setStyle(state == InstallState::Update ? ButtonStyle::Prominent : ButtonStyle::Default);
                m_actionState = state;
            }
            m_action->setDisabled(state == InstallState::Busy || state == InstallState::Unavailable);
        }
        paintChildren(p);
    }

    bool onPointerDown(const PointerEvent& e) override { return e.button == MouseButton::Left; }
    void onPointerUp(const PointerEvent& e) override {
        if (e.button == MouseButton::Left && frame().contains(e.position) && m_model.callbacks.openApp)
            m_model.callbacks.openApp(m_id);
    }
    void onPointerMove(const PointerEvent& e) override {
        if (!host()) return;
        host()->setCursor(frame().contains(e.position) ? CursorShape::PointingHand : CursorShape::Arrow);
    }
    bool onKeyDown(const KeyEvent& e) override {
        if ((e.key == Key::Enter || e.key == Key::Space) && m_model.callbacks.openApp) {
            m_model.callbacks.openApp(m_id);
            return true;
        }
        return false;
    }
    [[nodiscard]] bool acceptsFocus() const override { return true; }

private:
    StoreModel& m_model;
    std::string m_id;
    float m_width;
    bool m_chip;
    std::shared_ptr<LabelView> m_title, m_subtitle;
    std::shared_ptr<ButtonView> m_action;
    InstallState m_actionState = InstallState::Get;
};

/// A horizontally scrollable shelf of icon cards (touchpads swipe it sideways; a vertical wheel
/// stays with the page). Cards that don't fit peek in from the edge, like the App Store's shelves.
class CardRowView : public Widget {
public:
    CardRowView(StoreModel& model, std::vector<std::string> ids, float cardWidth, bool chip = true)
        : m_model(model), m_width(cardWidth) {
        m_physics.setAxes(true, false);
        m_physics.setBouncesWhenFitting(true, false);
        for (const auto& id : ids) {
            if (!model.find(id)) continue;
            m_cards.push_back(std::make_shared<AppCardView>(model, id, cardWidth, chip));
            addChild(m_cards.back());
        }
    }
    Size measureContent(Size available) override { return {available.width, kCardHeight}; }
    [[nodiscard]] bool expandsContent(Axis axis) const override { return axis == Axis::Horizontal; }
    void arrangeContent(const Rect& content) override {
        for (std::size_t i = 0; i < m_cards.size(); ++i) {
            m_cards[i]->measure({m_width, kCardHeight});
            m_cards[i]->arrange({content.x - m_physics.offset().x + static_cast<float>(i) * (m_width + kGap),
                                 content.y, m_width, kCardHeight});
        }
        const float total = static_cast<float>(m_cards.size()) * (m_width + kGap) - kGap;
        m_physics.setLimits({std::max(0.0f, total - content.width), 0}, content.size());
    }
    void draw(Painter& p) override {
        step();
        p.pushClip(contentRect());
        paintChildren(p);
        p.popClip();
    }
    bool onScroll(const ScrollEvent& e) override {
        // Horizontal deltas swipe the shelf; Shift+wheel is turned into horizontal by the window.
        if (e.delta.x == 0) return false;
        const double t = now();
        if (!m_physics.handle(e, t)) return true;
        if (auto* h = host(); h && m_physics.deadline() >= 0) h->scheduleFrame(m_physics.deadline());
        if (m_physics.isAnimating()) requestAnimationFrame();
        if (parent()) parent()->invalidateLayout();
        return true;
    }
    [[nodiscard]] bool isInteractive() const override { return true; }

private:
    void step() {
        if (!m_physics.isAnimating()) return;
        const bool moving = m_physics.step(now());
        if (parent()) parent()->invalidateLayout();
        if (moving) requestAnimationFrame();
        else if (auto* h = host(); h && m_physics.deadline() >= 0) h->scheduleFrame(m_physics.deadline());
    }

    StoreModel& m_model;
    std::vector<std::shared_ptr<AppCardView>> m_cards;
    float m_width;
    ScrollPhysics m_physics;
};

/// Responsive grid of icon cards.
class AppGridView : public Widget {
public:
    AppGridView(StoreModel& model, std::vector<std::string> ids, float minCard = kGridCard)
        : m_model(model), m_minCard(minCard) {
        for (const auto& id : ids) {
            if (!model.find(id)) continue;
            m_cards.push_back(std::make_shared<AppCardView>(model, id, minCard));
            addChild(m_cards.back());
        }
    }
    Size measureContent(Size available) override {
        const int cols = columns(available.width);
        const auto rows = (m_cards.size() + static_cast<std::size_t>(cols) - 1) / static_cast<std::size_t>(cols);
        return {available.width, static_cast<float>(rows) * (kCardHeight + kGap) - (rows ? kGap : 0.0f)};
    }
    [[nodiscard]] bool expandsContent(Axis axis) const override { return axis == Axis::Horizontal; }
    void arrangeContent(const Rect& content) override {
        const int cols = columns(content.width);
        const float w = cardWidth(content.width, cols);
        for (std::size_t i = 0; i < m_cards.size(); ++i) {
            const int col = static_cast<int>(i) % cols;
            const int row = static_cast<int>(i) / cols;
            m_cards[i]->setCardWidth(w);
            m_cards[i]->measure({w, kCardHeight});
            m_cards[i]->arrange({content.x + static_cast<float>(col) * (w + kGap),
                                 content.y + static_cast<float>(row) * (kCardHeight + kGap), w, kCardHeight});
        }
    }
    void draw(Painter& p) override {
        p.pushClip(contentRect());
        paintChildren(p);
        p.popClip();
    }

private:
    [[nodiscard]] int columns(float width) const {
        return std::max(1, static_cast<int>((width + kGap) / (m_minCard + kGap)));
    }
    [[nodiscard]] float cardWidth(float width, int cols) const {
        return std::floor((width - kGap * static_cast<float>(cols - 1)) / static_cast<float>(cols));
    }

    StoreModel& m_model;
    float m_minCard;
    std::vector<std::shared_ptr<AppCardView>> m_cards;
};

// --- hero ----------------------------------------------------------------------------------------

class HeroView : public Widget {
public:
    explicit HeroView(StoreModel& model) : m_model(model) {}
    Size measureContent(Size available) override { return {available.width, kHeroHeight}; }
    [[nodiscard]] bool expandsContent(Axis axis) const override { return axis == Axis::Horizontal; }

    void draw(Painter& p) override {
        const Rect r = frame();
        const auto& ids = heroOrder();
        if (ids.empty()) return;
        const int index = ((m_model.heroIndex % static_cast<int>(ids.size())) + static_cast<int>(ids.size())) %
                          static_cast<int>(ids.size());
        const AppEntry* app = m_model.find(ids[static_cast<std::size_t>(index)]);
        if (!app) return;
        if (app->screenshots.empty() && app->summary.empty()) return;
        const float radius = 22;
        bool drawn = false;
        if (!app->screenshots.empty()) {
            const Screenshot& shot = app->screenshots.front();
            if (auto img = m_model.images.screenshot(shot.width >= 1200 ? shot.source : shot.thumbnail, 1800)) {
                const float ia = static_cast<float>(img->width()) / static_cast<float>(std::max(1, img->height()));
                const float ra = r.width / std::max(1.0f, r.height);
                Rect src{0, 0, 1, 1};
                if (ia > ra) {
                    src.width = ra / ia;
                    src.x = (1 - src.width) * 0.5f;
                } else {
                    src.height = ia / ra;
                    src.y = (1 - src.height) * 0.5f;
                }
                p.drawImage(r, *img, radius, 1, src);
                drawn = true;
            }
        }
        if (!drawn) {
            const Color tint = accentFor(*app);
            p.fillVerticalGradient(r, radius, tint.lighter(0.2f), tint.lighter(-0.45f));
        }
        p.pushClip(r, radius);
        // Scrims keep white text legible over any screenshot: a left-to-right shade for the title and
        // a bottom shade under the title block.
        for (int i = 0; i < 26; ++i) {
            const float t = static_cast<float>(i) / 25.0f;
            const float w = r.width * (0.80f * (1 - t) + 0.05f);
            p.fillRect({r.x, r.y, w, r.height}, Color::black(0.034f));
        }
        for (int i = 0; i < 30; ++i) {
            const float t = static_cast<float>(i) / 29.0f;
            const float h = 250.0f * (1 - t) + 6;
            p.fillRect({r.x, r.bottom() - h, r.width, h}, Color::black(0.041f));
        }
        p.popClip();
        p.strokeRoundedRect(r, radius, Color::white(0.16f), 0.5f);

        const float x = r.x + 34;
        const float textW = std::min(r.width * 0.62f, 620.0f);
        // Label, name and summary stack above the action row, like the App Store's editorial banner.
        const float chipY = r.bottom() - 60;
        p.drawText("今日应用", {x, chipY - 96, 200, 18}, Font{11, FontWeight::Semibold, 14}, Color::white(0.80f));
        float y = chipY - 78;
        for (const auto& line : wrapText(p, app->name, titleFont(), textW, 2)) {
            p.drawText(line, {x, y, textW, 36}, titleFont(), Color::white());
            y += 34;
        }
        if (!app->summary.empty())
            p.drawText(cutText(p, app->summary, cardTitleFont(), textW), {x, y + 2, textW, 20}, cardTitleFont(),
                       Color::white(0.86f));

        const Rect chip{x, chipY, 94, 32};
        p.drawShadow(chip, 16, Color::black(0.18f), 10, {0, 2});
        p.drawMaterial(chip, 16, Material::Glass);
        if (m_hoverChip) p.fillRoundedRect(chip, 16, Color::white(0.18f));
        p.drawText("查看", chip, Font{13, FontWeight::Medium, 18}, Color::white(), TextAlign::Center);

        const float dotY = r.bottom() - 34;
        float dotX = r.center().x - static_cast<float>(ids.size()) * 7.0f;
        for (std::size_t i = 0; i < ids.size(); ++i) {
            const bool active = static_cast<int>(i) == index;
            p.fillCircle({dotX + static_cast<float>(i) * 14.0f, dotY}, active ? 3.6f : 3.0f,
                         Color::white(active ? 0.95f : 0.38f));
        }
        if (m_hoverPrev) {
            p.fillCircle({r.x + 20, r.center().y}, 17, Color::black(0.30f));
            p.drawIcon(Icon::ChevronLeft, {r.x + 11, r.center().y - 9, 18, 18}, Color::white());
        }
        if (m_hoverNext) {
            p.fillCircle({r.right() - 20, r.center().y}, 17, Color::black(0.30f));
            p.drawIcon(Icon::ChevronRight, {r.right() - 29, r.center().y - 9, 18, 18}, Color::white());
        }
    }

    void onPointerMove(const PointerEvent& e) override {
        const Rect r = frame();
        const bool prev = e.position.x < r.x + 44;
        const bool next = e.position.x > r.right() - 44;
        const bool chip = e.position.x < r.x + 150 && e.position.y > r.bottom() - 74;
        if (prev != m_hoverPrev || next != m_hoverNext || chip != m_hoverChip) {
            m_hoverPrev = prev;
            m_hoverNext = next;
            m_hoverChip = chip;
            invalidate();
        }
        if (host()) host()->setCursor(CursorShape::PointingHand);
    }
    void onPointerLeave() override {
        m_hoverPrev = m_hoverNext = m_hoverChip = false;
        invalidate();
    }
    bool onPointerDown(const PointerEvent& e) override { return e.button == MouseButton::Left; }
    void onPointerUp(const PointerEvent& e) override {
        const Rect r = frame();
        const auto& ids = heroOrder();
        if (ids.empty()) return;
        if (e.position.x < r.x + 44) {
            m_model.heroIndex = (m_model.heroIndex + static_cast<int>(ids.size()) - 1) % static_cast<int>(ids.size());
            invalidate();
            return;
        }
        if (e.position.x > r.right() - 44) {
            m_model.heroIndex = (m_model.heroIndex + 1) % static_cast<int>(ids.size());
            invalidate();
            return;
        }
        if (r.contains(e.position)) {
            const int index = ((m_model.heroIndex % static_cast<int>(ids.size())) + static_cast<int>(ids.size())) %
                              static_cast<int>(ids.size());
            if (m_model.callbacks.openApp) m_model.callbacks.openApp(ids[static_cast<std::size_t>(index)]);
        }
    }
    [[nodiscard]] bool isInteractive() const override { return true; }

private:
    StoreModel& m_model;
    bool m_hoverPrev = false, m_hoverNext = false, m_hoverChip = false;
};

// --- featured editorial card ---------------------------------------------------------------------

class FeaturedCardView : public Widget {
public:
    FeaturedCardView(StoreModel& model, std::string id) : m_model(model), m_id(std::move(id)) {}
    Size measureContent(Size available) override { return {available.width, 208}; }
    [[nodiscard]] bool expandsContent(Axis axis) const override { return axis == Axis::Horizontal; }

    void draw(Painter& p) override {
        const AppEntry* app = m_model.find(m_id);
        if (!app) return;
        const Rect r = frame();
        const bool hovered = isHovered();
        cardBackground(p, r, 18, hovered);
        const Rect icon{r.x + 20, r.y + 20, 58, 58};
        drawAppIcon(p, m_model, *app, icon, 14);
        const float textW = std::max(80.0f, r.width * 0.42f - 24);
        p.drawText(cutText(p, app->name, cardTitleFont(), textW), {r.x + 20, r.y + 88, textW, 20}, cardTitleFont(),
                   theme().colors.label);
        if (!app->summary.empty()) {
            float y = r.y + 112;
            for (const auto& line : wrapText(p, app->summary, cardSubFont(), textW, 2)) {
                p.drawText(cutText(p, line, cardSubFont(), textW), {r.x + 20, y, textW, 16}, cardSubFont(),
                           theme().colors.secondaryLabel);
                y += 16;
            }
        }
        // Screenshot preview.
        const float shotW = std::min(300.0f, r.width * 0.44f);
        const Rect shot{r.right() - shotW - 18, r.y + 18, shotW, r.height - 36};
        p.fillRoundedRect(shot, 11, theme().colors.fill);
        if (!app->screenshots.empty()) {
            if (auto img = m_model.images.screenshot(app->screenshots.front().thumbnail, 900)) {
                const float ia = static_cast<float>(img->width()) / std::max(1, img->height());
                const float ra = shot.width / std::max(1.0f, shot.height);
                Rect src{0, 0, 1, 1};
                if (ia > ra) {
                    src.width = ra / ia;
                    src.x = (1 - src.width) * 0.5f;
                } else {
                    src.height = ia / ra;
                    src.y = (1 - src.height) * 0.5f;
                }
                p.drawImage(shot, *img, 11, 1, src);
                p.strokeRoundedRect(shot, 11, Color::black(0.08f), 0.5f);
            }
        } else {
            p.drawIcon(Icon::Photo, {shot.center().x - 18, shot.center().y - 18, 36, 36},
                       theme().colors.tertiaryLabel, 0.8f);
        }
        drawActionChip(p, m_model, *app, chipsRect(r), hovered);
    }

    bool onPointerDown(const PointerEvent& e) override {
        if (e.button != MouseButton::Left) return false;
        const AppEntry* app = m_model.find(m_id);
        if (app && chipsRect(frame()).contains(e.position)) {
            m_chipPressed = true;
            return true;
        }
        return true;
    }
    void onPointerUp(const PointerEvent& e) override {
        const AppEntry* app = m_model.find(m_id);
        if (!app) return;
        if (m_chipPressed) {
            m_chipPressed = false;
            if (chipsRect(frame()).contains(e.position)) activate(m_model, *app);
            return;
        }
        if (frame().contains(e.position) && m_model.callbacks.openApp) m_model.callbacks.openApp(m_id);
    }
    void onPointerMove(const PointerEvent& e) override {
        if (host()) host()->setCursor(frame().contains(e.position) ? CursorShape::PointingHand : CursorShape::Arrow);
    }
    [[nodiscard]] bool isInteractive() const override { return true; }

private:
    StoreModel& m_model;
    std::string m_id;
    bool m_chipPressed = false;
};

/// Two featured cards per row.
class FeaturedRowView : public Widget {
public:
    FeaturedRowView(StoreModel& model, std::vector<std::string> ids) : m_model(model) {
        for (const auto& id : ids) {
            if (!model.find(id)) continue;
            m_cards.push_back(std::make_shared<FeaturedCardView>(model, id));
            addChild(m_cards.back());
        }
    }
    Size measureContent(Size available) override {
        const int cols = available.width >= 700 ? 2 : 1;
        const int rows = static_cast<int>((m_cards.size() + cols - 1) / cols);
        return {available.width, static_cast<float>(rows) * (208 + kGap) - (rows ? kGap : 0)};
    }
    [[nodiscard]] bool expandsContent(Axis axis) const override { return axis == Axis::Horizontal; }
    void arrangeContent(const Rect& content) override {
        const int cols = content.width >= 700 && m_cards.size() > 1 ? 2 : 1;
        const float w = (content.width - kGap * (cols - 1)) / cols;
        for (std::size_t i = 0; i < m_cards.size(); ++i) {
            const int col = static_cast<int>(i) % cols;
            const int row = static_cast<int>(i) / cols;
            m_cards[i]->measure({w, 208});
            m_cards[i]->arrange({content.x + static_cast<float>(col) * (w + kGap),
                                 content.y + static_cast<float>(row) * (208 + kGap), w, 208});
        }
    }
    void draw(Painter& p) override {
        p.pushClip(contentRect());
        paintChildren(p);
        p.popClip();
    }

private:
    StoreModel& m_model;
    std::vector<std::shared_ptr<FeaturedCardView>> m_cards;
};

// --- category tiles ------------------------------------------------------------------------------

class CategoryTileView : public Widget {
public:
    CategoryTileView(StoreModel& model, std::string id, std::string title, std::string count)
        : m_model(model), m_id(std::move(id)), m_title(std::move(title)), m_count(std::move(count)) {}
    Size measureContent(Size available) override { return {available.width, 96}; }
    [[nodiscard]] bool expandsContent(Axis axis) const override { return axis == Axis::Horizontal; }

    void draw(Painter& p) override {
        const Rect r = frame();
        const bool hovered = isHovered();
        if (theme().isDark()) p.fillRoundedRect(r, 16, Color::white(hovered ? 0.10f : 0.06f));
        else {
            p.drawShadow(r, 16, Color::black(hovered ? 0.10f : 0.05f), 10, {0, 2});
            p.fillRoundedRect(r, 16, hovered ? Color::white() : Color::hex(0xFAFAFC));
        }
        const Color tints[] = {theme().colors.blue, theme().colors.purple, theme().colors.green,
                               theme().colors.orange, theme().colors.pink, theme().colors.teal,
                               theme().colors.indigo, theme().colors.yellow};
        const Color tint = tints[std::hash<std::string>{}(m_id) % std::size(tints)];
        const Rect tile{r.x + 16, r.center().y - 24, 48, 48};
        p.fillVerticalGradient(tile, 12, tint.lighter(0.20f), tint);
        p.drawIcon(iconFor(m_id), tile.inset(13), Color::white(0.95f), 1.05f);
        p.drawText(m_title, {tile.right() + 14, r.center().y - 17, r.width - 100, 20}, cardTitleFont(),
                   theme().colors.label);
        p.drawText(m_count, {tile.right() + 14, r.center().y + 4, r.width - 100, 16}, cardSubFont(),
                   theme().colors.secondaryLabel);
        p.drawIcon(Icon::ChevronRight, {r.right() - 24, r.center().y - 7, 14, 14}, theme().colors.tertiaryLabel);
    }
    bool onPointerDown(const PointerEvent& e) override { return e.button == MouseButton::Left; }
    void onPointerUp(const PointerEvent& e) override {
        if (frame().contains(e.position) && m_model.callbacks.openCategory) m_model.callbacks.openCategory(m_id);
    }
    [[nodiscard]] bool isInteractive() const override { return true; }
    void onPointerMove(const PointerEvent& e) override {
        if (host()) host()->setCursor(frame().contains(e.position) ? CursorShape::PointingHand : CursorShape::Arrow);
    }

private:
    static Icon iconFor(const std::string& id) {
        static const std::map<std::string, Icon, std::less<>> icons{
            {"Game", Icon::Bolt},          {"AudioVideo", Icon::Play},     {"Development", Icon::Terminal},
            {"Graphics", Icon::Paintbrush}, {"Office", Icon::Document},    {"Utility", Icon::Gear},
            {"Education", Icon::Library},  {"Network", Icon::Globe},       {"System", Icon::Display},
            {"Photography", Icon::Photo},  {"Music", Icon::MusicNote},     {"Video", Icon::Film},
            {"Audio", Icon::Speaker},      {"Science", Icon::Sparkles},    {"Finance", Icon::Tag},
        };
        if (const auto it = icons.find(id); it != icons.end()) return it->second;
        return Icon::Grid;
    }
    StoreModel& m_model;
    std::string m_id, m_title, m_count;
};

class CategoryGridView : public Widget {
public:
    CategoryGridView(StoreModel& model, std::vector<std::string> categories) : m_model(model) {
        for (const auto& cat : categories) {
            const auto apps = model.catalog.inCategory(cat, 100000);
            if (apps.empty()) continue;
            m_tiles.push_back(std::make_shared<CategoryTileView>(model, cat, categoryName(cat),
                                                                 std::to_string(apps.size()) + " 个应用"));
            addChild(m_tiles.back());
        }
    }
    Size measureContent(Size available) override {
        const int rows = static_cast<int>((m_tiles.size() + 1) / 2);
        return {available.width, static_cast<float>(rows) * (96 + kGap)};
    }
    [[nodiscard]] bool expandsContent(Axis axis) const override { return axis == Axis::Horizontal; }
    void arrangeContent(const Rect& content) override {
        const float w = (content.width - kGap) * 0.5f;
        for (std::size_t i = 0; i < m_tiles.size(); ++i) {
            const int col = static_cast<int>(i) % 2;
            const int row = static_cast<int>(i) / 2;
            m_tiles[i]->measure({w, 96});
            m_tiles[i]->arrange({content.x + static_cast<float>(col) * (w + kGap),
                                 content.y + static_cast<float>(row) * (96 + kGap), w, 96});
        }
    }
    void draw(Painter& p) override {
        p.pushClip(contentRect());
        paintChildren(p);
        p.popClip();
    }

private:
    StoreModel& m_model;
    std::vector<std::shared_ptr<CategoryTileView>> m_tiles;
};

// --- rows (updates / installed) ------------------------------------------------------------------

class AppRowView : public Widget {
public:
    AppRowView(StoreModel& model, std::string id, bool updateRow) : m_model(model), m_id(std::move(id)), m_update(updateRow) {}
    Size measureContent(Size available) override { return {available.width, 72}; }
    [[nodiscard]] bool expandsContent(Axis axis) const override { return axis == Axis::Horizontal; }

    void draw(Painter& p) override {
        const AppEntry* app = m_model.find(m_id);
        if (!app) return;
        const Rect r = frame();
        const Theme& t = theme();
        if (isHovered()) p.fillRoundedRect(r, 12, t.isDark() ? Color::white(0.06f) : Color::hex(0xF5F5F8));
        const Rect icon{r.x + 12, r.center().y - 22, 44, 44};
        drawAppIcon(p, m_model, *app, icon, 11);
        const float tx = icon.right() + 14;
        const float tw = r.width - (tx - r.x) - 130;
        p.drawText(cutText(p, app->name, cardTitleFont(), tw), {tx, r.y + 12, tw, 20}, cardTitleFont(), t.colors.label);
        std::string sub = app->summary;
        if (sub.empty() && app->installed)
            sub = app->installation == "user" ? "用户安装" : "系统安装";
        if (m_update && sub == (app->installation == "user" ? "用户安装" : "系统安装")) sub = "可以更新";
        if (!m_update && app->installed && !app->installedVersion.empty())
            sub = "版本 " + app->installedVersion + (app->updateAvailable ? " · 可更新至 " + app->version : "");
        p.drawText(cutText(p, sub, cardSubFont(), tw), {tx, r.y + 34, tw, 16}, cardSubFont(), t.colors.secondaryLabel);
        if (m_update) {
            std::string versions = app->installedVersion.empty() ? "" : app->installedVersion;
            if (!app->version.empty()) versions += (versions.empty() ? "" : " → ") + app->version;
            if (!versions.empty())
                p.drawText(cutText(p, versions, smallFont(), tw), {tx, r.y + 52, tw, 14}, smallFont(),
                           t.colors.tertiaryLabel);
        }
        const Rect chip{r.right() - 104, r.center().y - 13, 92, 26};
        if (m_update) {
            const bool busy = m_model.busy.count(app->id) > 0;
            p.fillRoundedRect(chip, 13, busy ? t.colors.fill : t.colors.accent);
            p.drawText(busy ? "正在更新…" : "更新", chip, Font{11, FontWeight::Semibold, 15},
                       busy ? t.colors.secondaryLabel : t.colors.onAccent, TextAlign::Center);
        } else {
            drawActionChip(p, m_model, *app, chip, isHovered());
        }
    }

    bool onPointerDown(const PointerEvent& e) override { return e.button == MouseButton::Left; }
    void onPointerUp(const PointerEvent& e) override {
        const AppEntry* app = m_model.find(m_id);
        if (!app) return;
        const Rect chip{frame().right() - 104, frame().center().y - 13, 92, 26};
        if (chip.contains(e.position)) {
            if (m_update) {
                if (m_model.callbacks.update) m_model.callbacks.update(m_id);
            } else {
                activate(m_model, *app);
            }
            return;
        }
        if (frame().contains(e.position) && m_model.callbacks.openApp) m_model.callbacks.openApp(m_id);
    }
    void onPointerMove(const PointerEvent& e) override {
        if (host()) host()->setCursor(frame().contains(e.position) ? CursorShape::PointingHand : CursorShape::Arrow);
    }
    [[nodiscard]] bool isInteractive() const override { return true; }

private:
    StoreModel& m_model;
    std::string m_id;
    bool m_update;
};

/// Vertical list of app rows with hairline separators.
class AppListView : public Widget {
public:
    AppListView(StoreModel& model, std::vector<std::string> ids, bool updateRow) : m_model(model) {
        for (const auto& id : ids) {
            if (!model.find(id)) continue;
            m_rows.push_back(std::make_shared<AppRowView>(model, id, updateRow));
            addChild(m_rows.back());
        }
        m_separators = !updateRow;
    }
    Size measureContent(Size available) override {
        return {available.width, static_cast<float>(m_rows.size()) * 72.0f};
    }
    [[nodiscard]] bool expandsContent(Axis axis) const override { return axis == Axis::Horizontal; }
    void arrangeContent(const Rect& content) override {
        for (std::size_t i = 0; i < m_rows.size(); ++i) {
            m_rows[i]->measure({content.width, 72});
            m_rows[i]->arrange({content.x, content.y + static_cast<float>(i) * 72.0f, content.width, 72});
        }
    }
    void draw(Painter& p) override {
        paintChildren(p);
        if (!m_separators) return;
        const Rect r = frame();
        const float px = 1.0f / p.scale();
        for (std::size_t i = 0; i + 1 < m_rows.size(); ++i) {
            const float y = m_rows[i]->frame().bottom();
            p.fillRect({r.x + 70, p.snap(y), r.width - 70, px}, theme().colors.separator.multipliedAlpha(0.7f));
        }
    }

private:
    StoreModel& m_model;
    std::vector<std::shared_ptr<AppRowView>> m_rows;
    bool m_separators = true;
};

// --- empty state ---------------------------------------------------------------------------------

class EmptyView : public Widget {
public:
    EmptyView(Icon icon, std::string title, std::string message) : m_icon(icon), m_title(std::move(title)), m_message(std::move(message)) {}
    Size measureContent(Size available) override { return {available.width, 220}; }
    [[nodiscard]] bool expandsContent(Axis axis) const override { return axis == Axis::Horizontal; }
    void draw(Painter& p) override {
        const Rect r = frame();
        const Theme& t = theme();
        p.drawIcon(m_icon, {r.center().x - 26, r.y + 26, 52, 52}, t.colors.tertiaryLabel, 1.0f);
        const Font title = t.font(TextStyle::Title2);
        const float tw = p.measureText(m_title, title);
        p.drawText(m_title, {r.center().x - tw * 0.5f, r.y + 100, tw, 24}, title, t.colors.label);
        const auto lines = wrapText(p, m_message, t.font(TextStyle::Body), std::min(520.0f, r.width), 2);
        float y = r.y + 130;
        for (const auto& line : lines) {
            const float w = p.measureText(line, t.font(TextStyle::Body));
            p.drawText(line, {r.center().x - w * 0.5f, y, w, 18}, t.font(TextStyle::Body), t.colors.secondaryLabel);
            y += 19;
        }
    }

private:
    Icon m_icon;
    std::string m_title, m_message;
};

// --- app detail pieces ---------------------------------------------------------------------------

class AppHeaderView : public Widget {
public:
    AppHeaderView(StoreModel& model, std::string id) : m_model(model), m_id(std::move(id)) {}
    Size measureContent(Size available) override { return {available.width, 132}; }
    [[nodiscard]] bool expandsContent(Axis axis) const override { return axis == Axis::Horizontal; }

    void draw(Painter& p) override {
        const AppEntry* app = m_model.find(m_id);
        if (!app) return;
        const Rect r = frame();
        const Theme& t = theme();
        const Rect icon{r.x, r.y + 2, 104, 104};
        drawAppIcon(p, m_model, *app, icon, 24);
        const float x = icon.right() + 22;
        const float textW = std::max(80.0f, r.width - (x - r.x) - 130);
        p.drawText(cutText(p, app->name, titleFont(), textW), {x, r.y - 4, textW, 38}, titleFont(), t.colors.label);
        if (!app->summary.empty())
            p.drawText(cutText(p, app->summary, cardTitleFont(), textW), {x, r.y + 38, textW, 20}, cardTitleFont(),
                       t.colors.secondaryLabel);
        if (!app->developer.empty())
            p.drawText(cutText(p, app->developer, cardSubFont(), textW), {x, r.y + 62, textW, 16}, cardSubFont(),
                       t.colors.accent);
        std::string meta;
        if (!app->primaryCategory().empty()) meta = categoryName(app->primaryCategory());
        if (!appSizeText(*app).empty()) meta += "  ·  " + appSizeText(*app);
        if (app->ageRating > 0) meta += "  ·  " + std::to_string(app->ageRating) + "+";
        if (app->verified) meta += "  ·  已验证开发者";
        p.drawText(cutText(p, meta, cardSubFont(), textW), {x, r.y + 84, textW, 16}, cardSubFont(),
                   t.colors.secondaryLabel);
        const bool busy = m_model.stateOf(*app) == InstallState::Busy;
        const Rect chip{r.right() - (busy ? 150.0f : 116.0f), r.y + 66, busy ? 150.0f : 116.0f, 34};
        drawActionChip(p, m_model, *app, chip, m_hoverChip);
        if (app->installed && !app->installedVersion.empty()) {
            const Font f = smallFont();
            const std::string text = "已安装 " + app->installedVersion;
            const float w = p.measureText(text, f);
            p.drawText(text, {r.right() - w, r.y + 44, w, 14}, f, t.colors.tertiaryLabel);
        }
    }
    bool onPointerDown(const PointerEvent& e) override {
        if (e.button != MouseButton::Left) return false;
        if (chipRect().contains(e.position)) {
            m_pressedChip = true;
            return true;
        }
        return false;
    }
    void onPointerUp(const PointerEvent& e) override {
        const Rect chip = chipRect();
        if (!m_pressedChip) return;
        m_pressedChip = false;
        const AppEntry* app = m_model.find(m_id);
        if (app && chip.contains(e.position)) activate(m_model, *app);
    }
    void onPointerMove(const PointerEvent& e) override {
        const bool over = chipRect().contains(e.position);
        if (over != m_hoverChip) {
            m_hoverChip = over;
            invalidate();
        }
        if (host()) host()->setCursor(CursorShape::PointingHand);
    }
    [[nodiscard]] bool isInteractive() const override { return true; }
    bool onKeyDown(const KeyEvent& e) override {
        if (e.key == Key::Enter && m_model.callbacks.openApp) {
            m_model.callbacks.openApp(m_id);
            return true;
        }
        return false;
    }

private:
    [[nodiscard]] Rect chipRect() const {
        const AppEntry* app = m_model.find(m_id);
        const bool busy = app && m_model.stateOf(*app) == InstallState::Busy;
        return {frame().right() - (busy ? 150.0f : 116.0f), frame().y + 66, busy ? 150.0f : 116.0f, 34};
    }

    StoreModel& m_model;
    std::string m_id;
    bool m_hoverChip = false;
    bool m_pressedChip = false;
};

class InfoGridView : public Widget {
public:
    explicit InfoGridView(std::vector<std::pair<std::string, std::string>> rows) : m_rows(std::move(rows)) {}
    Size measureContent(Size available) override { return {available.width, 72}; }
    [[nodiscard]] bool expandsContent(Axis axis) const override { return axis == Axis::Horizontal; }
    void draw(Painter& p) override {
        const Rect r = frame();
        if (m_rows.empty()) return;
        const Theme& t = theme();
        p.fillRoundedRect(r, 14, t.isDark() ? Color::white(0.045f) : Color::hex(0xFAFAFC));
        p.strokeRoundedRect(r, 14, t.colors.separator.multipliedAlpha(0.6f), 0.5f);
        const float cell = r.width / static_cast<float>(m_rows.size());
        for (std::size_t i = 0; i < m_rows.size(); ++i) {
            if (i > 0)
                p.fillRect({r.x + cell * static_cast<float>(i), r.y + 14, 1.0f / p.scale(), r.height - 28},
                           t.colors.separator);
            const Rect c{r.x + cell * static_cast<float>(i), r.y, cell, r.height};
            const Font label = smallFont();
            const Font value = Font{12, FontWeight::Semibold, 16};
            const float lw = p.measureText(m_rows[i].first, label);
            p.drawText(m_rows[i].first, {c.center().x - lw * 0.5f, c.y + 13, lw, 14}, label, t.colors.tertiaryLabel);
            const std::string v = cutText(p, m_rows[i].second, value, cell - 12);
            const float vw = p.measureText(v, value);
            p.drawText(v, {c.center().x - vw * 0.5f, c.y + 31, vw, 18}, value, t.colors.label);
        }
    }

private:
    std::vector<std::pair<std::string, std::string>> m_rows;
};

class GalleryView : public Widget {
public:
    GalleryView(StoreModel& model, std::vector<Screenshot> shots) : m_model(model), m_shots(std::move(shots)) {}
    std::function<void(const Screenshot&)> onOpen;

    Size measureContent(Size available) override {
        const int cols = available.width > 900 ? 3 : 2;
        const float w = (available.width - kGap * static_cast<float>(cols - 1)) / static_cast<float>(cols);
        const float rows = std::ceil(static_cast<float>(m_shots.size()) / static_cast<float>(cols));
        return {available.width, rows * (w * 0.60f + 24)};
    }
    [[nodiscard]] bool expandsContent(Axis axis) const override { return axis == Axis::Horizontal; }
    void draw(Painter& p) override {
        const Rect r = frame();
        const int cols = r.width > 900 ? 3 : 2;
        const float w = (r.width - kGap * static_cast<float>(cols - 1)) / static_cast<float>(cols);
        const float h = w * 0.60f;
        for (std::size_t i = 0; i < m_shots.size(); ++i) {
            const int col = static_cast<int>(i) % cols;
            const int row = static_cast<int>(i) / cols;
            const Rect shot{r.x + static_cast<float>(col) * (w + kGap), r.y + static_cast<float>(row) * (h + 24),
                            w, h};
            p.fillRoundedRect(shot, 12, theme().colors.fill);
            if (auto img = m_model.images.screenshot(m_shots[i].thumbnail, 1100)) {
                const float ia = static_cast<float>(img->width()) / std::max(1, img->height());
                const float ra = shot.width / std::max(1.0f, shot.height);
                Rect src{0, 0, 1, 1};
                if (ia > ra) {
                    src.width = ra / ia;
                    src.x = (1 - src.width) * 0.5f;
                } else {
                    src.height = ia / ra;
                    src.y = (1 - src.height) * 0.5f;
                }
                p.drawImage(shot, *img, 12, 1, src);
                p.strokeRoundedRect(shot, 12, Color::black(0.08f), 0.5f);
            } else {
                p.drawIcon(Icon::Photo, {shot.center().x - 16, shot.center().y - 16, 32, 32},
                           theme().colors.tertiaryLabel, 0.8f);
            }
            if (m_hover == static_cast<int>(i))
                p.strokeRoundedRect(shot.inset(-1.5f), 13.5f, theme().colors.accent.multipliedAlpha(0.7f), 2);
            if (!m_shots[i].caption.empty())
                p.drawText(cutText(p, m_shots[i].caption, cardSubFont(), w), {shot.x, shot.bottom() + 3, w, 16},
                           cardSubFont(), theme().colors.secondaryLabel);
        }
    }
    void onPointerMove(const PointerEvent& e) override {
        const Rect r = frame();
        const int cols = r.width > 900 ? 3 : 2;
        const float w = (r.width - kGap * static_cast<float>(cols - 1)) / static_cast<float>(cols);
        const float h = w * 0.60f;
        int over = -1;
        for (std::size_t i = 0; i < m_shots.size(); ++i) {
            const int col = static_cast<int>(i) % cols;
            const int row = static_cast<int>(i) / cols;
            const Rect shot{r.x + static_cast<float>(col) * (w + kGap), r.y + static_cast<float>(row) * (h + 24),
                            w, h};
            if (shot.contains(e.position)) over = static_cast<int>(i);
        }
        if (over != m_hover) {
            m_hover = over;
            invalidate();
        }
        if (host()) host()->setCursor(over >= 0 ? CursorShape::PointingHand : CursorShape::Arrow);
    }
    void onPointerLeave() override {
        m_hover = -1;
        invalidate();
    }
    bool onPointerDown(const PointerEvent& e) override { return e.button == MouseButton::Left; }
    void onPointerUp(const PointerEvent& e) override {
        if (m_hover < 0 || m_hover >= static_cast<int>(m_shots.size()) || !frame().contains(e.position)) return;
        if (onOpen) onOpen(m_shots[static_cast<std::size_t>(m_hover)]);
    }
    [[nodiscard]] bool isInteractive() const override { return true; }

private:
    StoreModel& m_model;
    std::vector<Screenshot> m_shots;
    int m_hover = -1;
};

/// Release notes block: version + date on one line, then the wrapped notes.
class ReleaseView : public Widget {
public:
    ReleaseView(std::string version, std::string date, std::string notes)
        : m_version(std::move(version)), m_date(std::move(date)), m_notes(std::move(notes)) {}
    Size measureContent(Size available) override {
        const int lines = std::max(1, static_cast<int>(m_notes.size() / 70) + 1);
        return {available.width, 30 + static_cast<float>(std::min(lines, 12)) * 19};
    }
    [[nodiscard]] bool expandsContent(Axis axis) const override { return axis == Axis::Horizontal; }
    void draw(Painter& p) override {
        const Rect r = frame();
        const Theme& t = theme();
        const std::string title = m_version.empty() ? "版本说明" : "版本 " + m_version;
        p.drawText(title, {r.x, r.y, r.width * 0.6f, 20}, t.font(TextStyle::Headline), t.colors.label);
        if (!m_date.empty()) {
            const Font f = smallFont();
            const float w = p.measureText(m_date, f);
            p.drawText(m_date, {r.right() - w, r.y + 3, w, 16}, f, t.colors.secondaryLabel);
        }
        const std::string text = m_notes.empty() ? "本次更新包含错误修复和性能改进。" : m_notes;
        float y = r.y + 26;
        for (const auto& line : wrapText(p, text, t.font(TextStyle::Body), r.width, 12)) {
            if (line.empty()) {
                y += 8;
                continue;
            }
            p.drawText(line, {r.x, y, r.width, 19}, t.font(TextStyle::Body), t.colors.secondaryLabel);
            y += 19;
        }
    }

private:
    std::string m_version, m_date, m_notes;
};

/// A long description: six lines with a "更多 / 收起" toggle. The framework's LabelView does the wrapping,
/// so measuring uses real glyph advances (a custom wrapper would have to guess before the first frame).
class DescriptionBlockView : public Widget {
public:
    explicit DescriptionBlockView(std::string text) {
        m_label = std::make_shared<LabelView>(std::move(text));
        m_label->setTextTranslationEnabled(false);
        m_label->setTextStyle(TextStyle::Body);
        m_label->setColorRole(LabelColor::Secondary);
        m_label->setLineLimit(6);
        addChild(m_label);
        m_toggle = std::make_shared<CanvasView>([](Painter& p, const Rect& r) {
            const auto& t = theme();
            p.drawText("更多", r, t.font(TextStyle::Body).withWeight(FontWeight::Medium), t.colors.accent);
        });
        m_toggle->setOnClick([this](Point) {
            m_expanded = !m_expanded;
            m_label->setLineLimit(m_expanded ? 0 : 6);
            invalidateLayout();
            if (parent()) parent()->invalidateLayout();
        });
        m_toggle->setFixedWidth(44);
        m_toggle->setFixedHeight(20);
        addChild(m_toggle);
        // A second, hidden label without a line limit tells us whether the text was truncated at all.
        m_full = std::make_shared<LabelView>(m_label->text());
        m_full->setTextTranslationEnabled(false);
        m_full->setTextStyle(TextStyle::Body);
        m_full->setLineLimit(0);
    }

    Size measureContent(Size available) override {
        const Size label = m_label->measure({available.width, kInfinity});
        const Size full = m_full->measure({available.width, kInfinity});
        m_truncated = full.height > label.height + 1;
        return {available.width, label.height + (m_truncated ? 24 : 0)};
    }
    [[nodiscard]] bool expandsContent(Axis axis) const override { return axis == Axis::Horizontal; }
    void arrangeContent(const Rect& content) override {
        const Size label = m_label->measure({content.width, kInfinity});
        m_label->arrange({content.x, content.y, content.width, label.height});
        m_toggle->setHidden(!m_truncated);
        if (m_truncated) m_toggle->arrange({content.x, content.y + label.height + 4, 44, 20});
    }
    void draw(Painter& p) override { paintChildren(p); }

private:
    std::shared_ptr<LabelView> m_label;
    std::shared_ptr<LabelView> m_full;
    std::shared_ptr<CanvasView> m_toggle;
    bool m_expanded = false;
    bool m_truncated = false;
};

/// "卸载" with an inline confirmation: the first click asks, the second one acts.
class UninstallButtonView : public Widget {
public:
    explicit UninstallButtonView(std::function<void()> onConfirm) : m_onConfirm(std::move(onConfirm)) {}
    Size measureContent(Size /*available*/) override { return {120, 30}; }
    void draw(Painter& p) override {
        const Rect r = frame();
        const Theme& t = theme();
        if (isHovered()) p.fillRoundedRect(r, 8, t.colors.hover);
        p.drawIcon(Icon::Trash, {r.x + 4, r.center().y - 8, 16, 16}, t.colors.destructive);
        p.drawText(m_confirming ? "再次点击以卸载" : "卸载", {r.x + 28, r.y, r.width - 28, r.height},
                   t.font(TextStyle::Body), t.colors.destructive);
    }
    bool onPointerDown(const PointerEvent& e) override { return e.button == MouseButton::Left; }
    void onPointerUp(const PointerEvent& e) override {
        if (!frame().contains(e.position)) return;
        if (!m_confirming) {
            m_confirming = true;
            invalidateLayout();
            if (parent()) parent()->invalidateLayout();
            return;
        }
        const auto action = m_onConfirm;
        if (action) action();
    }
    void onPointerMove(const PointerEvent& e) override {
        if (host()) host()->setCursor(frame().contains(e.position) ? CursorShape::PointingHand : CursorShape::Arrow);
    }
    [[nodiscard]] bool isInteractive() const override { return true; }

private:
    std::function<void()> m_onConfirm;
    bool m_confirming = false;
};

/// Small tinted icon button ("分享", "在浏览器中打开").
class LinkButtonView : public Widget {
public:
    LinkButtonView(Icon icon, std::string title, std::function<void()> onClick)
        : m_icon(icon), m_title(std::move(title)), m_onClick(std::move(onClick)) {}
    Size measureContent(Size /*available*/) override { return {150, 30}; }
    void draw(Painter& p) override {
        const Rect r = frame();
        const Theme& t = theme();
        if (isHovered()) p.fillRoundedRect(r, 8, t.colors.hover);
        p.drawIcon(m_icon, {r.x + 4, r.center().y - 8, 16, 16}, t.colors.accent);
        p.drawText(m_title, {r.x + 28, r.y, r.width - 28, r.height}, t.font(TextStyle::Body), t.colors.accent);
    }
    bool onPointerDown(const PointerEvent& e) override { return e.button == MouseButton::Left; }
    void onPointerUp(const PointerEvent& e) override {
        if (frame().contains(e.position) && m_onClick) m_onClick();
    }
    void onPointerMove(const PointerEvent& e) override {
        if (host()) host()->setCursor(frame().contains(e.position) ? CursorShape::PointingHand : CursorShape::Arrow);
    }
    [[nodiscard]] bool isInteractive() const override { return true; }

private:
    Icon m_icon;
    std::string m_title;
    std::function<void()> m_onClick;
};

// --- page builders -------------------------------------------------------------------------------

std::vector<std::string> sectionApps(StoreModel& model, const Section& s, std::size_t limit = 60) {
    std::vector<std::string> ids;
    std::set<std::string> seen;
    for (const auto& id : s.featured) {
        if (model.find(id) && seen.insert(id).second) ids.push_back(id);
    }
    for (const auto& cat : s.categories) {
        for (const AppEntry* app : model.catalog.inCategory(cat, 100000)) {
            if (ids.size() >= limit) break;
            if (seen.insert(app->id).second) ids.push_back(app->id);
        }
    }
    return ids;
}

std::shared_ptr<Widget> heading(const std::string& title, const std::string& subtitle = {},
                                const std::string& action = {}, std::function<void()> onAction = {}) {
    return std::make_shared<HeadingView>(title, subtitle, action, std::move(onAction));
}

std::shared_ptr<Widget> appGrid(StoreModel& model, const std::vector<std::string>& ids, float minCard = kGridCard) {
    return std::make_shared<AppGridView>(model, ids, minCard);
}

std::shared_ptr<PageView> makePage() { return std::make_shared<PageView>(); }

}  // namespace

// -------------------------------------------------------------------------------------------------
// Pages
// -------------------------------------------------------------------------------------------------

/// Shown when there is no AppStream catalogue at all (Flatpak missing, no remote, or no cache yet).
class SetupView : public Widget {
public:
    explicit SetupView(StoreModel& model) : m_model(model) {}
    Size measureContent(Size available) override { return {available.width, 260}; }
    [[nodiscard]] bool expandsContent(Axis axis) const override { return axis == Axis::Horizontal; }
    void draw(Painter& p) override {
        const Rect r = frame();
        const Theme& t = theme();
        p.drawIcon(Icon::HardDrive, {r.center().x - 30, r.y + 20, 60, 60}, t.colors.tertiaryLabel, 1.0f);
        const std::string title = m_model.catalog.flatpakAvailable ? "尚未载入应用目录" : "未找到 Flatpak";
        const float tw = p.measureText(title, t.font(TextStyle::Title2));
        p.drawText(title, {r.center().x - tw * 0.5f, r.y + 96, tw, 24}, t.font(TextStyle::Title2), t.colors.label);
        const std::string body =
            m_model.catalog.flatpakAvailable
                ? "本机还没有 Flatpak 的 AppStream 缓存。添加 Flathub 并生成元数据后即可浏览应用。"
                : "Forma 应用商店使用 Flatpak 安装应用。请先安装 flatpak，并添加 Flathub 远程仓库。";
        const auto lines = wrapText(p, body, t.font(TextStyle::Body), std::min(560.0f, r.width), 3);
        float y = r.y + 126;
        for (const auto& line : lines) {
            const float w = p.measureText(line, t.font(TextStyle::Body));
            p.drawText(line, {r.center().x - w * 0.5f, y, w, 18}, t.font(TextStyle::Body), t.colors.secondaryLabel);
            y += 19;
        }
        const Rect code{r.center().x - 260, y + 10, 520, 30};
        p.fillRoundedRect(code, 8, t.isDark() ? Color::white(0.06f) : Color::hex(0xF2F2F5));
        p.drawText("flatpak remote-add --if-not-exists flathub https://dl.flathub.org/repo/", code,
                   Font{11, FontWeight::Regular, 15}, t.colors.secondaryLabel, TextAlign::Center);
    }

private:
    StoreModel& m_model;
};

std::shared_ptr<Widget> discoverPage(StoreModel& model) {
    auto page = makePage();
    // Installed refs alone (flatpak list without AppStream) can't fill a storefront; ask for setup.
    std::size_t described = 0;
    for (const auto& app : model.catalog.apps)
        if (!app.summary.empty()) ++described;
    if (described < 8) {
        page->add(heading("精选", "发现值得一试的 Flatpak 应用"));
        page->add(Element(std::make_shared<SetupView>(model)));
        return page;
    }
    page->add(Element(std::make_shared<HeroView>(model)));

    // "本周精选": icon carousel of hand-picked apps.
    std::vector<std::string> picks;
    for (const auto& id : discoverOrder()) {
        if (picks.size() >= 6) break;
        if (model.find(id)) picks.push_back(id);
    }
    page->add(Element(std::make_shared<SectionHeadingView>("本周精选", "查看全部", [&model] {
        if (model.callbacks.openSection) model.callbacks.openSection("create");
    })));
    page->add(Element(std::make_shared<CardRowView>(model, picks, kCard)));

    // Editorial cards: the first two featured apps with screenshots.
    std::vector<std::string> featured;
    for (const auto& id : discoverOrder()) {
        const AppEntry* app = model.find(id);
        if (!app || app->screenshots.empty()) continue;
        featured.push_back(id);
        if (featured.size() >= 4) break;
    }
    page->add(Element(std::make_shared<SectionHeadingView>("编辑推荐")));
    page->add(Element(std::make_shared<FeaturedRowView>(model, featured)));

    // "热门应用": a grid of everything else.
    std::vector<std::string> popular;
    std::set<std::string> seen(picks.begin(), picks.end());
    for (const auto& id : discoverOrder()) {
        if (popular.size() >= 18) break;
        if (seen.insert(id).second && model.find(id)) popular.push_back(id);
    }
    page->add(Element(std::make_shared<SectionHeadingView>("热门应用")));
    page->add(Element(appGrid(model, popular)));

    // Category shortcuts.
    page->add(Element(std::make_shared<SectionHeadingView>("按类别浏览", "全部类别", [&model] {
        if (model.callbacks.openCategory) model.callbacks.openCategory("");
    })));
    std::vector<std::string> cats{"Graphics", "Development", "AudioVideo", "Office", "Utility", "Game", "Education", "Network"};
    page->add(Element(std::make_shared<CategoryGridView>(model, cats)));
    return page;
}

std::shared_ptr<Widget> sectionPage(StoreModel& model, const std::string& sectionId) {
    auto page = makePage();
    const Section* s = section(sectionId);
    if (!s) return page;
    page->add(heading(s->title, s->subtitle));
    std::vector<std::string> ids = sectionApps(model, *s);
    std::vector<std::string> featured;
    std::vector<std::string> rest;
    std::set<std::string> inFeatured;
    for (const auto& id : ids) {
        const AppEntry* app = model.find(id);
        if (!app) continue;
        if (featured.size() < 4 && !app->screenshots.empty() && inFeatured.insert(id).second)
            featured.push_back(id);
        else
            rest.push_back(id);
    }
    if (!featured.empty()) {
        page->add(Element(std::make_shared<SectionHeadingView>("精选推荐")));
        page->add(Element(std::make_shared<FeaturedRowView>(model, featured)));
    }
    if (!rest.empty()) {
        page->add(Element(std::make_shared<SectionHeadingView>(featured.empty() ? "全部应用" : "更多应用")));
        page->add(Element(appGrid(model, rest)));
    }
    if (ids.empty()) {
        page->add(Element(std::make_shared<EmptyView>(Icon::Grid, "此分类暂无应用",
                                                      "当前 Flatpak 远程仓库中没有符合此分类的应用。")));
    }
    return page;
}

std::shared_ptr<Widget> categoryPage(StoreModel& model, const std::string& category) {
    auto page = makePage();
    if (category.empty()) return categoriesPage(model);
    std::vector<std::string> ids;
    for (const AppEntry* app : model.catalog.inCategory(category, 100000)) ids.push_back(app->id);
    page->add(heading(categoryName(category), std::to_string(ids.size()) + " 个应用"));
    if (ids.empty())
        page->add(Element(std::make_shared<EmptyView>(Icon::Grid, "暂无应用", "此类别下还没有应用。")));
    else
        page->add(Element(appGrid(model, ids)));
    return page;
}

std::shared_ptr<Widget> categoriesPage(StoreModel& model) {
    auto page = makePage();
    page->add(heading("类别", "按用途浏览 Flatpak 应用"));
    std::vector<std::string> cats;
    std::map<std::string, int> counts;
    for (const auto& app : model.catalog.apps) {
        for (const auto& cat : app.categories) {
            if (cat == "GTK" || cat == "Qt" || cat == "GNOME" || cat == "KDE") continue;
            counts[cat]++;
        }
    }
    for (const auto& [cat, count] : counts) {
        if (count >= 8) cats.push_back(cat);
    }
    std::sort(cats.begin(), cats.end(), [&](const std::string& a, const std::string& b) {
        return counts[a] > counts[b];
    });
    if (cats.size() > 24) cats.resize(24);
    page->add(Element(std::make_shared<CategoryGridView>(model, cats)));
    return page;
}

std::shared_ptr<Widget> searchPage(StoreModel& model, const std::string& query) {
    auto page = makePage();
    const std::string q = query;
    page->add(heading(q.empty() ? "搜索" : "搜索 “" + q + "”"));
    if (q.empty()) {
        page->add(Element(std::make_shared<EmptyView>(Icon::Magnifier, "搜索应用",
                                                      "输入应用名称、开发者或关键字，例如“终端”“音乐”“绘图”。")));
        return page;
    }
    const auto found = model.catalog.search(q, 90);
    std::vector<std::string> ids;
    for (const AppEntry* app : found) ids.push_back(app->id);
    if (ids.empty()) {
        page->add(Element(std::make_shared<EmptyView>(Icon::Magnifier, "无搜索结果",
                                                      "没有找到与 “" + q + "” 匹配的应用。试试更短的关键字。")));
    } else {
        page->add(Element(std::make_shared<SectionHeadingView>(std::to_string(ids.size()) + " 个结果")));
        page->add(Element(appGrid(model, ids)));
    }
    return page;
}

std::shared_ptr<Widget> updatesPage(StoreModel& model) {
    auto page = makePage();
    const auto pending = model.catalog.updates();
    std::vector<std::string> ids;
    for (const AppEntry* app : pending) ids.push_back(app->id);
    page->add(heading("更新", pending.empty() ? "所有应用均为最新版本"
                                              : std::to_string(pending.size()) + " 个应用可以更新",
                      pending.empty() ? "" : "全部更新", [&model] {
                          if (model.callbacks.updateAll) model.callbacks.updateAll();
                      }));
    page->add(Element(std::make_shared<SpaceView>(4, true)));
    if (ids.empty()) {
        page->add(Element(std::make_shared<EmptyView>(Icon::Checkmark, "已是最新版本",
                                                      "Flatpak 应用和运行时都已更新。新版本可用时会显示在这里。")));
        return page;
    }
    page->add(Element(std::make_shared<AppListView>(model, ids, true)));
    if (model.offline) {
        page->add(Element(std::make_shared<SectionHeadingView>("离线")));
        page->add(Element(std::make_shared<EmptyView>(Icon::Wifi, "无法连接 Flatpak 远程仓库",
                                                      "检查网络连接后刷新，以查看可用的更新。")));
    }
    return page;
}

std::shared_ptr<Widget> installedPage(StoreModel& model) {
    auto page = makePage();
    const auto installed = model.catalog.installedApps();
    std::vector<std::string> ids;
    for (const AppEntry* app : installed) ids.push_back(app->id);
    page->add(heading("已安装", std::to_string(installed.size()) + " 个应用"));
    page->add(Element(std::make_shared<SpaceView>(2, true)));
    if (ids.empty()) {
        page->add(Element(std::make_shared<EmptyView>(Icon::HardDrive, "还没有安装应用",
                                                      "在精选或搜索页面选择应用，点击“获取”即可安装。")));
        return page;
    }
    page->add(Element(std::make_shared<AppListView>(model, ids, false)));
    return page;
}

std::shared_ptr<Widget> appPage(StoreModel& model, const std::string& id) {
    auto page = makePage();
    const AppEntry* app = model.find(id);
    if (!app) {
        page->add(Element(std::make_shared<EmptyView>(Icon::Info, "找不到应用", "此应用不在当前 Flatpak 目录中。")));
        return page;
    }
    page->add(Element(std::make_shared<AppHeaderView>(model, id)));
    page->add(Element(std::make_shared<SpaceView>(8, true)));

    std::vector<std::pair<std::string, std::string>> info;
    if (app->ageRating > 0) info.emplace_back("年龄分级", std::to_string(app->ageRating) + "+");
    if (!appSizeText(*app).empty()) info.emplace_back("大小", appSizeText(*app));
    if (!app->primaryCategory().empty()) info.emplace_back("类别", categoryName(app->primaryCategory()));
    if (!app->license.empty()) info.emplace_back("许可证", app->license);
    if (!app->runtime.empty()) {
        std::string runtime = app->runtime;
        const std::size_t slash = runtime.find('/');
        const std::size_t last = runtime.rfind('/');
        if (slash != std::string::npos && last != slash && last + 1 < runtime.size())
            runtime = runtime.substr(0, slash) + " " + runtime.substr(last + 1);
        info.emplace_back("运行时", runtime);
    }
    if (info.empty()) info.emplace_back("来源", app->origin.empty() ? "Flatpak" : app->origin);
    page->add(Element(std::make_shared<InfoGridView>(info)));

    if (!app->screenshots.empty()) {
        auto gallery = std::make_shared<GalleryView>(model, app->screenshots);
        page->add(Element(gallery));
    }

    page->add(Element(std::make_shared<SectionHeadingView>("关于此应用")));
    std::string description = app->description;
    if (description.empty()) description = app->summary;
    if (!description.empty()) page->add(Element(std::make_shared<DescriptionBlockView>(description)));

    page->add(Element(std::make_shared<SectionHeadingView>("新功能")));
    page->add(Element(std::make_shared<ReleaseView>(app->version, formatDate(app->releaseTimestamp),
                                                    app->releaseNotes.empty() ? "" : app->releaseNotes)));

    // Links and ID.
    std::vector<Element> links;
    if (!app->homepage.empty()) {
        const std::string url = app->homepage;
        links.emplace_back(std::make_shared<LinkButtonView>(Icon::Globe, "开发者网站", [url] {
            if (launchDetached({"xdg-open", url})) return;
            launchDetached({"gio", "open", url});
        }));
    }
    const std::string appId = app->id;
    StoreModel* modelPtr = &model;
    links.emplace_back(std::make_shared<LinkButtonView>(Icon::DocOnDoc, "复制应用 ID", [modelPtr, appId] {
        if (modelPtr->callbacks.copyText) modelPtr->callbacks.copyText(appId);
    }));
    links.emplace_back(std::make_shared<LinkButtonView>(Icon::Globe, "在 Flathub 查看", [appId] {
        const std::string url = "https://flathub.org/apps/" + appId;
        if (!launchDetached({"xdg-open", url})) launchDetached({"gio", "open", url});
    }));
    if (app->installed) {
        links.emplace_back(std::make_shared<UninstallButtonView>([modelPtr, appId] {
            if (modelPtr->callbacks.uninstall) modelPtr->callbacks.uninstall(appId);
        }));
    }
    // A simple HStack of link buttons.
    auto linkRow = std::make_shared<StackView>(Axis::Horizontal);
    linkRow->setSpacing(16);
    for (auto& e : links) linkRow->addChild(e.widget());
    page->add(Element(linkRow));

    // Technical details.
    std::vector<std::pair<std::string, std::string>> technical;
    technical.emplace_back("标识符", app->id);
    if (!app->branch.empty()) technical.emplace_back("分支", app->branch);
    if (!app->arch.empty()) technical.emplace_back("架构", app->arch);
    if (!app->origin.empty()) technical.emplace_back("来源", app->origin);
    if (app->installed) technical.emplace_back("安装", app->installation == "user" ? "用户" : "系统");
    page->add(Element(std::make_shared<InfoGridView>(technical)));
    return page;
}

std::shared_ptr<Widget> settingsPage(StoreModel& model) {
    auto page = makePage();
    page->add(heading("设置", "Forma 应用商店"));
    std::vector<std::pair<std::string, std::string>> info;
    info.emplace_back("可用应用", std::to_string(model.catalog.apps.size()));
    info.emplace_back("已安装", std::to_string(model.catalog.installedApps().size()));
    info.emplace_back("可更新", std::to_string(model.catalog.updates().size()));
    page->add(Element(std::make_shared<InfoGridView>(info)));
    std::string remoteText;
    for (const auto& r : model.catalog.remotes) remoteText += (remoteText.empty() ? "" : "、") + r;
    page->add(Element(std::make_shared<SectionHeadingView>("Flatpak")));
    std::vector<std::pair<std::string, std::string>> fp;
    fp.emplace_back("远程仓库", remoteText.empty() ? "未配置" : remoteText);
    fp.emplace_back("数据来源", "AppStream");
    page->add(Element(std::make_shared<InfoGridView>(fp)));
    page->add(Element(std::make_shared<SectionHeadingView>("元数据")));
    auto desc = std::make_shared<DescriptionBlockView>(
        model.catalog.appStreamPath.empty()
            ? "未找到 Flatpak AppStream 缓存。请运行一次 `flatpak remote-ls` 或软件中心以生成元数据。"
            : "AppStream 文件：" + model.catalog.appStreamPath + "\n\n应用图标缓存在 ~/.cache/mayonnaise/app_store。"
              "安装与更新通过 flatpak 命令行完成，遵循系统权限与远程仓库配置。");
    page->add(Element(desc));
    if (!model.catalog.error.empty()) {
        page->add(Element(std::make_shared<SectionHeadingView>("错误")));
        page->add(Element(std::make_shared<DescriptionBlockView>(model.catalog.error)));
    }
    return page;
}

// -------------------------------------------------------------------------------------------------
// Sidebar
// -------------------------------------------------------------------------------------------------

namespace {

class SidebarBodyView : public Widget {
public:
    SidebarBodyView(StoreModel& model, std::string selected, std::function<void(const std::string&)> onSelect,
                    std::shared_ptr<Widget> search) : m_model(model) {
        m_sidebar = std::make_shared<SidebarView>(items());
        m_sidebar->setHeader(std::move(search));
        m_sidebar->setSelection(selected);
        m_sidebar->setOnSelect(onSelect);
        addChild(m_sidebar);
        m_footer = Button(Icon::Person, "本机应用与设置").style(ButtonStyle::Borderless)
                       .onClick([onSelect] { if (onSelect) onSelect("settings"); }).node();
        addChild(m_footer);
        m_count = model.updateCount;
    }
    Size measureContent(Size available) override { return available; }
    void setSelected(const std::string& id) {
        if (m_count != m_model.updateCount) {
            m_count = m_model.updateCount;
            m_sidebar->setItems(items());
        }
        m_sidebar->setSelection(id);
    }
    void arrangeContent(const Rect& r) override {
        const float footer = 54;
        m_sidebar->measure({r.width, std::max(0.0f, r.height - footer)});
        m_sidebar->arrange({r.x, r.y + 8, r.width, std::max(0.0f, r.height - footer - 8)});
        m_footer->measure({r.width - 24, 32});
        m_footer->arrange({r.x + 12, r.bottom() - 42, r.width - 24, 32});
    }
    void draw(Painter& p) override { paintChildren(p); }
private:
    std::vector<SidebarItem> items() const {
        std::vector<SidebarItem> rows;
        const Icon icons[] = {Icon::Sparkles, Icon::Paintbrush, Icon::Document, Icon::Play,
                              Icon::Terminal, Icon::Library, Icon::Grid};
        std::size_t index = 0;
        for (const auto& [id, title] : sidebarSections()) rows.emplace_back(id, icons[index++], title);
        rows.push_back(SidebarItem::header(""));
        rows.emplace_back("categories", Icon::Columns, "类别");
        rows.push_back(SidebarItem("updates", Icon::ArrowDownCircle, "更新")
                           .withBadge(m_model.updateCount > 0 ? std::to_string(m_model.updateCount) : ""));
        rows.emplace_back("installed", Icon::HardDrive, "已安装");
        return rows;
    }
    StoreModel& m_model;
    std::shared_ptr<SidebarView> m_sidebar;
    std::shared_ptr<Widget> m_footer;
    int m_count = 0;
};

}  // namespace

std::shared_ptr<Widget> sidebarBody(StoreModel& model, const std::string& selected,
                                    std::function<void(const std::string&)> onSelect,
                                    const Element& searchField) {
    return std::make_shared<SidebarBodyView>(model, selected, std::move(onSelect), searchField.widget());
}

void sidebarSelect(Widget& sidebar, const std::string& id) {
    if (auto* body = dynamic_cast<SidebarBodyView*>(&sidebar)) body->setSelected(id);
}

const std::vector<std::pair<std::string, std::string>>& sidebarSections() {
    static const std::vector<std::pair<std::string, std::string>> items{
        {"discover", "精选"}, {"create", "创意"}, {"work", "工作"}, {"play", "娱乐"},
        {"develop", "开发"},  {"learn", "学习"},  {"utilities", "实用工具"},
    };
    return items;
}

InstallState StoreModel::stateOf(const AppEntry& app) const {
    if (busy.count(app.id)) return InstallState::Busy;
    if (broken.count(app.id)) return InstallState::Broken;
    if (app.installed) return app.updateAvailable ? InstallState::Update : InstallState::Open;
    if (app.eol) return InstallState::Unavailable;
    return InstallState::Get;
}

float StoreModel::progressOf(const std::string& id) const {
    const auto it = progress.find(id);
    return it == progress.end() ? -1.0f : it->second;
}

}  // namespace forma::store
