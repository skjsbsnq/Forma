#include "../common/zh_cn.hpp"
// Task Manager — an Activity Monitor-style system monitor for Linux built on Mayonnaise.
//
// CPU / Memory / Disk / Network tabs with live history graphs, a sortable process table (click headers,
// drag column edges), search, process filters, an inspector and Quit / Force Quit.
#include <mayonnaise/mayonnaise.hpp>

#include <signal.h>
#include <unistd.h>

#include <atomic>
#include <cerrno>
#include <chrono>
#include <cmath>
#include <condition_variable>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <mutex>
#include <thread>

#include "system_stats.hpp"
#include "widgets.hpp"

using namespace mayo;

namespace {

/// Samples /proc on a background thread and hands snapshots to the UI thread.
class Monitor {
public:
    Monitor(App& app, std::function<void(std::shared_ptr<SystemSnapshot>)> deliver)
        : m_app(app), m_deliver(std::move(deliver)), m_thread([this] { run(); }) {}
    ~Monitor() {
        {
            std::lock_guard lock(m_mutex);
            m_stop = true;
        }
        m_cv.notify_all();
        m_thread.join();
    }
    Monitor(const Monitor&) = delete;
    Monitor& operator=(const Monitor&) = delete;

    void setInterval(double seconds) {
        m_interval = seconds;
        m_cv.notify_all();
    }
    [[nodiscard]] double interval() const { return m_interval; }

private:
    void run() {
        Sampler sampler;
        sampler.sample();  // prime the counters so the first delivered sample has rates
        std::unique_lock lock(m_mutex);
        m_cv.wait_for(lock, std::chrono::milliseconds(400));
        while (!m_stop) {
            lock.unlock();
            auto snapshot = sampler.sample();
            m_app.post([this, snapshot] { m_deliver(snapshot); });
            lock.lock();
            m_cv.wait_for(lock, std::chrono::duration<double>(m_interval.load()), [&] { return m_stop; });
        }
    }

