// Forma App Store — a Flatpak app store with the macOS 26 App Store layout.
//
//   forma-app-store [--demo <page>]
//
// The sidebar lists editor's sections (Discover, Create, Work, Play, Develop, Learn, Utilities) plus
// Categories, Updates and Installed; the detail side is a page of icon cards, editorial cards and app
// pages with screenshots. Data comes from Flatpak's AppStream cache, so the whole catalogue is browsable
// offline; installs and updates run `flatpak` and stream their progress into the cards.
#include <mayonnaise/mayonnaise.hpp>

#include <algorithm>
#include <atomic>
#include <cstdio>
#include <cstdlib>
#include <cctype>
#include <cstring>
#include <deque>
#include <memory>
#include <mutex>
#include <string>
#include <thread>
#include <vector>

#include "catalog.hpp"
#include "images.hpp"
#include "store_ui.hpp"

using namespace mayo;
using namespace forma::store;

namespace {

constexpr float kSidebarWidth = 226;

/// Detail container: the page scrolls under the toolbar glass, like the App Store's content area.
/// A dismissible strip at the bottom reports failed installs and Flatpak errors.
class StorePageView : public Widget {
public:
    StorePageView() {
        m_scroll = std::make_shared<ScrollViewView>(std::make_shared<Widget>());
        addChild(m_scroll);
    }

    void setMessage(std::string message) {
        m_message = std::move(message);
        invalidateLayout();
    }


    [[nodiscard]] bool extendsIntoTitleBar() const override { return true; }
    [[nodiscard]] bool expandsContent(Axis /*axis*/) const override { return true; }

    void setPage(const std::shared_ptr<Widget>& page) {
        m_scroll->setContent(page);
        m_scroll->scrollTo({0, 0}, false);
        // The outer scroll view must re-measure now that its content changed.
        invalidateLayout();
    }

    [[nodiscard]] ScrollViewView* scroll() const { return m_scroll.get(); }

