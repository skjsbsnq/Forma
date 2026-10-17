#include "../common/zh_cn.hpp"
// Terminal — a terminal emulator in the style of macOS Terminal, built on Mayonnaise.
//
//   terminal [directory]
//
// xterm-compatible (256 colours, true colour, alternate screen, mouse reporting, bracketed paste, focus
// events, OSC titles / working directory / clipboard), with scrollback that re-wraps on resize, tabs,
// selection and copy / paste, Ctrl+click links and colour profiles. Linux only (forkpty, /proc).
//
// Shortcuts: Ctrl+Shift+T new tab, Ctrl+Shift+W close tab, Ctrl+PageUp/PageDown or Ctrl+Tab switch tabs,
// Ctrl+Shift+C / Ctrl+Shift+V copy / paste, Ctrl+Shift+A select all, Ctrl+Shift+K clear scrollback,
// Ctrl+= / Ctrl+- / Ctrl+0 text size, Shift+PageUp/PageDown scroll back, Ctrl+, settings.
#include <mayonnaise/mayonnaise.hpp>

#include <sys/wait.h>
#include <unistd.h>

#include <algorithm>
#include <cmath>
#include <cstdlib>
#include <memory>
#include <string>
#include <vector>

#include "profiles.hpp"
#include "pty.hpp"
#include "terminal_view.hpp"

using namespace mayo;
using term::TerminalView;

namespace {

constexpr float kTabBarHeight = 30;

/// macOS Terminal's tab bar: equal-width tabs, the close button on hover, "+" at the end. Shown only
/// when there is more than one tab.
class TabBarView final : public Widget {
public:
    std::vector<std::string> titles;
    int active = 0;
    std::function<void(int)> onSelect;
    std::function<void(int)> onClose;
    std::function<void()> onNew;

    Size measureContent(Size available) override { return {available.width, kTabBarHeight}; }
    [[nodiscard]] bool expandsContent(Axis a) const override { return a == Axis::Horizontal; }
    [[nodiscard]] bool isInteractive() const override { return true; }

    void draw(Painter& p) override {
        const Theme& t = theme();
        const auto& c = t.colors;
        const Rect r = frame();
        const float px = 1.0f / p.scale();
        p.fillRect(r, c.windowBackground.mix(t.isDark() ? Color::black() : Color::black(), t.isDark() ? 0.25f : 0.05f));
        p.fillRect({r.x, p.snap(r.bottom()) - px, r.width, px}, c.separator);
        const Font f = t.font(TextStyle::Subheadline);
        for (int i = 0; i < static_cast<int>(titles.size()); ++i) {
            const Rect tab = tabRect(i);
            const bool on = i == active;
            if (on) p.fillRect({tab.x, r.y, tab.width, r.height - px}, c.windowBackground);
            else if (i == m_hover) p.fillRect({tab.x, r.y, tab.width, r.height - px}, c.hover);
            if (i > 0 && i != active && i - 1 != active) p.fillRect({p.snap(tab.x), r.y + 7, px, r.height - 14}, c.separator);
            const Rect label = tab.inset(Insets(0, 26, 0, 26));
            const std::string title = fit(p, titles[static_cast<size_t>(i)], f, label.width);
            p.drawText(title, label, on ? f.withWeight(FontWeight::Medium) : f, on ? c.label : c.secondaryLabel, TextAlign::Center);
            if (i == m_hover) {
                const Rect x = closeRect(i);
                if (m_overClose) p.fillRoundedRect(x, 4, c.fill);
                p.drawIcon(Icon::Xmark, x.inset(4), c.secondaryLabel, 1.2f);
            }
        }
        const Rect plus = plusRect();
        if (m_overPlus) p.fillRoundedRect(plus.inset(3), 5, c.hover);
        p.drawIcon(Icon::Plus, plus.inset(8), c.secondaryLabel, 1.1f);
    }