    App& m_app;
    std::function<void(std::shared_ptr<SystemSnapshot>)> m_deliver;
    std::mutex m_mutex;
    std::condition_variable m_cv;
    bool m_stop = false;
    std::atomic<double> m_interval{2.0};
    std::thread m_thread;
};

/// Rounded panel background that follows the theme.
class Card : public Widget {
public:
    explicit Card(const Element& content) {
        setPadding(Insets(12, 14));
        addChild(content.widget());
    }
    void draw(Painter& p) override {
        const Theme& t = theme();
        Color bg = t.isDark() ? Color::white(0.05f) : Color::white(0.7f);
        p.fillRoundedRect(frame(), 12, bg, t.colors.separator.multipliedAlpha(0.7f), 0.5f);
        paintChildren(p);
    }
    [[nodiscard]] bool expandsContent(Axis a) const override {
        return !children().empty() && children().front()->expands(a);
    }
};

Custom<Card> card(const Element& content) {
    auto c = Custom<Card>(content);
    c.fillHeight();  // panels line up: every card spans the panel's height
    return c;
}

/// "● Title ......... value" row; `dot` is drawn when it has a colour.
struct StatRow {
    Label value = Label("—").textAlign(TextAlign::Trailing);
    Element row;
    StatRow(const std::string& title, std::function<Color()> dot = {}) {
        value.font(TextStyle::Callout).weight(FontWeight::Medium);
        std::vector<Element> parts;
        if (dot) {
            parts.push_back(Canvas([dot](Painter& p, const Rect& r) { p.fillCircle(r.center(), 4, dot()); }).frame(10, 10));
        }
        parts.push_back(Label(title).font(TextStyle::Callout).secondary());
        parts.push_back(Spacer());
        parts.push_back(value);
        row = HStack(parts).spacing(6).align(Alignment::Center);
    }
    void set(const std::string& v) {
        if (v != value.text()) value.setText(v);
    }
};

/// White (dark: near-black) rounded surface for the process table; clips the rows to its corners.
class TableCard : public Widget {
public:
    explicit TableCard(const Element& content) { addChild(content.widget()); }
    [[nodiscard]] bool expandsContent(Axis /*axis*/) const override { return true; }
    void draw(Painter& p) override {
        const Theme& t = theme();
        const float radius = 12;
        p.drawShadow(frame(), radius, Color::black(t.isDark() ? 0.3f : 0.06f), 6, {0, 1});
        p.fillRoundedRect(frame(), radius, t.isDark() ? Color::hex(0x1E1E20) : Color::white(), t.colors.separator, 0.5f);
        p.pushClip(frame(), radius);
        paintChildren(p);
        p.popClip();
    }
};

Element caption(const std::string& text) {
    return Label(text).font(TextStyle::Caption).weight(FontWeight::Semibold).tertiary();
}

std::string percent(double fraction) {
    char buf[16];
    std::snprintf(buf, sizeof buf, "%.1f%%", fraction * 100);
    return buf;
}

const char* stateName(char s) {
    switch (s) {
        case 'R': return "Running";
        case 'S': return "Sleeping";
        case 'D': return "Waiting";
        case 'Z': return "Zombie";
        case 'T': return "Stopped";
        case 't': return "Traced";
        case 'I': return "Idle";
        case 'X': return "Dead";
        default: return "Unknown";
    }
}

std::string lower(std::string s) {
    for (auto& ch : s) ch = static_cast<char>(std::tolower(static_cast<unsigned char>(ch)));
    return s;
}

enum Tab { kCpu, kMemory, kDisk, kNetwork };

std::vector<Column> columnsFor(Tab tab, const std::uint64_t& memTotal) {
    auto name = Column{"Process Name", 0, false, [](const ProcessInfo& p) { return p.name; }, {}};
    auto pid = Column{"PID", 72, true, [](const ProcessInfo& p) { return std::to_string(p.pid); },
                      [](const ProcessInfo& p) { return static_cast<double>(p.pid); }};
    auto user = Column{"User", 110, false, [](const ProcessInfo& p) { return p.user; }, {}};
    auto threads = Column{"Threads", 78, true, [](const ProcessInfo& p) { return std::to_string(p.threads); },
                          [](const ProcessInfo& p) { return static_cast<double>(p.threads); }};
    auto cpu = Column{"% CPU", 80, true,
                      [](const ProcessInfo& p) {
                          char b[16];
                          std::snprintf(b, sizeof b, "%.1f", p.cpuPercent);
                          return std::string(b);
                      },
                      [](const ProcessInfo& p) { return p.cpuPercent; }};
    const std::string dash = "—";
    switch (tab) {
        case kCpu:
            return {name, cpu,
                    Column{"CPU Time", 100, true, [](const ProcessInfo& p) { return formatDuration(p.cpuSeconds); },
                           [](const ProcessInfo& p) { return p.cpuSeconds; }},
                    threads,
                    Column{"State", 90, false, [](const ProcessInfo& p) { return mayo::uiText(stateName(p.state)); }, {}},
                    pid, user};
        case kMemory:
            return {name,
                    Column{"Memory", 100, true, [](const ProcessInfo& p) { return formatBytes(static_cast<double>(p.residentBytes)); },
                           [](const ProcessInfo& p) { return static_cast<double>(p.residentBytes); }},
                    Column{"% Memory", 90, true,
                           [&memTotal](const ProcessInfo& p) {
                               char b[16];
                               std::snprintf(b, sizeof b, "%.1f", 100.0 * static_cast<double>(p.residentBytes) / static_cast<double>(std::max<std::uint64_t>(1, memTotal)));
                               return std::string(b);
                           },
                           [](const ProcessInfo& p) { return static_cast<double>(p.residentBytes); }},
                    Column{"Virtual", 100, true, [](const ProcessInfo& p) { return formatBytes(static_cast<double>(p.virtualBytes)); },
                           [](const ProcessInfo& p) { return static_cast<double>(p.virtualBytes); }},
                    threads, pid, user};
        case kDisk:
            return {name,
                    Column{"Read/s", 100, true, [dash](const ProcessInfo& p) { return p.ioKnown ? formatRate(p.readRate) : dash; },
                           [](const ProcessInfo& p) { return p.ioKnown ? p.readRate : -1.0; }},
                    Column{"Write/s", 100, true, [dash](const ProcessInfo& p) { return p.ioKnown ? formatRate(p.writeRate) : dash; },
                           [](const ProcessInfo& p) { return p.ioKnown ? p.writeRate : -1.0; }},
                    Column{"Bytes Read", 100, true,
                           [dash](const ProcessInfo& p) { return p.ioKnown ? formatBytes(static_cast<double>(p.bytesRead)) : dash; },
                           [](const ProcessInfo& p) { return p.ioKnown ? static_cast<double>(p.bytesRead) : -1.0; }},
                    Column{"Bytes Written", 110, true,
                           [dash](const ProcessInfo& p) { return p.ioKnown ? formatBytes(static_cast<double>(p.bytesWritten)) : dash; },
                           [](const ProcessInfo& p) { return p.ioKnown ? static_cast<double>(p.bytesWritten) : -1.0; }},
                    pid, user};
        case kNetwork:
            return {name,
                    Column{"Sockets", 84, true, [dash](const ProcessInfo& p) { return p.sockets >= 0 ? std::to_string(p.sockets) : dash; },
                           [](const ProcessInfo& p) { return static_cast<double>(p.sockets); }},
                    cpu, threads, pid, user};
    }
    return {name};
}

}  // namespace