    void arrangeContent(const Rect& content) override {
        const float bar = host() ? host()->titleBarHeight() : 0.0f;
        m_scroll->setContentInsetTop(bar);
        const Rect body = m_message.empty() ? content
                                            : Rect{content.x, content.y, content.width, content.height - 52};
        m_scroll->measure(body.size());
        m_scroll->arrange(body);
    }
    void draw(Painter& p) override {
        const auto& c = theme().colors;
        p.fillRect(frame(), theme().isDark() ? c.windowBackground : Color::hex(0xFFFFFF));
        paintChildren(p);
        // Soft edge under the toolbar so scrolling content fades behind the glass.
        const float bar = host() ? host()->titleBarHeight() : 0.0f;
        const Rect f = frame();
        p.fillRect({f.x, f.y + bar - 1, f.width, 1.0f / p.scale()}, c.separator.multipliedAlpha(0.5f));
        if (m_message.empty()) return;
        const Rect banner = messageRect();
        p.fillRoundedRect(banner, 10, c.destructive.multipliedAlpha(0.10f));
        p.strokeRoundedRect(banner, 10, c.destructive.multipliedAlpha(0.35f), 0.5f);
        p.drawIcon(Icon::Info, {banner.x + 12, banner.center().y - 8, 16, 16}, c.destructive);
        p.pushClip({banner.x + 36, banner.y, banner.width - 72, banner.height});
        p.drawText(m_message, {banner.x + 36, banner.y, banner.width - 72, banner.height},
                   theme().font(TextStyle::Subheadline), c.label);
        p.popClip();
        p.drawIcon(Icon::Xmark, {banner.right() - 26, banner.center().y - 7, 14, 14}, c.secondaryLabel);
    }
    [[nodiscard]] Rect messageRect() const {
        const Rect f = frame();
        return {f.x + 24, f.bottom() - 46, f.width - 48, 36};
    }
    bool onPointerDown(const PointerEvent& e) override {
        if (!m_message.empty() && messageRect().contains(e.position)) {
            m_message.clear();
            invalidateLayout();
            return true;
        }
        return false;
    }
    [[nodiscard]] bool isInteractive() const override { return !m_message.empty(); }

private:
    std::shared_ptr<ScrollViewView> m_scroll;
    std::string m_message;
};

/// Localises one line of flatpak's progress output ("Installing com.x.Y: 63%" -> "正在安装 63%").
std::string stageText(const std::string& line) {
    std::string text = line;
    while (!text.empty() && (text.back() == '\r' || text.back() == '\n' || text.back() == ' ')) text.pop_back();
    const std::size_t cr = text.find_last_of('\r');
    if (cr != std::string::npos) text = text.substr(cr + 1);
    auto replaceAll = [&text](std::string_view from, std::string_view to) {
        for (std::size_t at = text.find(from); at != std::string::npos; at = text.find(from, at + to.size()))
            text.replace(at, from.size(), to);
    };
    replaceAll("Installing", "正在安装");
    replaceAll("Downloading", "正在下载");
    replaceAll("Fetching", "正在获取");
    replaceAll("Updating", "正在更新");
    replaceAll("Uninstalling", "正在卸载");
    replaceAll("runtime", "运行时");
    // "1/7" is flatpak's progress through several refs (the app plus its runtimes); it already says
    // where the install is, so no percentage is appended later.
    const std::size_t slash = text.find('/');
    if (slash != std::string::npos && text.find("正在") != std::string::npos &&
        std::isdigit(static_cast<unsigned char>(text[slash + 1])))
        text = text.substr(0, slash) + " " + text.substr(slash);
    // Drop the app id ("正在安装 com.x.Y: 52%"): the page already says which app is being installed.
    const std::size_t colon = text.find(':');
    if (colon != std::string::npos && text.find("正在") == 0) {
        std::string head = text.substr(0, colon);
        const std::size_t space = head.rfind(' ');
        if (space != std::string::npos) head.resize(space);
        text = head + text.substr(colon + 1);
    }
    // Trim by characters (never through a UTF-8 sequence); the chip ellipsises what still doesn't fit.
    std::size_t chars = 0;
    for (std::size_t at = 0; at < text.size(); ++at) {
        if ((static_cast<unsigned char>(text[at]) & 0xC0) != 0x80 && ++chars > 18) {
            text.resize(at);
            break;
        }
    }
    return text;
}

int lastPercent(const std::string& chunk) {
    int percent = -1;
    for (std::size_t i = 0; i < chunk.size(); ++i) {
        if (chunk[i] != '%') continue;
        std::size_t j = i;
        int value = 0;
        int digits = 0;
        while (j > 0 && chunk[j - 1] >= '0' && chunk[j - 1] <= '9' && digits < 3) {
            value += static_cast<int>(chunk[j - 1] - '0') * (digits == 0 ? 1 : digits == 1 ? 10 : 100);
            --j;
            ++digits;
        }
        if (digits > 0) percent = value;
    }
    return percent;
}

/// Turns flatpak's stderr into one readable line for the banner.
std::string describeFailure(const std::string& verb, const std::string& output) {
    std::string line;
    std::size_t start = 0;
    while (start <= output.size()) {
        const std::size_t nl = output.find('\n', start);
        std::string candidate = output.substr(start, nl == std::string::npos ? std::string::npos : nl - start);
        while (!candidate.empty() && (candidate.back() == '\r' || candidate.back() == ' ')) candidate.pop_back();
        if (candidate.find("error:") != std::string::npos || line.empty()) {
            if (!candidate.empty()) line = candidate;
            if (candidate.find("error:") != std::string::npos) break;
        }
        if (nl == std::string::npos) break;
        start = nl + 1;
    }
    if (line.empty()) line = "Flatpak 未能完成操作。";
    if (line.size() > 160) line.resize(160);
    // "正在安装" -> "安装失败"，so the banner reads naturally.
    std::string action = verb;
    if (action.rfind("正在", 0) == 0) action = action.substr(6);
    return action + "失败：" + line;
}

class StoreApp {
public:
    StoreApp(App& app, Window& window) : m_app(app), m_window(window), m_model() {
        m_model.images.setOnLoaded([this] {
            m_window.raise();
            if (m_page) m_page->scroll()->invalidate();
        });
        m_model.callbacks.openApp = [this](const std::string& id) { openApp(id); };
        m_model.callbacks.openSection = [this](const std::string& id) { showSection(id); };
        m_model.callbacks.openCategory = [this](const std::string& id) { showCategory(id); };
        m_model.callbacks.install = [this](const std::string& id) { install(id); };
        m_model.callbacks.update = [this](const std::string& id) { update(id); };
        m_model.callbacks.updateAll = [this] { update({}); };
        m_model.callbacks.launch = [this](const std::string& id) { launch(id); };
        m_model.callbacks.uninstall = [this](const std::string& id) { uninstall(id); };
        m_model.callbacks.copyText = [this](std::string text) {
            if (m_search.node()->host()) m_search.node()->host()->setClipboardText(text);
        };
        buildUi();
    }