    bool onPointerDown(const PointerEvent& e) override {
        if (plusRect().contains(e.position)) {
            if (onNew) onNew();
            return true;
        }
        const int i = tabAt(e.position);
        if (i < 0) return false;
        if (e.button == MouseButton::Middle || (e.button == MouseButton::Left && closeRect(i).contains(e.position))) {
            if (onClose) onClose(i);
        } else if (onSelect) {
            onSelect(i);
        }
        return true;
    }
    void onPointerMove(const PointerEvent& e) override {
        const int h = tabAt(e.position);
        const bool overClose = h >= 0 && closeRect(h).contains(e.position);
        const bool overPlus = plusRect().contains(e.position);
        if (h != m_hover || overClose != m_overClose || overPlus != m_overPlus) {
            m_hover = h;
            m_overClose = overClose;
            m_overPlus = overPlus;
            invalidate();
        }
    }
    void onPointerLeave() override {
        m_hover = -1;
        m_overClose = m_overPlus = false;
        invalidate();
    }

private:
    static std::string fit(Painter& p, const std::string& s, const Font& f, float w) {
        if (p.measureText(s, f) <= w) return s;
        std::string out = s;
        while (!out.empty() && p.measureText(out + "…", f) > w) {
            out.pop_back();
            while (!out.empty() && (static_cast<unsigned char>(out.back()) & 0xC0) == 0x80) out.pop_back();
        }
        return out + "…";
    }
    [[nodiscard]] Rect plusRect() const { return {frame().right() - 34, frame().y, 34, frame().height}; }
    [[nodiscard]] Rect tabRect(int i) const {
        const float w = (frame().width - 34) / static_cast<float>(std::max<size_t>(1, titles.size()));
        return {frame().x + w * static_cast<float>(i), frame().y, w, frame().height};
    }
    [[nodiscard]] Rect closeRect(int i) const {
        const Rect t = tabRect(i);
        return {t.x + 6, t.center().y - 8, 16, 16};
    }
    [[nodiscard]] int tabAt(Point pt) const {
        for (int i = 0; i < static_cast<int>(titles.size()); ++i) {
            if (tabRect(i).contains(pt)) return i;
        }
        return -1;
    }
    int m_hover = -1;
    bool m_overClose = false;
    bool m_overPlus = false;
};

/// Tab bar on top (when there are several tabs), the active terminal below it.
class TerminalPage final : public Widget {
public:
    std::shared_ptr<TabBarView> tabBar = std::make_shared<TabBarView>();
    std::shared_ptr<Widget> menuAnchor = std::make_shared<Widget>();
    TerminalPage() {
        addChild(tabBar);
        addChild(menuAnchor);
    }
    [[nodiscard]] bool expandsContent(Axis /*axis*/) const override { return true; }
    void arrangeContent(const Rect& c) override {
        const bool tabs = tabBar->titles.size() > 1;
        tabBar->setHidden(!tabs);
        const float top = tabs ? kTabBarHeight : 0.0f;
        if (tabs) {
            tabBar->measure({c.width, kTabBarHeight});
            tabBar->arrange({c.x, c.y, c.width, kTabBarHeight});
        }
        for (const auto& child : children()) {
            if (child == tabBar || child == menuAnchor || !child->isVisible()) continue;
            child->measure({c.width, c.height - top});
            child->arrange({c.x, c.y + top, c.width, c.height - top});
        }
    }
};

/// Small confirmation sheet, centred in the window.
class ConfirmPanel final : public PopupView, public std::enable_shared_from_this<ConfirmPanel> {
public:
    ConfirmPanel(const std::string& title, const std::string& message, const std::string& action, std::function<void()> confirm)
        : PopupView(Material::Popover) {
        setPadding(Insets(18, 20, 16, 20));
        auto cancel = Button("Cancel").onClick([this] { dismiss(); });
        auto ok = Button(action).style(ButtonStyle::Destructive).onClick([this, confirm] {
            auto keep = PopupView::shared_from_this();
            dismiss();
            confirm();
        });
        auto stack = VStack({Label(title).font(TextStyle::Headline), Label(message).secondary().maxWidth(300),
                             HStack({Spacer(), cancel, ok}).spacing(8)})
                         .spacing(10);
        stack.width(300);
        addChild(stack.node());
        m_confirm = std::move(confirm);
    }
    void present(WidgetHost& host) {
        m_shown = true;
        m_shownHost = &host;
        m_appear.set(0.0f);
        m_appear.animateTo(1.0f, host.time(), Transition{0.16f, Curve::EaseOut});
        host.showOverlay(PopupView::shared_from_this());
        host.setFocus(this, false);
    }
    [[nodiscard]] bool acceptsFocus() const override { return true; }
    bool onKeyDown(const KeyEvent& e) override {
        if (e.key == Key::Enter) {
            auto keep = PopupView::shared_from_this();
            dismiss();
            if (m_confirm) m_confirm();
            return true;
        }
        return false;  // Escape dismisses through the window
    }
    Rect placePopup(Size window, Size measured) override {
        return {std::round((window.width - measured.width) * 0.5f), std::round((window.height - measured.height) * 0.35f),
                measured.width, measured.height};
    }

private:
    std::function<void()> m_confirm;
};

std::string tildePath(const std::string& path) {
    const char* home = std::getenv("HOME");
    if (home && *home && path.rfind(home, 0) == 0) return "~" + path.substr(std::string(home).size());
    return path;
}

}  // namespace