int main() {
    mayo::setUiTranslator(forma::zhCN);
    App app("task_manager", MAYO_DESKTOP_ID);
    auto& window = app.createWindow(
        {.title = "Task Manager", .width = 1180, .height = 780, .minWidth = 820, .minHeight = 560, .autosaveName = "main"});
    Settings& prefs = app.settings();

    std::shared_ptr<SystemSnapshot> latest;
    Tab tab = kCpu;
    int filter = std::clamp(prefs.getInt("filter"), 0, 3);  // all, mine, others, active
    bool showKernel = prefs.getBool("showKernelThreads");
    std::string query;

    // --- process table ------------------------------------------------------------------------
    auto model = std::make_shared<TableModel>();
    model->columns = columnsFor(kCpu, model->memTotal);
    auto header = std::make_shared<TableHeader>(model);
    auto rows = std::make_shared<TableRows>(model);
    auto rowsScroll = ScrollView(Element(rows));
    rows->setScroller(rowsScroll.node().get());
    auto table = Element(std::make_shared<TableCard>(VStack({Element(header), rowsScroll}).spacing(0)));

    auto rebuildRows = [&] {
        model->rows.clear();
        if (!latest) return;
        model->snapshot = latest;
        model->memTotal = latest->memTotal;
        const std::string q = lower(query);
        for (const auto& p : latest->processes) {
            if (p.isKernelThread && !showKernel) continue;
            if (filter == 1 && p.uid != latest->currentUid) continue;
            if (filter == 2 && p.uid == latest->currentUid) continue;
            if (filter == 3 && p.cpuPercent < 0.05 && p.readRate + p.writeRate <= 0) continue;
            if (tab == kNetwork && p.sockets == 0) continue;
            if (!q.empty() && lower(p.name).find(q) == std::string::npos && lower(p.command).find(q) == std::string::npos &&
                lower(p.user).find(q) == std::string::npos && std::to_string(p.pid).find(q) == std::string::npos)
                continue;
            model->rows.push_back(&p);
        }
        model->sort();
        rows->rowsChanged();
        header->invalidate();
    };
    header->onSortChanged = rebuildRows;
    header->onColumnsResized = [&] { rows->invalidate(); };

    // --- bottom panels -----------------------------------------------------------------------------
    const auto& colors = [] { return theme().colors; };
    // CPU
    StatRow cpuSystem("System", [&] { return colors().red; });
    StatRow cpuUser("User", [&] { return colors().blue; });
    StatRow cpuIdle("Idle");
    StatRow cpuFreq("Frequency");
    StatRow cpuTemp("Temperature");
    StatRow cpuGpu("GPU");
    auto cpuGraph = std::make_shared<HistoryGraph>(HistoryGraph::Mode::Stacked);
    cpuGraph->setFixedMax(1.0);
    cpuGraph->setColorProvider([&] { return std::vector<Color>{colors().red, colors().blue}; });
    auto coreBars = std::make_shared<CoreBars>();
    StatRow procCount("Processes");
    StatRow threadCount("Threads");
    StatRow loadAvg("Load Average");
    StatRow uptime("Uptime");
    auto cpuPanel = HStack({
        card(VStack({caption("CPU"), cpuSystem.row, cpuUser.row, cpuIdle.row, Divider(), cpuFreq.row, cpuTemp.row, cpuGpu.row}).spacing(5)).width(240),
        card(VStack({caption("CPU LOAD"), Element(cpuGraph)}).spacing(6)),
        card(VStack({caption("CORES"), Element(coreBars), Divider(), procCount.row, threadCount.row, loadAvg.row, uptime.row}).spacing(5)).width(280),
    }).spacing(12);

    // Memory
    auto memGraph = std::make_shared<HistoryGraph>(HistoryGraph::Mode::Stacked);
    memGraph->setFixedMax(1.0);
    double pressure = 0;
    auto pressureColor = [&] { return pressure < 0.4 ? colors().green : pressure < 0.75 ? colors().yellow : colors().red; };
    memGraph->setColorProvider([&] { return std::vector<Color>{pressureColor()}; });
    StatRow memPhysical("Physical Memory");
    StatRow memUsed("Memory Used", [&] { return colors().blue; });
    StatRow memCached("Cached Files", [&] { return colors().purple; });
    StatRow memFree("Available");
    StatRow swapUsed("Swap Used", [&] { return colors().orange; });
    StatRow swapActivity("Swap Activity");
    auto memBar = std::make_shared<UsageBar>();
    auto swapBar = std::make_shared<UsageBar>();
    StatRow memPressure("Memory Pressure", pressureColor);
    StatRow legendUsed("Used", [&] { return colors().blue; });
    StatRow legendCached("Cached", [&] { return colors().purple; });
    StatRow legendFree("Free", [&] { return colors().fill.withAlpha(0.6f); });
    StatRow legendSwap("In use");
    auto memPanel = HStack({
        card(VStack({caption("MEMORY PRESSURE"), Element(memGraph), memPressure.row}).spacing(6)),
        card(VStack({caption("MEMORY"), memPhysical.row, memUsed.row, memCached.row, memFree.row, Divider(), swapUsed.row, swapActivity.row}).spacing(5)).width(280),
        card(VStack({caption("USAGE"), Element(memBar), legendUsed.row, legendCached.row, legendFree.row, Spacer(4),
                     caption("SWAP"), Element(swapBar), legendSwap.row}).spacing(6)).width(260),
    }).spacing(12);

    // Disk
    StatRow diskRead("Data read/s", [&] { return colors().blue; });
    StatRow diskWrite("Data written/s", [&] { return colors().red; });
    StatRow diskReadOps("Reads/s");
    StatRow diskWriteOps("Writes/s");
    StatRow diskTotalRead("Total read");
    StatRow diskTotalWrite("Total written");
    auto diskGraph = std::make_shared<HistoryGraph>(HistoryGraph::Mode::Mirrored);
    diskGraph->setMinimumMax(64 * 1024);
    diskGraph->setFormatter([](double v) { return formatRate(v); });
    diskGraph->setColorProvider([&] { return std::vector<Color>{colors().blue, colors().red}; });
    auto volumes = VStack({}).spacing(8);
    auto diskPanel = HStack({
        card(VStack({caption("DISK ACTIVITY"), diskRead.row, diskWrite.row, diskReadOps.row, diskWriteOps.row, Divider(), diskTotalRead.row, diskTotalWrite.row}).spacing(5)).width(260),
        card(VStack({caption("READ ↑ · WRITTEN ↓"), Element(diskGraph)}).spacing(6)),
        card(VStack({caption("VOLUMES"), ScrollView(volumes)}).spacing(6)).width(300),
    }).spacing(12);

    // Network
    StatRow netIn("Received/s", [&] { return colors().blue; });
    StatRow netOut("Sent/s", [&] { return colors().orange; });
    StatRow netPktIn("Packets in/s");
    StatRow netPktOut("Packets out/s");
    StatRow netTotalIn("Total received");
    StatRow netTotalOut("Total sent");
    auto netGraph = std::make_shared<HistoryGraph>(HistoryGraph::Mode::Mirrored);
    netGraph->setMinimumMax(16 * 1024);
    netGraph->setFormatter([](double v) { return formatRate(v); });
    netGraph->setColorProvider([&] { return std::vector<Color>{colors().blue, colors().orange}; });
    auto interfaces = VStack({}).spacing(8);
    auto netPanel = HStack({
        card(VStack({caption("NETWORK"), netIn.row, netOut.row, netPktIn.row, netPktOut.row, Divider(), netTotalIn.row, netTotalOut.row}).spacing(5)).width(260),
        card(VStack({caption("RECEIVED ↑ · SENT ↓"), Element(netGraph)}).spacing(6)),
        card(VStack({caption("INTERFACES"), ScrollView(interfaces)}).spacing(6)).width(300),
    }).spacing(12);

    auto panels = ZStack({cpuPanel, memPanel, diskPanel, netPanel}).height(214);
    auto showPanel = [&] {
        cpuPanel.setHidden(tab != kCpu);
        memPanel.setHidden(tab != kMemory);
        diskPanel.setHidden(tab != kDisk);
        netPanel.setHidden(tab != kNetwork);
    };
    showPanel();

    auto updatePanels = [&](const SystemSnapshot& s) {
        char buf[64];
        cpuSystem.set(percent(s.cpuSystem));
        cpuUser.set(percent(s.cpuUser));
        cpuIdle.set(percent(s.cpuIdle));
        std::snprintf(buf, sizeof buf, "%.2f GHz", s.frequencyMHz / 1000.0);
        cpuFreq.set(s.frequencyMHz > 0 ? buf : "—");
        std::snprintf(buf, sizeof buf, "%.0f °C", s.temperatureC);
        cpuTemp.set(s.temperatureC >= 0 ? buf : "—");
        cpuGpu.set(s.gpuBusy >= 0 ? percent(s.gpuBusy) : "—");
        cpuGraph->push({s.cpuSystem, s.cpuUser});
        coreBars->setCores(s.cores);
        procCount.set(std::to_string(s.processCount));
        threadCount.set(std::to_string(s.threadCount));
        std::snprintf(buf, sizeof buf, "%.2f  %.2f  %.2f", s.loadAverage[0], s.loadAverage[1], s.loadAverage[2]);
        loadAvg.set(buf);
        uptime.set(formatUptime(s.uptime));

        pressure = s.memoryPressure;
        const double total = static_cast<double>(std::max<std::uint64_t>(1, s.memTotal));
        memGraph->push({static_cast<double>(s.memUsed) / total});
        memPressure.set(pressure < 0.4 ? "Normal" : pressure < 0.75 ? "Elevated" : "Critical");
        memPhysical.set(formatBytes(static_cast<double>(s.memTotal)));
        memUsed.set(formatBytes(static_cast<double>(s.memUsed)));
        memCached.set(formatBytes(static_cast<double>(s.memCached)));
        memFree.set(formatBytes(static_cast<double>(s.memAvailable)));
        swapUsed.set(formatBytes(static_cast<double>(s.swapUsed)) + " / " + formatBytes(static_cast<double>(s.swapTotal), 0));
        std::snprintf(buf, sizeof buf, "每秒换入 %.0f · 换出 %.0f", s.swapInRate, s.swapOutRate);
        swapActivity.set(buf);
        memBar->setSegments({{static_cast<double>(s.memUsed) / total, colors().blue},
                             {static_cast<double>(s.memCached) / total, colors().purple}});
        auto pct = [&](std::uint64_t v) { return percent(static_cast<double>(v) / total); };
        legendUsed.set(pct(s.memUsed));
        legendCached.set(pct(s.memCached));
        legendFree.set(pct(s.memTotal - std::min(s.memTotal, s.memUsed + s.memCached)));
        legendSwap.set(s.swapTotal ? percent(static_cast<double>(s.swapUsed) / static_cast<double>(s.swapTotal)) : "No swap");
        swapBar->setSegments({{s.swapTotal ? static_cast<double>(s.swapUsed) / static_cast<double>(s.swapTotal) : 0.0, colors().orange}});

        diskRead.set(formatRate(s.diskReadRate));
        diskWrite.set(formatRate(s.diskWriteRate));
        std::snprintf(buf, sizeof buf, "%.0f", s.diskReadOps);
        diskReadOps.set(buf);
        std::snprintf(buf, sizeof buf, "%.0f", s.diskWriteOps);
        diskWriteOps.set(buf);
        double totalRead = 0;
        double totalWritten = 0;
        for (const auto& d : s.disks) {
            totalRead += static_cast<double>(d.bytesRead);
            totalWritten += static_cast<double>(d.bytesWritten);
        }
        diskTotalRead.set(formatBytes(totalRead));
        diskTotalWrite.set(formatBytes(totalWritten));
        diskGraph->push({s.diskReadRate, s.diskWriteRate});
        volumes.clear();
        for (const auto& fs : s.fileSystems) {
            const double used = fs.total ? 1.0 - static_cast<double>(fs.available) / static_cast<double>(fs.total) : 0.0;
            auto bar = std::make_shared<UsageBar>();
            bar->setSegments({{used, used > 0.9 ? colors().red : colors().accent}});
            volumes.add(VStack({HStack({Label(fs.mountPoint).font(TextStyle::Callout).weight(FontWeight::Medium),
                                        Label(fs.type).font(TextStyle::Caption).tertiary(), Spacer(),
                                        Label(formatBytes(static_cast<double>(fs.available)) + " 可用").font(TextStyle::Caption).secondary()})
                                    .spacing(6)
                                    .align(Alignment::Center),
                                Element(bar),
                                Label(formatBytes(static_cast<double>(fs.total - fs.available)) + " 已用 / " +
                                      formatBytes(static_cast<double>(fs.total)) + " · " + fs.device)
                                    .font(TextStyle::Caption)
                                    .tertiary()
                                    .lineLimit(1)})
                           .spacing(3));
        }
        for (const auto& d : s.disks) {
            volumes.add(Label(d.name + (d.model.empty() ? "" : " · " + d.model)).font(TextStyle::Caption).tertiary().lineLimit(1));
        }

        netIn.set(formatRate(s.netRxRate));
        netOut.set(formatRate(s.netTxRate));
        std::snprintf(buf, sizeof buf, "%.0f", s.netRxPackets);
        netPktIn.set(buf);
        std::snprintf(buf, sizeof buf, "%.0f", s.netTxPackets);
        netPktOut.set(buf);
        double rxTotal = 0;
        double txTotal = 0;
        for (const auto& n : s.interfaces) {
            rxTotal += static_cast<double>(n.rxBytes);
            txTotal += static_cast<double>(n.txBytes);
        }
        netTotalIn.set(formatBytes(rxTotal));
        netTotalOut.set(formatBytes(txTotal));
        netGraph->push({s.netRxRate, s.netTxRate});
        interfaces.clear();
        for (const auto& n : s.interfaces) {
            Icon icon = n.wireless ? Icon::Wifi : Icon::Globe;
            const bool up = n.up;
            auto dot = Canvas([icon, up](Painter& p, const Rect& r) {
                p.drawIcon(icon, r, up ? theme().colors.accent : theme().colors.tertiaryLabel);
            });
            dot.frame(16, 16);
            interfaces.add(HStack({dot,
                                   VStack({Label(n.name).font(TextStyle::Callout).weight(FontWeight::Medium),
                                           Label(n.address.empty() ? (n.up ? "No IPv4 address" : "Disconnected") : n.address)
                                               .font(TextStyle::Caption)
                                               .tertiary()})
                                       .spacing(1),
                                   Spacer(),
                                   VStack({Label("↓ " + formatRate(n.rxRate)).font(TextStyle::Caption).secondary(),
                                           Label("↑ " + formatRate(n.txRate)).font(TextStyle::Caption).secondary()})
                                       .spacing(1)
                                       .align(Alignment::TopTrailing)})
                               .spacing(8)
                               .align(Alignment::Center));
        }
    };

    // --- inspector & quit -------------------------------------------------------------------------
    auto stopButton = Button(Icon::Xmark).style(ButtonStyle::Borderless).tooltip("Quit Process (Delete)").disabled();
    auto infoButton = Button(Icon::Info).style(ButtonStyle::Borderless).tooltip("Inspect Process (Enter)").disabled();

    pid_t inspected = -1;
    auto inspName = Label("").font(TextStyle::Title3).lineLimit(1).maxWidth(380);
    std::vector<std::pair<std::string, Label>> inspRows;
    for (const char* key : {"PID", "Parent", "User", "State", "Threads", "% CPU", "CPU Time", "Memory", "Virtual Memory",
                            "Disk Read", "Disk Written", "Sockets", "Running For"}) {
        inspRows.emplace_back(key, Label("—").font(TextStyle::Callout));
    }
    auto inspCommand = Label("").font(TextStyle::Subheadline).secondary().maxWidth(380);
    auto inspGrid = VStack({}).spacing(3);
    for (auto& [key, value] : inspRows) {
        inspGrid.add(HStack({Label(key).font(TextStyle::Callout).secondary().width(110), value}).spacing(8));
    }
    auto inspector = Popover(VStack({inspName, Divider(), inspGrid, Divider(), caption("COMMAND"), inspCommand}).spacing(8).width(400));
    auto refreshInspector = [&] {
        if (!latest || inspected < 0) return;
        const ProcessInfo* p = nullptr;
        const ProcessInfo* parent = nullptr;
        for (const auto& proc : latest->processes) {
            if (proc.pid == inspected) p = &proc;
        }
        if (p) {
            for (const auto& proc : latest->processes) {
                if (proc.pid == p->ppid) parent = &proc;
            }
        }
        if (!p) {
            inspName.setText(inspName.text().find("（已退出）") == std::string::npos ? inspName.text() + "（已退出）" : inspName.text());
            return;
        }
        char buf[64];
        inspName.setText(p->name);
        std::snprintf(buf, sizeof buf, "%.1f", p->cpuPercent);
        const double running = std::max(0.0, latest->uptime - p->startedAt);
        const std::string values[] = {
            std::to_string(p->pid),
            parent ? parent->name + " (" + std::to_string(parent->pid) + ")" : std::to_string(p->ppid),
            p->user + " (" + std::to_string(p->uid) + ")",
            stateName(p->state),
            std::to_string(p->threads),
            buf,
            formatDuration(p->cpuSeconds),
            formatBytes(static_cast<double>(p->residentBytes)),
            formatBytes(static_cast<double>(p->virtualBytes)),
            p->ioKnown ? formatBytes(static_cast<double>(p->bytesRead)) : "Not permitted",
            p->ioKnown ? formatBytes(static_cast<double>(p->bytesWritten)) : "Not permitted",
            p->sockets >= 0 ? std::to_string(p->sockets) : "Not permitted",
            formatUptime(running),
        };
        for (size_t i = 0; i < inspRows.size(); ++i) {
            if (inspRows[i].second.text() != values[i]) inspRows[i].second.setText(values[i]);
        }
        inspCommand.setText(p->isKernelThread ? "Kernel thread" : p->command.empty() ? "—" : p->command);
    };
    auto inspect = [&](pid_t pid) {
        inspected = pid;
        inspName.setText("");
        refreshInspector();
        inspector.show(infoButton);
    };

    pid_t quitting = -1;
    auto quitQuestion = Label("").font(TextStyle::Headline).maxWidth(330);
    auto quitError = Label("").foreground(Color::hex(0xFF3B30)).maxWidth(330).hidden();
    auto quitCancel = Button("Cancel");
    auto quitForce = Button("Force Quit").style(ButtonStyle::Destructive);
    auto quitButton = Button("Quit").style(ButtonStyle::Prominent);
    auto quitPopover = Popover(VStack({
                                          quitQuestion,
                                          Label("Quit asks the process to exit (SIGTERM). Force Quit stops it immediately "
                                                "(SIGKILL); unsaved work is lost.")
                                              .secondary()
                                              .font(TextStyle::Subheadline)
                                              .maxWidth(330),
                                          quitError,
                                          HStack({Spacer(), quitCancel, quitForce, quitButton}).spacing(8),
                                      })
                                   .spacing(10)
                                   .width(350));
    auto askQuit = [&](pid_t pid, bool force) {
        const ProcessInfo* p = nullptr;
        for (const auto* row : model->rows) {
            if (row->pid == pid) p = row;
        }
        if (!p) return;
        quitting = pid;
        quitQuestion.setText("确定要" + std::string(force ? "强制结束" : "结束") + "进程“" + p->name + "”吗？");
        quitError.setHidden(true);
        quitPopover.show(stopButton);
    };
    auto sendSignal = [&](int sig) {
        if (quitting <= 0) return;
        if (::kill(quitting, sig) == 0 || errno == ESRCH) {
            quitPopover.dismiss();
            return;
        }
        quitError.setText(errno == EPERM ? "You don't have permission to quit this process. It belongs to another user."
                                         : std::string("无法发送信号：") + std::strerror(errno));
        quitError.setHidden(false);
    };
    quitCancel.onClick([&] { quitPopover.dismiss(); });
    quitButton.onClick([&] { sendSignal(SIGTERM); });
    quitForce.onClick([&] { sendSignal(SIGKILL); });

    rows->onSelect = [&](pid_t pid) {
        stopButton.setDisabled(pid <= 0);
        infoButton.setDisabled(pid <= 0);
    };
    rows->onOpen = inspect;
    rows->onQuit = askQuit;
    stopButton.onClick([&] { askQuit(model->selected, false); });
    infoButton.onClick([&] { inspect(model->selected); });

    // --- toolbar -------------------------------------------------------------------------------
    auto filterPicker = Picker({"All Processes", "My Processes", "Other Users", "Active Processes"}, filter).onChange([&](int i) {
        filter = i;
        prefs.set("filter", i);
        rebuildRows();
    });
    std::function<void()> syncSettings;  // keeps an open Settings window in step
    std::unique_ptr<Monitor> monitor;
    std::function<void()> openSettings;
    Menu viewMenu("View", {});
    std::function<void()> rebuildViewMenu = [&] {
        auto every = [&](double seconds, const char* label) {
            MenuItem item(label, [&, seconds] {
                monitor->setInterval(seconds);
                prefs.set("interval", seconds);
                rebuildViewMenu();
                if (syncSettings) syncSettings();
            });
            item.checked = monitor && std::abs(monitor->interval() - seconds) < 0.01;
            return item;
        };
        std::vector<MenuItem> items{every(1, "Update Every Second"), every(2, "Update Every 2 Seconds"),
                                    every(5, "Update Every 5 Seconds"), MenuItem::separator()};
        MenuItem kernel("Show Kernel Threads", [&] {
            showKernel = !showKernel;
            prefs.set("showKernelThreads", showKernel);
            rebuildViewMenu();
            rebuildRows();
            if (syncSettings) syncSettings();
        });
        kernel.checked = showKernel;
        items.push_back(kernel);
        items.push_back(MenuItem::separator());
        items.push_back(MenuItem("Settings…", [&] { openSettings(); }).withIcon(Icon::Gear).withShortcut("Ctrl+,"));
        viewMenu.setItems(std::move(items));
    };

    auto tabs = SegmentedControl({"CPU", "Memory", "Disk", "Network"}).onChange([&](int i) {
        tab = static_cast<Tab>(i);
        prefs.set("tab", i);
        model->columns = columnsFor(tab, model->memTotal);
        model->sortColumn = 1;
        model->descending = true;
        showPanel();
        rebuildRows();
    });
    window.setToolbar(Toolbar({
        ToolbarGroup({stopButton, infoButton}),
        Spacer(),
        tabs,
        Spacer(),
        filterPicker,
        viewMenu,
        SearchField("Search").width(170).onChange([&](const std::string& q) {
            query = q;
            rebuildRows();
        }),
    }));

    window.setContent(VStack({table, panels}).spacing(12).padding(Insets(0, 12, 12, 12)));
    window.onKey([&](const KeyEvent& e) {
        if (e.primary() && e.key >= Key::Num1 && e.key <= Key::Num4) {
            int i = static_cast<int>(e.key) - static_cast<int>(Key::Num1);
            tabs->setSelected(i, true);
            return true;
        }
        if (e.primary() && e.key == Key::Comma) {
            openSettings();
            return true;
        }
        if (e.primary() && e.key == Key::F) {
            return true;  // search field focus would go here; Tab reaches it too
        }
        return false;
    });

    // --- data flow ----------------------------------------------------------------------------------
    monitor = std::make_unique<Monitor>(app, [&](std::shared_ptr<SystemSnapshot> snapshot) {
        latest = std::move(snapshot);
        rebuildRows();
        updatePanels(*latest);
        if (inspector.isShown()) refreshInspector();
        stopButton.setDisabled(model->selected <= 0);
        infoButton.setDisabled(model->selected <= 0);
    });
    monitor->setInterval(std::clamp(prefs.getDouble("interval", 2.0), 1.0, 5.0));

    // Settings window (Ctrl+,) ------------------------------------------------------------------------
    Window* settingsWindow = nullptr;
    openSettings = [&] {
        if (settingsWindow) {
            settingsWindow->raise();
            return;
        }
        auto& w = app.createWindow({.title = "Task Manager Settings", .width = 540, .height = 470, .minWidth = 460, .minHeight = 320});
        constexpr double kIntervals[] = {1, 2, 5};
        auto intervalIndex = [&] {
            const double now = monitor->interval();
            return now < 1.5 ? 0 : now < 3.5 ? 1 : 2;
        };
        auto interval = Picker({"Every Second", "Every 2 Seconds", "Every 5 Seconds"}, intervalIndex()).onChange([&](int i) {
            monitor->setInterval(kIntervals[i]);
            prefs.set("interval", kIntervals[i]);
            rebuildViewMenu();
        });
        auto kernel = Toggle("", showKernel).onChange([&](bool on) {
            showKernel = on;
            prefs.set("showKernelThreads", on);
            rebuildViewMenu();
            rebuildRows();
        });
        auto form = Form({
            AppearanceSettings(app),
            FormSection("Monitoring", {FormRow("Update frequency", interval, "Sampling more often costs a little CPU"),
                                       FormRow("Show kernel threads", kernel)}),
        });
        w.setContent(ScrollView(VStack({form}).padding(Insets(12, 24, 24, 24))));
        w.onKey([&w](const KeyEvent& e) {
            if (e.key == Key::Escape || (e.primary() && e.key == Key::W)) {
                w.close();
                return true;
            }
            return false;
        });
        syncSettings = [&, interval, kernel, intervalIndex]() mutable {
            interval->setSelected(intervalIndex());
            kernel.isOn(showKernel);
        };
        w.onClosed([&] {
            settingsWindow = nullptr;
            syncSettings = nullptr;
        });
        settingsWindow = &w;
    };
    rebuildViewMenu();
    if (const int saved = std::clamp(prefs.getInt("tab"), 0, 3); saved != 0) tabs->setSelected(saved, true);

    // MAYO_DEMO=memory|disk|network selects a tab at startup (screenshots); inspect opens the inspector;
    // settings opens the Settings window.
    if (const char* env = std::getenv("MAYO_DEMO")) {
        std::string demo = env;
        app.after(0.2, [&, demo] {
            if (demo == "memory") tabs->setSelected(1, true);
            if (demo == "disk") tabs->setSelected(2, true);
            if (demo == "network") tabs->setSelected(3, true);
        });
        if (demo == "settings") app.after(0.3, [&] { openSettings(); });
        if (demo == "inspect" || demo == "quit")
            app.after(2.6, [&, demo] {
                if (model->rows.empty()) return;
                rows->select(model->rows.front()->pid, true);
                demo == "inspect" ? inspect(model->selected) : askQuit(model->selected, false);
            });
    }
    const int code = app.run();
    monitor.reset();  // stop sampling before the widgets it posts to go away
    return code;
}