    ~StoreApp() {
        m_stopping = true;
        if (m_catalogThread.joinable()) m_catalogThread.join();
        if (m_installThread.joinable()) m_installThread.join();
        if (m_updateThread.joinable()) m_updateThread.join();
    }

    void start() {
        m_catalogThread = std::thread([this] {
            Catalog catalog = loadCatalog();
            m_app.post([this, catalog = std::move(catalog)]() mutable {
                m_model.catalog = std::move(catalog);
                m_model.offline = false;
                m_model.updateCount = static_cast<int>(m_model.catalog.updates().size());
                refreshChrome();
                if (!m_shownAnything) showSection("discover");
                else reloadCurrent();
                m_catalogLoaded = true;
                runPendingDemo();
            });
        });
        m_updateThread = std::thread([this] {
            const auto updates = queryUpdates();
            m_app.post([this, updates] {
                if (m_simulatedUpdates) return;
                std::set<std::string> available(updates.begin(), updates.end());
                for (auto& app : m_model.catalog.apps) app.updateAvailable = available.count(app.id) > 0;
                m_model.updateCount = static_cast<int>(updates.size());
                refreshChrome();
                if (m_current == "updates" || m_current == "installed") reloadCurrent();
            });
        });
    }

    bool onKey(const KeyEvent& e) {
        if (e.key == Key::F && e.primary()) {
            if (m_search.node()->host()) m_search.node()->host()->setFocus(m_search.node().get(), true);
            return true;
        }
        if (e.key == Key::R && e.primary()) {
            reload();
            return true;
        }
        if (e.key == Key::Comma && e.primary()) {
            showSection("settings");
            return true;
        }
        if (e.key == Key::Left && e.alt()) {
            back();
            return true;
        }
        return false;
    }

    void runDemo(const std::string& steps) {
        std::size_t start = 0;
        while (start <= steps.size()) {
            const std::size_t comma = steps.find(',', start);
            const std::string step = steps.substr(start, comma == std::string::npos ? std::string::npos : comma - start);
            if (!step.empty()) m_demoSteps.push_back(step);
            if (comma == std::string::npos) break;
            start = comma + 1;
        }
        runPendingDemo();
    }

    /// Runs queued demo steps once the catalogue is in (the first page needs real data).
    void runPendingDemo() {
        if (m_ready) return;
        if (m_model.catalog.apps.empty() && !m_catalogLoaded) return;
        m_ready = true;
        for (std::size_t i = 0; i < m_demoSteps.size(); ++i) {
            const std::string step = m_demoSteps[i];
            m_app.after(0.05 * static_cast<double>(i + 1), [this, step] { demoStep(step); });
        }
        m_demoSteps.clear();
    }

private:
    // --- UI --------------------------------------------------------------------------------------

