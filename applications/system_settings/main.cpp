#include "../common/zh_cn.hpp"
// Settings — a System Settings-style control centre for GNOME and KDE Plasma (and other GNU/Linux desktops
// where the services exist) built on Mayonnaise.
//
// Everything is live: panels read and write the same GSettings keys and D-Bus services as GNOME Settings
// (NetworkManager, BlueZ, UPower, power-profiles-daemon, Mutter's DisplayConfig, AccountsService,
// systemd's timedated / localed / hostnamed, PipeWire/PulseAudio via pactl), and follow changes made
// elsewhere while open. Under Plasma (XDG_CURRENT_DESKTOP=KDE, or SETTINGS_DESKTOP=kde) the desktop panels
// use KConfig files, KWin, KScreen and PowerDevil instead (kconfig.hpp, panels_kde.cpp).
//
//     system_settings [panel]          e.g. `system_settings wifi`
//     MAYO_DEMO=<panel>[:<action>] MAYO_SCREENSHOT=/tmp/s.png system_settings
//       actions: wifi:other, displays:keep, appearance:fonts, keyboard:add, keyboard:shortcut, datetime:zones
//
// Screenshot runs (MAYO_SCREENSHOT) and SETTINGS_READ_ONLY=1 never change the system.
#include <mayonnaise/mayonnaise.hpp>

#include <cstdlib>
#include <map>
#include <vector>

#include "glib_support.hpp"
#include "panels.hpp"
#include "ui.hpp"

using namespace mayo;

int main(int argc, char** argv) {
    mayo::setUiTranslator(forma::zhCN);
    App app("system-settings", MAYO_DESKTOP_ID);
    auto& prefs = app.settings();
    gx::setReadOnly(std::getenv("MAYO_SCREENSHOT") || std::getenv("SETTINGS_READ_ONLY"));
    gx::start(app);

    auto& window = app.createWindow({.title = "Settings",
                                     .width = 920,
                                     .height = 680,
                                     .minWidth = 720,
                                     .minHeight = 460,
                                     .autosaveName = "main"});
    window.setTitleVisible(false);  // the toolbar shows the panel title after the navigation buttons

    auto titleLabel = Label("").font(TextStyle::Title3).lineLimit(1);
    auto backButton = Button(Icon::ChevronLeft).style(ButtonStyle::Borderless).tooltip("Back (Alt+←)").disabled();
    auto forwardButton =
        Button(Icon::ChevronRight).style(ButtonStyle::Borderless).tooltip("Forward (Alt+→)").disabled();

    auto search = SearchField("Search").padding(Insets(0, 10, 8, 10));
    auto sidebar = Sidebar(std::vector<SidebarItem>{}).selectionStyle(SidebarSelectionStyle::Accent).header(search);
    auto placeholder = VStack({});
    auto split = SplitView(sidebar, placeholder).sidebarWidth(230).sidebarLimits(190, 320);

    std::map<std::string, Element> built;
    std::vector<std::string> back;
    std::vector<std::string> forward;
    std::string current;
    std::string query;

    Context ctx{app, window, {}, titleLabel, {}, {}};

    auto rebuildSidebar = [&] {
        std::vector<SidebarItem> items;
        const std::string q = ui::lower(query);
        int group = -1;
        for (const auto& p : panels()) {
            if (!q.empty() && ui::lower(mayo::uiText(p.title)).find(q) == std::string::npos &&
                std::string(p.keywords).find(q) == std::string::npos)
                continue;
            // Groups are separated by an untitled header (a gap), like System Settings.
            if (q.empty() && group != -1 && p.group != group) items.push_back(SidebarItem::header(""));
            group = p.group;
            items.push_back(SidebarItem(p.id, p.icon, p.title).withTile(theme().colors.*p.tint));
        }
        sidebar.setItems(std::move(items));
        sidebar.selection(current);
    };

    auto show = [&](const std::string& id, bool record) {
        const PanelInfo* info = findPanel(id);
        if (!info || id == current) return;
        if (record && !current.empty()) {
            back.push_back(current);
            forward.clear();
        }
        current = id;
        auto it = built.find(id);
        if (it == built.end()) it = built.emplace(id, info->build(ctx)).first;
        split.setDetail(it->second);
        titleLabel.setText(info->title);
        window.setTitle(std::string(info->title) + " — Forma 系统设置");
        sidebar.selection(id);
        backButton.setDisabled(back.empty());
        forwardButton.setDisabled(forward.empty());
        prefs.set("panel", id);
        auto hooks = ctx.onShown.equal_range(id);
        for (auto h = hooks.first; h != hooks.second; ++h) h->second();
    };
    ctx.show = [&](const std::string& id) { show(id, true); };
    auto goBack = [&] {
        if (back.empty()) return;
        forward.push_back(current);
        std::string id = back.back();
        back.pop_back();
        show(id, false);
    };
    auto goForward = [&] {
        if (forward.empty()) return;
        back.push_back(current);
        std::string id = forward.back();
        forward.pop_back();
        show(id, false);
    };
    backButton.onClick(goBack);
    forwardButton.onClick(goForward);
    sidebar.onSelect([&](const std::string& id) { show(id, true); });
    search.onChange([&](const std::string& q) {
        query = q;
        rebuildSidebar();
    });
    search.onSubmit([&](const std::string&) {
        // Enter opens the first match.
        const std::string q = ui::lower(query);
        for (const auto& p : panels()) {
            if (ui::lower(mayo::uiText(p.title)).find(q) != std::string::npos || std::string(p.keywords).find(q) != std::string::npos) {
                show(p.id, true);
                return;
            }
        }
    });

    window.setToolbar(Toolbar({ToolbarGroup({backButton, forwardButton}), titleLabel, Spacer()}));
    window.setContent(split);
    window.onKey([&](const KeyEvent& e) {
        if (e.alt() && e.key == Key::Left) return goBack(), true;
        if (e.alt() && e.key == Key::Right) return goForward(), true;
        if (e.primary() && e.key == Key::F) {
            if (auto* h = search->host()) h->setFocus(search.node().get(), true);
            return true;
        }
        if (e.primary() && e.key == Key::Q) return app.quit(), true;
        return false;
    });

    rebuildSidebar();
    // Start-up panel: command line (like gnome-control-center), MAYO_DEMO, then the last one shown.
    std::string first = prefs.getString("panel", "wifi");
    if (argc > 1 && findPanel(argv[1])) first = argv[1];
    if (const char* demo = std::getenv("MAYO_DEMO")) {
        const std::string d = demo;
        const std::string panel = d.substr(0, d.find(':'));
        if (findPanel(panel)) first = panel;
        if (d.find(':') != std::string::npos) ctx.demo = d.substr(d.find(':') + 1);
    }
    show(findPanel(first) ? first : "wifi", false);

    const int code = app.run();
    gx::stop();
    return code;
}