int main(int argc, char** argv) {
    mayo::setUiTranslator(forma::zhCN);
    App app("terminal", MAYO_DESKTOP_ID);
    auto& window = app.createWindow(
        {.title = "Terminal", .width = 760, .height = 500, .minWidth = 320, .minHeight = 180, .autosaveName = "main"});
    Settings& prefs = app.settings();
    auto config = std::make_shared<term::Config>();
    config->load(prefs);
    const bool demo = std::getenv("MAYO_DEMO") != nullptr;

    auto page = std::make_shared<TerminalPage>();
    std::vector<std::shared_ptr<TerminalView>> tabs;
    int active = -1;
    auto menu = std::make_shared<MenuView>(std::vector<MenuItem>{});

    std::function<void()> refreshTitles;
    std::function<void(const std::string&)> newTab;
    std::function<void(int)> closeTab;
    std::function<void(int)> selectTab;

    auto current = [&]() -> TerminalView* { return active >= 0 && active < static_cast<int>(tabs.size()) ? tabs[static_cast<size_t>(active)].get() : nullptr; };

    refreshTitles = [&] {
        std::vector<std::string> titles;
        for (const auto& t : tabs) titles.push_back(t->title());
        page->tabBar->titles = titles;
        page->tabBar->active = active;
        page->tabBar->invalidate();
        if (TerminalView* v = current()) {
            // macOS style: "program — directory — 80×24".
            std::string title = v->title();
            const std::string dir = v->currentDirectory();
            // Shells often put the directory in their title already.
            if (!dir.empty() && title.find(tildePath(dir)) == std::string::npos && title.find(dir) == std::string::npos)
                title += " — " + tildePath(dir);
            title += " — " + std::to_string(v->cols()) + "×" + std::to_string(v->rows());
            if (title != window.title()) window.setTitle(title);
        }
    };

    auto focusCurrent = [&] {
        if (TerminalView* v = current(); v && v->host()) v->host()->setFocus(v, false);
    };

    selectTab = [&](int i) {
        if (i < 0 || i >= static_cast<int>(tabs.size())) return;
        active = i;
        for (int k = 0; k < static_cast<int>(tabs.size()); ++k) tabs[static_cast<size_t>(k)]->setHidden(k != active);
        page->invalidateLayout();
        refreshTitles();
        focusCurrent();
    };

    auto openUrl = [&](const std::string& url) {
        if (demo) return;
        if (fork() == 0) {
            setsid();
            if (fork() == 0) {
                execlp("xdg-open", "xdg-open", url.c_str(), static_cast<char*>(nullptr));
                _exit(127);
            }
            _exit(0);
        }
        wait(nullptr);
    };

    auto showContextMenu = [&](TerminalView& v, Point at) {
        const std::string url = v.urlAt(at);
        std::vector<MenuItem> items{
            MenuItem("Copy", [&v] { v.copySelection(); }).withIcon(Icon::DocOnDoc).withShortcut("Ctrl+Shift+C").disabled(!v.hasSelection()),
            MenuItem("Paste", [&v] { v.paste(); }).withShortcut("Ctrl+Shift+V"),
            MenuItem("Select All", [&v] { v.selectAll(); }).withShortcut("Ctrl+Shift+A"),
            MenuItem::separator(),
        };
        if (!url.empty()) {
            items.push_back(MenuItem("Open Link", [&, url] { openUrl(url); }).withIcon(Icon::Globe));
            items.push_back(MenuItem("Copy Link", [&, url] {
                if (auto* h = page->host()) h->setClipboardText(url);
            }));
            items.push_back(MenuItem::separator());
        }
        items.push_back(MenuItem("Clear Scrollback", [&v] { v.clearScrollback(); }).withShortcut("Ctrl+Shift+K"));
        items.push_back(MenuItem("New Tab", [&] { newTab(current() ? current()->currentDirectory() : std::string()); })
                            .withIcon(Icon::Plus)
                            .withShortcut("Ctrl+Shift+T"));
        menu->setItems(std::move(items));
        menu->setGap(2);
        page->menuAnchor->arrange({at.x, at.y, 1, 1});
        menu->showFrom(*page->menuAnchor);
        if (auto* h = page->host()) h->setFocus(menu.get(), false);
    };
    menu->setOnDismiss([&] { app.post([&] { focusCurrent(); }); });

    newTab = [&](const std::string& cwd) {
        auto view = std::make_shared<TerminalView>(app, config);
        TerminalView* raw = view.get();
        tabs.push_back(view);
        page->addChild(view);
        view->onTitleChanged = [&] { refreshTitles(); };
        view->onContextMenu = showContextMenu;
        view->onOpenUrl = openUrl;
        view->onExit = [&, raw](int status) {
            const bool clean = WIFEXITED(status) && WEXITSTATUS(status) == 0;
            const bool close = config->onExit == term::ExitAction::Close || (config->onExit == term::ExitAction::CloseIfClean && clean);
            if (!close) return;
            app.post([&, raw] {
                for (int i = 0; i < static_cast<int>(tabs.size()); ++i) {
                    if (tabs[static_cast<size_t>(i)].get() == raw) closeTab(i);
                }
            });
        };
        selectTab(static_cast<int>(tabs.size()) - 1);
        // Start the shell once the view has its real size, so it begins with the right columns.
        app.post([&, raw, cwd] {
            std::string error;
            if (!raw->start(cwd, error)) std::fprintf(stderr, "terminal: %s\n", error.c_str());
            refreshTitles();
        });
    };

    closeTab = [&](int i) {
        if (i < 0 || i >= static_cast<int>(tabs.size())) return;
        page->removeChild(tabs[static_cast<size_t>(i)].get());
        tabs.erase(tabs.begin() + i);
        if (tabs.empty()) {
            window.close();
            return;
        }
        selectTab(std::min(i, static_cast<int>(tabs.size()) - 1));
    };

    // Closing something that is still running asks first, like macOS Terminal.
    auto confirmThen = [&](bool busy, const std::string& what, std::function<void()> action) {
        if (!busy || !config->confirmClose || !page->host()) {
            action();
            return;
        }
        std::make_shared<ConfirmPanel>("Do you want to close this " + what + "?",
                                       "Closing it will terminate the running processes in it.", "Close", std::move(action))
            ->present(*page->host());
    };
    auto requestCloseTab = [&](int i) {
        if (i < 0 || i >= static_cast<int>(tabs.size())) return;
        confirmThen(tabs[static_cast<size_t>(i)]->busy() && !tabs[static_cast<size_t>(i)]->exited(), "tab", [&, i] { closeTab(i); });
    };
    window.onCloseRequested([&] {
        const bool busy = std::any_of(tabs.begin(), tabs.end(), [](const auto& t) { return t->busy() && !t->exited(); });
        if (!busy || !config->confirmClose) return true;
        confirmThen(true, "window", [&] { window.close(); });
        return false;
    });

    page->tabBar->onSelect = selectTab;
    page->tabBar->onClose = requestCloseTab;
    page->tabBar->onNew = [&] { newTab(current() ? current()->currentDirectory() : std::string()); };

    // Settings ---------------------------------------------------------------------------------------
    auto applyConfig = [&] {
        config->save(prefs);
        for (auto& t : tabs) t->applyConfig();
        page->invalidateLayout();
    };
    auto zoom = [&](float delta) {
        config->fontSize = delta == 0 ? 13.0f : std::clamp(config->fontSize + delta, 8.0f, 36.0f);
        applyConfig();
    };

    Window* settingsWindow = nullptr;
    auto openSettings = [&] {
        if (settingsWindow) {
            settingsWindow->raise();
            return;
        }
        auto& w = app.createWindow({.title = "Terminal Settings", .width = 560, .height = 640, .minWidth = 480, .minHeight = 360});
        auto indexOf = [](const std::vector<std::string>& list, const std::string& v) {
            auto it = std::find(list.begin(), list.end(), v);
            return it == list.end() ? 0 : static_cast<int>(it - list.begin());
        };
        const auto& names = term::profileNames();
        auto profile = Picker(names, indexOf(names, config->profile)).onChange([&, names](int i) {
            config->profile = names[static_cast<size_t>(i)];
            applyConfig();
        });
        // A strip of the profile's colours: paper, ink and the 16 ANSI colours.
        auto swatch = Canvas([config](Painter& p, const Rect& r) {
            const term::Profile pr = term::profileNamed(config->profile, theme().isDark());
            p.fillRoundedRect(r, 6, pr.background, theme().colors.separator, 1);
            Font f{12, FontWeight::Medium, 14};
            p.drawText("Aa", {r.x + 8, r.y, 30, r.height}, f, pr.foreground);
            const float cw = (r.width - 46) / 8;
            for (int i = 0; i < 16; ++i) {
                p.fillRoundedRect({r.x + 40 + static_cast<float>(i % 8) * cw, r.y + 5 + static_cast<float>(i / 8) * (r.height - 10) / 2, cw - 3,
                                   (r.height - 10) / 2 - 2},
                                  2, pr.ansi[static_cast<size_t>(i)]);
            }
        });
        swatch.frame(220, 34);

        std::vector<std::string> fonts{"Default (monospace)"};
        for (auto& f : systemFontFamilies(true)) fonts.push_back(f);
        const int fontIndex = config->fontFamily == "monospace" ? 0 : indexOf(fonts, config->fontFamily);
        auto font = Picker(fonts, fontIndex).onChange([&, fonts](int i) {
            config->fontFamily = i == 0 ? std::string("monospace") : fonts[static_cast<size_t>(i)];
            applyConfig();
        });
        const std::vector<float> sizes{9, 10, 11, 12, 13, 14, 15, 16, 18, 20, 22, 24, 28};
        std::vector<std::string> sizeNames;
        int sizeIndex = 4;
        for (size_t i = 0; i < sizes.size(); ++i) {
            sizeNames.push_back(std::to_string(static_cast<int>(sizes[i])) + " pt");
            if (std::abs(sizes[i] - config->fontSize) < 0.5f) sizeIndex = static_cast<int>(i);
        }
        auto size = Picker(sizeNames, sizeIndex).onChange([&, sizes](int i) {
            config->fontSize = sizes[static_cast<size_t>(i)];
            applyConfig();
        });
        const std::vector<float> spacings{1.0f, 1.1f, 1.2f, 1.3f, 1.4f};
        int spacingIndex = 0;
        for (size_t i = 0; i < spacings.size(); ++i) {
            if (std::abs(spacings[i] - config->lineSpacing) < 0.05f) spacingIndex = static_cast<int>(i);
        }
        auto spacing = Picker({"1.0", "1.1", "1.2", "1.3", "1.4"}, spacingIndex).onChange([&, spacings](int i) {
            config->lineSpacing = spacings[static_cast<size_t>(i)];
            applyConfig();
        });
        auto boldBright = Toggle("", config->boldIsBright).onChange([&](bool on) {
            config->boldIsBright = on;
            applyConfig();
        });
        auto cursor = Picker({"Block", "Underline", "Vertical Bar"}, config->cursorShape).onChange([&](int i) {
            config->cursorShape = i;
            applyConfig();
        });
        auto blink = Toggle("", config->cursorBlink).onChange([&](bool on) {
            config->cursorBlink = on;
            applyConfig();
        });
        const std::vector<int> scrollbacks{1000, 10000, 100000, 1000000};
        int scrollIndex = 1;
        for (size_t i = 0; i < scrollbacks.size(); ++i) {
            if (scrollbacks[i] == config->scrollback) scrollIndex = static_cast<int>(i);
        }
        auto scrollback = Picker({"1,000 lines", "10,000 lines", "100,000 lines", "1,000,000 lines"}, scrollIndex).onChange([&, scrollbacks](int i) {
            config->scrollback = scrollbacks[static_cast<size_t>(i)];
            applyConfig();
        });
        auto exitAction = Picker({"Close the tab", "Close if the shell exited cleanly", "Don't close the tab"},
                                 static_cast<int>(config->onExit))
                              .onChange([&](int i) {
                                  config->onExit = static_cast<term::ExitAction>(i);
                                  applyConfig();
                              });
        auto confirm = Toggle("", config->confirmClose).onChange([&](bool on) {
            config->confirmClose = on;
            applyConfig();
        });
        auto bell = Toggle("", config->visualBell).onChange([&](bool on) {
            config->visualBell = on;
            applyConfig();
        });
        auto form = Form({
            AppearanceSettings(app),
            FormSection("Profile", {FormRow("Colors", profile), FormRow(swatch)}, "“Basic” follows the light or dark appearance."),
            FormSection("Text", {FormRow("Font", font), FormRow("Size", size, "Ctrl+= and Ctrl+- change it too"),
                                 FormRow("Line spacing", spacing), FormRow("Use bright colors for bold text", boldBright)}),
            FormSection("Cursor", {FormRow("Shape", cursor), FormRow("Blink", blink)}),
            FormSection("Shell", {FormRow("Scrollback", scrollback), FormRow("When the shell exits", exitAction),
                                  FormRow("Ask before closing", confirm, "If a program is still running"), FormRow("Visual bell", bell)}),
        });
        w.setContent(ScrollView(VStack({form}).padding(Insets(12, 24, 24, 24))));
        w.onKey([&w](const KeyEvent& e) {
            if (e.key == Key::Escape) {
                w.close();
                return true;
            }
            return false;
        });
        w.onClosed([&] { settingsWindow = nullptr; });
        settingsWindow = &w;
    };

    // Keyboard -------------------------------------------------------------------------------------------
    window.onKey([&](const KeyEvent& e) {
        TerminalView* v = current();
        if (!e.primary()) return false;
        if (e.shift()) {
            switch (e.key) {
                case Key::T: newTab(v ? v->currentDirectory() : std::string()); return true;
                case Key::W: requestCloseTab(active); return true;
                case Key::C: if (v) v->copySelection(); return true;
                case Key::V: if (v) v->paste(); return true;
                case Key::A: if (v) v->selectAll(); return true;
                case Key::K: if (v) v->clearScrollback(); return true;
                case Key::Equals: zoom(1); return true;
                case Key::Tab: selectTab((active + static_cast<int>(tabs.size()) - 1) % static_cast<int>(tabs.size())); return true;
                default: break;
            }
            if (e.key >= Key::Num1 && e.key <= Key::Num9) {
                selectTab(static_cast<int>(e.key) - static_cast<int>(Key::Num1));
                return true;
            }
            return false;
        }
        switch (e.key) {
            case Key::PageUp: selectTab((active + static_cast<int>(tabs.size()) - 1) % static_cast<int>(tabs.size())); return true;
            case Key::PageDown:
            case Key::Tab: selectTab((active + 1) % static_cast<int>(tabs.size())); return true;
            case Key::Equals: zoom(1); return true;
            case Key::Minus: zoom(-1); return true;
            case Key::Num0: zoom(0); return true;
            case Key::Comma: openSettings(); return true;
            default: return false;
        }
    });

    // Dropped files are pasted as shell-quoted paths, like macOS Terminal.
    window.onFilesDropped([&](const std::vector<std::string>& paths) {
        std::string text;
        for (const auto& p : paths) text += term::shellQuote(p) + " ";
        if (TerminalView* v = current()) v->pasteText(text);
    });

    window.setContent(Element(page));
    std::string cwd = argc > 1 ? argv[1] : std::string();
    newTab(cwd);

    // The foreground program changes without telling us; keep titles fresh.
    std::function<void()> poll = [&] {
        refreshTitles();
        app.after(1.0, poll);
    };
    app.after(1.0, poll);

    // MAYO_DEMO=<command> types a command into the first tab (screenshots); "settings" opens the Settings
    // window, "tabs" opens a second tab.
    if (const char* env = std::getenv("MAYO_DEMO")) {
        std::string step = env;
        app.after(0.6, [&, step] {
            if (step == "settings") openSettings();
            else if (step == "tabs") newTab({});
            else if (TerminalView* v = current()) {
                v->pasteText(step);
                v->sendInput("\r");
            }
        });
        // MAYO_DEMO_SCROLL=<points>: afterwards, a touchpad swipe back into the history (smooth scrolling).
        if (const char* scroll = std::getenv("MAYO_DEMO_SCROLL")) {
            const float points = std::strtof(scroll, nullptr);
            app.after(1.5, [&, points] {
                if (TerminalView* v = current()) {
                    const Point at = v->frame().center();
                    v->onScroll(ScrollEvent{at, {0, -points}, true, Modifiers::None});
                }
            });
        }
    }
    return app.run();
}