    void buildUi() {
        m_window.setTitleVisible(false);
        m_detail = std::make_shared<StorePageView>();
        m_page = m_detail;

        m_search = SearchField("搜索应用");
        m_search.height(28);
        m_search.node()->setOnChange([this](const std::string& value) { search(value); });
        m_search.node()->setOnSubmit([this](const std::string& value) { search(value); });
        m_sidebarBody = sidebarBody(m_model, "discover", [this](const std::string& id) { select(id); }, Element(m_search));
        m_sidebar = m_sidebarBody;
        m_split = std::make_shared<SplitViewView>(m_sidebar, m_detail);
        m_split->setSidebarWidth(kSidebarWidth);
        m_split->setSidebarLimits(200, 280);
        m_window.setContent(Element(m_split));

        m_back = Button(Icon::ChevronLeft).style(ButtonStyle::Borderless).tooltip("后退 (⌥←)");
        m_back.setDisabled(true);
        m_refresh = Button(Icon::Repeat).style(ButtonStyle::Borderless).tooltip("重新载入目录 (Ctrl+R)");
        m_title = Label("精选").font(TextStyle::Headline);
        m_account = Button(Icon::PersonCircle).style(ButtonStyle::Glass).tooltip("账户");
        m_account.onClick([this] { showSection("settings"); });
        m_back.onClick([this] { back(); });
        m_refresh.onClick([this] { reload(); });
        auto toolbar = Toolbar({m_back, m_title, Spacer(), m_refresh, m_account}).spacing(10);
        auto group = ToolbarGroup({Button(Icon::Import).style(ButtonStyle::Borderless).tooltip("获取更新").onClick([this] {
            showSection("updates");
        })});
        (void)group;
        m_window.setToolbar(toolbar);
        m_window.setMinimumSize({860, 520});
        m_window.onKey([this](const KeyEvent& e) { return onKey(e); });
    }

    void refreshChrome() {
        m_model.updateCount = static_cast<int>(m_model.catalog.updates().size());
        if (m_sidebarBody) m_sidebarBody->invalidate();
        if (m_detail) m_detail->scroll()->invalidate();
        m_back.setDisabled(m_history.empty());
    }

    /// Rebuilds the current page from the model (icons, install states).
    void reloadCurrent() {
        const std::string current = m_current;
        if (current == "discover") showSection("discover", false);
        else if (current == "updates") showSection("updates", false);
        else if (current == "installed") showSection("installed", false);
        else if (current == "categories") showSection("categories", false);
        else if (current.rfind("app:", 0) == 0) openApp(current.substr(4), false);
        else if (current.rfind("category:", 0) == 0) showCategory(current.substr(9), false);
        else if (current.rfind("search:", 0) == 0) navigate("search:" + m_lastQuery, false);
        else if (current == "settings") showSection("settings", false);
        else showSection(current.empty() ? "discover" : current, false);
    }

    void pushHistory(const std::string& state) {
        if (!m_current.empty() && m_current != state) m_history.push_back(m_current);
        if (m_history.size() > 32) m_history.pop_front();
        m_current = state;
        m_back.setDisabled(m_history.empty());
    }

    void back() {
        if (m_history.empty()) return;
        const std::string state = m_history.back();
        m_history.pop_back();
        m_current.clear();
        navigate(state, false, true);
    }

    void select(const std::string& id) {
        if (id == "settings") {
            showSection("settings");
            return;
        }
        showSection(id);
    }

    void navigate(const std::string& state, bool record, bool fromHistory = false) {
        (void)fromHistory;
        if (!record) m_current = state;
        const std::string previous = m_current;
        if (record) pushHistory(state);
        else m_current = state;
        std::string title = "精选";
        if (state == "discover") {
            m_page->setPage(discoverPage(m_model));
            title = "精选";
        } else if (state == "updates") {
            m_page->setPage(updatesPage(m_model));
            title = "更新";
        } else if (state == "installed") {
            m_page->setPage(installedPage(m_model));
            title = "已安装";
        } else if (state == "categories") {
            m_page->setPage(categoriesPage(m_model));
            title = "类别";
        } else if (state == "settings") {
            m_page->setPage(settingsPage(m_model));
            title = "设置";
        } else if (state.rfind("app:", 0) == 0) {
            const std::string id = state.substr(4);
            m_page->setPage(appPage(m_model, id));
            const AppEntry* app = m_model.find(id);
            title = app ? app->name : "应用";
        } else if (state.rfind("category:", 0) == 0) {
            const std::string cat = state.substr(9);
            m_page->setPage(categoryPage(m_model, cat));
            title = categoryName(cat);
        } else if (state.rfind("search:", 0) == 0) {
            const std::string query = state.substr(7);
            m_page->setPage(searchPage(m_model, query));
            title = query.empty() ? "搜索" : "搜索 “" + query + "”";
        } else {
            // An editorial section.
            const Section* s = section(state);
            m_page->setPage(sectionPage(m_model, state));
            title = s ? s->title : state;
        }
        m_title.setText(title);
        if (m_detail) m_detail->setMessage({});
        m_window.setTitle(title + " — Forma 应用商店");
        m_window.setTitle(title);
        m_shownAnything = true;
        if (m_sidebarBody) {
            std::string highlight = state;
            if (state.rfind("app:", 0) == 0) highlight = m_previousSection.empty() ? "discover" : m_previousSection;
            else if (state.rfind("search:", 0) == 0) highlight = "discover";
            else if (state.rfind("category:", 0) == 0) highlight = "categories";
            if (highlight != "settings") {
                sidebarSelect(*m_sidebarBody, highlight);
                m_previousSection = highlight;
            }
        }
        refreshChrome();
        (void)previous;
    }

    void showSection(const std::string& id, bool record = true) { navigate(id, record); }
    void showCategory(const std::string& id, bool record = true) {
        navigate("category:" + id, record);
    }
    void openApp(const std::string& id, bool record = true) { navigate("app:" + id, record); }

    void search(const std::string& query, bool record = true) {
        m_lastQuery = query;
        if (query.empty()) {
            if (m_current.rfind("search:", 0) == 0) navigate("search:", record);
            return;
        }
        navigate("search:" + query, record);
    }

    // --- jobs ------------------------------------------------------------------------------------

    void install(const std::string& id) {
        const AppEntry* app = m_model.find(id);
        if (!app) return;
        std::string remote = app->origin;
        if (remote.empty() && !m_model.catalog.remotes.empty()) remote = m_model.catalog.remotes.front();
        if (remote.empty()) remote = "flathub";
        startJob(id, {"flatpak", "install", "--assumeyes", remote, id}, "正在安装");
    }
    void update(const std::string& id) {
        if (id.empty()) {
            startJob("", {"flatpak", "update", "--assumeyes"}, "正在更新全部应用");
        } else {
            startJob(id, {"flatpak", "update", "--assumeyes", id}, "正在更新");
        }
    }
    void uninstall(const std::string& id) {
        startJob(id, {"flatpak", "uninstall", "--assumeyes", id}, "正在卸载");
    }

    void launch(const std::string& id) {
        if (!validAppId(id)) return;
        launchDetached({"flatpak", "run", id});
    }

    void startJob(const std::string& id, std::vector<std::string> argv, const std::string& verb) {
        if (m_jobRunning) return;
        if (!id.empty()) {
            m_model.busy.insert(id);
            m_model.broken.erase(id);
            m_model.progress[id] = -1;
            m_model.stage[id] = verb + "…";
            m_model.installing = id;
        }
        m_jobRunning = true;
        refreshChrome();
        reloadCurrent();
        if (m_installThread.joinable()) m_installThread.join();
        m_installThread = std::thread([this, id, argv = std::move(argv), verb] {
            std::string tail;
            const CommandResult result =
                run(argv, 0, [this, id, verb](std::string_view chunk) {
                    const int percent = lastPercent(std::string(chunk));
                    const std::string stage = stageText(std::string(chunk));
                    m_app.post([this, id, percent, stage, verb] {
                        if (id.empty()) return;
                        if (percent >= 0) m_model.progress[id] = static_cast<float>(percent) / 100.0f;
                        if (!stage.empty()) m_model.stage[id] = stage;
                        (void)verb;
                        m_detail->scroll()->invalidate();
                    });
                });
            const bool ok = result.exitCode == 0;
            m_app.post([this, id, ok, output = result.output, verb] {
                m_jobRunning = false;
                if (!id.empty()) {
                    m_model.busy.erase(id);
                    m_model.stage.erase(id);
                    m_model.progress.erase(id);
                    if (!ok) m_model.broken.insert(id);
                }
                (void)output;
                (void)verb;
                // Re-read installed refs (fast) and refresh what is on screen.
                for (auto& installed : listInstalled()) {
                    auto* app = m_model.findMutable(installed.id);
                    if (app) {
                        app->installed = true;
                        app->installedVersion = installed.installedVersion;
                        app->installation = installed.installation;
                    }
                }
                m_model.installing.clear();
                refreshChrome();
                reloadCurrent();
                if (!ok && !id.empty()) m_detail->setMessage(describeFailure(verb, output));
                else m_detail->setMessage({});
            });
        });
    }

    void reload() {
        if (m_catalogThread.joinable()) m_catalogThread.join();
        m_catalogThread = std::thread([this] {
            Catalog catalog = loadCatalog();
            m_app.post([this, catalog = std::move(catalog)]() mutable {
                const std::string current = m_current;
                m_model.catalog = std::move(catalog);
                refreshChrome();
                // Keep the page the user was on.
                m_current.clear();
                navigate(current.empty() ? "discover" : current, false);
            });
        });
    }

    void demoStep(const std::string& step) {
        (void)0;
        [this, step] {
            if (step == "discover") showSection("discover");
            else if (step == "create") showSection("create");
            else if (step == "work") showSection("work");
            else if (step == "play") showSection("play");
            else if (step == "develop") showSection("develop");
            else if (step == "learn") showSection("learn");
            else if (step == "utilities") showSection("utilities");
            else if (step == "categories") showSection("categories");
            else if (step == "updates") showSection("updates");
            else if (step == "installed") showSection("installed");
            else if (step == "settings") showSection("settings");
            else if (step == "hero") {
                showSection("discover");
                m_model.heroIndex = 1;
            } else if (step.rfind("search:", 0) == 0) {
                const std::string query = step.substr(7);
                m_search.setText(query);
                search(query);
            } else if (step.rfind("app:", 0) == 0) {
                openApp(step.substr(4));
            } else if (step.rfind("category:", 0) == 0) {
                showCategory(step.substr(9));
            } else if (step.rfind("install:", 0) == 0) {
                install(step.substr(8));
            } else if (step.rfind("updateapp:", 0) == 0) {
                update(step.substr(10));
            } else if (step == "simupdate") {
                // Demo only: pretend the installed apps have newer versions so the Updates page can be
                // checked without touching the real installation.
                for (auto& app : m_model.catalog.apps) {
                    if (!app.installed) continue;
                    if (app.installedVersion.empty()) app.installedVersion = "1.0.0";
                    if (app.version.empty() || app.version == app.installedVersion) app.version = "1.1.0";
                    app.updateAvailable = true;
                }
                m_simulatedUpdates = true;
                m_model.updateCount = static_cast<int>(m_model.catalog.updates().size());
                showSection("updates");
            } else if (step == "updateall") {
                update({});
            } else if (step.rfind("scroll:", 0) == 0) {
                const float y = static_cast<float>(std::atof(step.substr(7).c_str()));
                m_detail->scroll()->scrollTo({0, y}, false);
            } else if (step.rfind("wait:", 0) == 0) {
                // handled by the caller's screenshot delay
            }
        }();
    }

    App& m_app;
    Window& m_window;
    StoreModel m_model;
    std::shared_ptr<StorePageView> m_detail;
    std::shared_ptr<StorePageView> m_page;
    std::shared_ptr<SplitViewView> m_split;
    std::shared_ptr<Widget> m_sidebar;
    std::shared_ptr<Widget> m_sidebarBody;
    TextField m_search;
    Button m_back{Icon::ChevronLeft}, m_refresh{Icon::Repeat}, m_account{Icon::PersonCircle};
    Label m_title;
    std::deque<std::string> m_history;
    std::string m_current;
    std::string m_previousSection = "discover";
    std::string m_lastQuery;
    std::thread m_catalogThread;
    std::thread m_installThread;
    std::thread m_updateThread;
    std::atomic<bool> m_stopping{false};
    bool m_jobRunning = false;
    bool m_shownAnything = false;
    bool m_ready = false;
    std::vector<std::string> m_demoSteps;
    bool m_simulatedUpdates = false;
    bool m_catalogLoaded = false;
};

}  // namespace

int main(int argc, char** argv) {
    (void)argc;
    (void)argv;
    App app("app_store", MAYO_DESKTOP_ID);
    auto& window = app.createWindow(
        {.title = "Forma 应用商店", .width = 1180, .height = 764, .minWidth = 900, .minHeight = 560,
         .autosaveName = "main"});
    StoreApp store(app, window);
    if (const char* demo = std::getenv("MAYO_DEMO")) store.runDemo(demo);
    store.start();
    return app.run();
}
