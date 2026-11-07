// Displays (Mutter's DisplayConfig or KDE's KScreen, logind brightness, Night Light), Sound (PipeWire /
// PulseAudio through pactl) and Power (UPower, power-profiles-daemon, gnome-settings-daemon's power plugin or
// PowerDevil).
#include <algorithm>
#include <cmath>
#include <cstdio>
#include <filesystem>
#include <fstream>
#include <map>
#include <optional>
#include <set>

#include "glib_support.hpp"
#include "json.hpp"
#include "kconfig.hpp"
#include "panels.hpp"
#include "ui.hpp"

using namespace mayo;
namespace fs = std::filesystem;

// Displays ------------------------------------------------------------------------------------------------

namespace {

constexpr const char* kMutterDisplay = "org.gnome.Mutter.DisplayConfig";
constexpr const char* kMutterDisplayPath = "/org/gnome/Mutter/DisplayConfig";

struct DisplayMode {
    std::string id;
    int width = 0;
    int height = 0;
    double refresh = 0;
    double preferredScale = 1;
    std::vector<double> scales;
    bool current = false;
    bool preferred = false;
    bool vrr = false;
};

struct DisplayMonitor {
    std::string connector;
    std::string name;
    bool builtin = false;
    std::vector<DisplayMode> modes;
};

struct LogicalMonitor {
    int x = 0;
    int y = 0;
    double scale = 1;
    unsigned transform = 0;
    bool primary = false;
    std::vector<std::string> connectors;
};

struct DisplayState {
    unsigned serial = 0;
    unsigned layoutMode = 1;  // 1 = logical (sizes are divided by the scale), 2 = physical
    std::vector<DisplayMonitor> monitors;
    std::vector<LogicalMonitor> logical;
};

/// What the user picked for one monitor.
struct MonitorChoice {
    std::string modeId;
    double scale = 1;
    unsigned transform = 0;
    bool operator==(const MonitorChoice& o) const {
        return modeId == o.modeId && std::abs(scale - o.scale) < 0.001 && transform == o.transform;
    }
};

std::optional<DisplayState> readDisplayState() {
    auto reply = gx::callSync(gx::sessionBus(), kMutterDisplay, kMutterDisplayPath, kMutterDisplay, "GetCurrentState", nullptr,
                              "(ua((ssss)a(siiddada{sv})a{sv})a(iiduba(ssss)a{sv})a{sv})");
    if (!reply) return std::nullopt;
    DisplayState s;
    GVariant* monitors = nullptr;
    GVariant* logical = nullptr;
    GVariant* props = nullptr;
    g_variant_get(reply.get(), "(u@a((ssss)a(siiddada{sv})a{sv})@a(iiduba(ssss)a{sv})@a{sv})", &s.serial, &monitors, &logical, &props);
    gx::Variant m(monitors);
    gx::Variant l(logical);
    gx::Variant p(props);
    s.layoutMode = static_cast<unsigned>(gx::lookupInt(props, "layout-mode", 1));

    GVariantIter it;
    g_variant_iter_init(&it, monitors);
    const char* conn = nullptr;
    const char* vendor = nullptr;
    const char* product = nullptr;
    const char* serial = nullptr;
    GVariant* modes = nullptr;
    GVariant* mprops = nullptr;
    while (g_variant_iter_next(&it, "((&s&s&s&s)@a(siiddada{sv})@a{sv})", &conn, &vendor, &product, &serial, &modes, &mprops)) {
        gx::Variant mv(modes);
        gx::Variant pv(mprops);
        DisplayMonitor mon;
        mon.connector = conn;
        mon.builtin = gx::lookupBool(mprops, "is-builtin");
        mon.name = gx::lookupString(mprops, "display-name", std::string(vendor) + " " + product);
        GVariantIter mi;
        g_variant_iter_init(&mi, modes);
        const char* id = nullptr;
        gint32 w = 0;
        gint32 h = 0;
        double refresh = 0;
        double pref = 1;
        GVariant* scales = nullptr;
        GVariant* modeProps = nullptr;
        while (g_variant_iter_next(&mi, "(&siidd@ad@a{sv})", &id, &w, &h, &refresh, &pref, &scales, &modeProps)) {
            gx::Variant sv(scales);
            gx::Variant mp(modeProps);
            DisplayMode mode;
            mode.id = id;
            mode.width = w;
            mode.height = h;
            mode.refresh = refresh;
            mode.preferredScale = pref;
            gsize n = 0;
            const auto* d = static_cast<const double*>(g_variant_get_fixed_array(scales, &n, sizeof(double)));
            mode.scales.assign(d, d + n);
            mode.current = gx::lookupBool(modeProps, "is-current");
            mode.preferred = gx::lookupBool(modeProps, "is-preferred");
            mode.vrr = gx::lookupString(modeProps, "refresh-rate-mode") == "variable";
            mon.modes.push_back(std::move(mode));
        }
        s.monitors.push_back(std::move(mon));
    }

    g_variant_iter_init(&it, logical);
    gint32 x = 0;
    gint32 y = 0;
    double scale = 1;
    guint32 transform = 0;
    gboolean primary = FALSE;
    GVariant* lmons = nullptr;
    GVariant* lprops = nullptr;
    while (g_variant_iter_next(&it, "(iidub@a(ssss)@a{sv})", &x, &y, &scale, &transform, &primary, &lmons, &lprops)) {
        gx::Variant lm(lmons);
        gx::Variant lp(lprops);
        LogicalMonitor lmon{x, y, scale, transform, primary != FALSE, {}};
        GVariantIter ci;
        g_variant_iter_init(&ci, lmons);
        const char* c = nullptr;
        while (g_variant_iter_next(&ci, "(&s&s&s&s)", &c, nullptr, nullptr, nullptr)) lmon.connectors.emplace_back(c);
        s.logical.push_back(std::move(lmon));
    }
    return s;
}


// KDE: KScreen, through kscreen-doctor (its JSON dump to read, output.<name>.<setting> arguments to apply).

/// Whether kscreen-doctor knows output priorities (Plasma 5.27+) instead of a single primary flag.
bool g_kscreenPriority = true;

/// Monitor name from the EDID's display name descriptor, empty if there is none.
std::string edidName(const std::string& connector) {
    std::error_code ec;
    for (const auto& card : fs::directory_iterator("/sys/class/drm", ec)) {
        const std::string n = card.path().filename().string();
        if (n.size() <= connector.size() || n.compare(n.size() - connector.size(), connector.size(), connector) != 0 ||
            n[n.size() - connector.size() - 1] != '-')
            continue;
        std::ifstream in(card.path() / "edid", std::ios::binary);
        std::string edid((std::istreambuf_iterator<char>(in)), std::istreambuf_iterator<char>());
        for (size_t d = 54; d + 18 <= edid.size() && d < 126; d += 18) {
            if (edid[d] != 0 || edid[d + 1] != 0 || static_cast<unsigned char>(edid[d + 3]) != 0xFC) continue;
            std::string name = edid.substr(d + 5, 13);
            name = name.substr(0, name.find('\n'));
            return ui::trim(name);
        }
    }
    return {};
}

std::optional<DisplayState> readKScreenState() {
    if (!gx::haveProgram("kscreen-doctor")) return std::nullopt;
    auto r = gx::runSync({"kscreen-doctor", "-j"});
    if (!r.ok()) return std::nullopt;
    const auto doc = json::Value::parse(r.out);
    const auto& outputs = doc["outputs"];
    if (outputs.type() != json::Value::Type::Array) return std::nullopt;
    auto idText = [](const json::Value& v) {
        if (v.type() == json::Value::Type::String) return v.string();
        return std::to_string(static_cast<long long>(v.number(-1)));
    };
    DisplayState s;
    s.layoutMode = 1;  // KScreen positions are logical
    for (const auto& o : outputs.array()) {
        if (!o["connected"].boolean(true)) continue;
        DisplayMonitor mon;
        mon.connector = o["name"].str();
        const std::string c = mon.connector;
        mon.builtin = o["type"].number() == 7 || c.rfind("eDP", 0) == 0 || c.rfind("LVDS", 0) == 0 || c.rfind("DSI", 0) == 0;
        mon.name = edidName(c);
        if (mon.name.empty()) mon.name = mon.builtin ? "Built-in Display" : c;
        const std::string current = idText(o["currentModeId"]);
        std::set<std::string> preferred;
        if (o["preferredModes"].type() == json::Value::Type::Array) {
            for (const auto& p : o["preferredModes"].array()) preferred.insert(idText(p));
        }
        const double scale = o["scale"].number(1);
        std::vector<double> scales{1, 1.25, 1.5, 1.75, 2, 2.25, 2.5, 2.75, 3};
        if (std::none_of(scales.begin(), scales.end(), [&](double x) { return std::abs(x - scale) < 0.01; })) {
            scales.push_back(scale);
            std::sort(scales.begin(), scales.end());
        }
        if (o["modes"].type() == json::Value::Type::Array) {
            for (const auto& m : o["modes"].array()) {
                DisplayMode mode;
                mode.id = idText(m["id"]);
                mode.width = static_cast<int>(m["size"]["width"].number());
                mode.height = static_cast<int>(m["size"]["height"].number());
                mode.refresh = m["refreshRate"].number();
                mode.scales = scales;
                // Plasma recommends 100% below ~1.5x the DPI of a typical desktop display; keep it simple.
                mode.preferredScale = mon.builtin && mode.width >= 2560 ? 1.5 : 1.0;
                mode.current = mode.id == current;
                mode.preferred = preferred.contains(mode.id);
                if (mode.width > 0) mon.modes.push_back(std::move(mode));
            }
        }
        if (o["enabled"].boolean(true)) {
            g_kscreenPriority = !o["priority"].isNull();
            const bool primary = g_kscreenPriority ? o["priority"].number() == 1 : o["primary"].boolean();
            // KScreen rotation flags: 1 none, 2 left (90° counter-clockwise), 4 inverted, 8 right.
            const int rot = static_cast<int>(o["rotation"].number(1));
            const unsigned transform = rot == 2 ? 1 : rot == 4 ? 2 : rot == 8 ? 3 : 0;
            s.logical.push_back({static_cast<int>(o["pos"]["x"].number()), static_cast<int>(o["pos"]["y"].number()), scale,
                                 transform, primary, {c}});
        }
        s.monitors.push_back(std::move(mon));
    }
    return s;
}

std::optional<DisplayState> readState() { return desk::kde() ? readKScreenState() : readDisplayState(); }

const DisplayMonitor* monitorFor(const DisplayState& s, const std::string& connector) {
    for (const auto& m : s.monitors) {
        if (m.connector == connector) return &m;
    }
    return nullptr;
}

const DisplayMode* modeFor(const DisplayMonitor& m, const std::string& id) {
    for (const auto& mode : m.modes) {
        if (mode.id == id) return &mode;
    }
    return nullptr;
}

std::string aspect(int w, int h) {
    const double r = static_cast<double>(w) / std::max(1, h);
    struct Ratio {
        double value;
        const char* name;
    };
    for (const Ratio& a : {Ratio{16.0 / 9, "16:9"}, {16.0 / 10, "16:10"}, {4.0 / 3, "4:3"}, {5.0 / 4, "5:4"}, {21.0 / 9, "21:9"},
                           {32.0 / 9, "32:9"}, {3.0 / 2, "3:2"}}) {
        if (std::abs(r - a.value) < 0.02) return a.name;
    }
    return {};
}

std::string scaleText(double s) {
    char buf[16];
    std::snprintf(buf, sizeof buf, "%.0f%%", s * 100);
    return buf;
}

class DisplaysPanel {
    struct KeepPrompt;

public:
    DisplaysPanel(App& app, Element anchor) : m_app(app), m_anchor(std::move(anchor)) {
        if (desk::kde()) {
            gx::subscribe(gx::sessionBus(), nullptr, "org.kde.kscreen.Backend", "configChanged", nullptr,
                          [this](const std::string&, const std::string&, GVariant*) { m_kscreenChanged(); });
        } else {
            gx::subscribe(gx::sessionBus(), kMutterDisplay, kMutterDisplay, "MonitorsChanged", kMutterDisplayPath,
                          [this](const std::string&, const std::string&, GVariant*) { reload(); });
        }
    }

    std::vector<Element> sections() {
        std::vector<Element> out{m_body};
        if (auto b = brightnessSection()) out.push_back(b);
        out.push_back(desk::kde() ? kde::nightLightSection() : nightLightSection());
        return out;
    }

    void reload() {
        auto s = readState();
        if (!s) {
            m_body.clear();
            m_body.add(ui::section({}, {FormRow(Label(desk::kde() ? "Display settings need KScreen (kscreen-doctor)."
                                                                  : "Display settings need GNOME's compositor (Mutter).")
                                                    .secondary())}));
            return;
        }
        m_state = std::move(*s);
        m_choices.clear();
        m_primary.clear();
        for (const auto& lm : m_state.logical) {
            for (const auto& c : lm.connectors) {
                const DisplayMonitor* mon = monitorFor(m_state, c);
                std::string current;
                if (mon) {
                    for (const auto& mode : mon->modes) {
                        if (mode.current) current = mode.id;
                    }
                }
                m_choices[c] = {current, lm.scale, lm.transform};
                if (lm.primary) m_primary = c;
            }
        }
        m_original = m_choices;
        m_originalPrimary = m_primary;
        if (!m_choices.contains(m_selected)) m_selected = m_primary.empty() && !m_choices.empty() ? m_choices.begin()->first : m_primary;
        rebuild();
    }

    /// The "Keep these display settings?" prompt without changing anything (screenshots).
    void demoKeepPrompt() {
        auto st = std::make_shared<KeepPrompt>();
        st->decided = true;  // nothing to revert
        confirmKeepWith(st);
    }

    void refreshBrightness() {
        if (m_backlight.empty()) return;
        m_brightness->setValue(readBrightness());
    }

private:
    // Monitor configuration ------------------------------------------------------------------------------

    struct LogicalConfig {
        int x, y;
        double scale;
        unsigned transform;
        bool primary;
        std::vector<std::pair<std::string, std::string>> monitors;  // connector, mode id
    };

    /// Logical layout for the given choices. Sizes change with the mode and scale, so monitors in a plain
    /// row (or column) are re-packed edge to edge; other arrangements keep their positions.
    std::vector<LogicalConfig> layout(const std::map<std::string, MonitorChoice>& choices, const std::string& primary) const {
        std::vector<LogicalConfig> out;
        bool row = true;
        bool column = true;
        for (const auto& lm : m_state.logical) {
            row = row && lm.y == 0;
            column = column && lm.x == 0;
        }
        for (const auto& lm : m_state.logical) {
            LogicalConfig c{lm.x, lm.y, lm.scale, lm.transform, false, {}};
            for (const auto& conn : lm.connectors) {
                auto it = choices.find(conn);
                if (it == choices.end()) continue;
                c.monitors.emplace_back(conn, it->second.modeId);
                c.scale = it->second.scale;
                c.transform = it->second.transform;
                if (conn == primary) c.primary = true;
            }
            out.push_back(std::move(c));
        }
        auto size = [&](const LogicalConfig& c) {
            const DisplayMonitor* mon = c.monitors.empty() ? nullptr : monitorFor(m_state, c.monitors[0].first);
            const DisplayMode* mode = mon ? modeFor(*mon, c.monitors[0].second) : nullptr;
            double w = mode ? mode->width : 0;
            double h = mode ? mode->height : 0;
            if (c.transform % 2 == 1) std::swap(w, h);
            if (m_state.layoutMode == 1) {
                w = std::round(w / c.scale);
                h = std::round(h / c.scale);
            }
            return std::pair{static_cast<int>(w), static_cast<int>(h)};
        };
        if (out.size() > 1 && (row || column)) {
            std::vector<LogicalConfig*> order;
            for (auto& c : out) order.push_back(&c);
            std::sort(order.begin(), order.end(), [row](auto* a, auto* b) { return row ? a->x < b->x : a->y < b->y; });
            int pos = 0;
            for (auto* c : order) {
                (row ? c->x : c->y) = pos;
                pos += row ? size(*c).first : size(*c).second;
            }
        }
        return out;
    }

    /// KScreen applies and stores in one go, so "temporary" (method 1) and "persistent" (2) are the same call
    /// and the keep prompt reverts by applying the previous layout again.
    void applyKScreen(const std::vector<LogicalConfig>& config, unsigned method, std::function<void(const std::string&)> done) {
        if (method == 2) return done({});
        std::vector<std::string> argv{"kscreen-doctor"};
        for (const auto& c : config) {
            for (const auto& [conn, mode] : c.monitors) {
                const std::string o = "output." + conn + ".";
                char scale[32];
                std::snprintf(scale, sizeof scale, "%g", c.scale);
                static const char* kRotation[] = {"normal", "left", "inverted", "right"};
                argv.push_back(o + "mode." + mode);
                argv.push_back(o + "scale." + scale);
                argv.push_back(o + "rotation." + kRotation[c.transform % 4]);
                argv.push_back(o + "position." + std::to_string(c.x) + "," + std::to_string(c.y));
                if (c.primary) argv.push_back(o + (g_kscreenPriority ? "priority.1" : "primary"));
            }
        }
        gx::run(argv, [done](const gx::ProcessResult& r) {
            done(r.ok() ? std::string() : ui::trim(r.err.empty() ? r.out : r.err));
        });
    }

    void applyConfig(const std::vector<LogicalConfig>& config, unsigned method, std::function<void(const std::string&)> done) {
        if (desk::kde()) return applyKScreen(config, method, std::move(done));
        auto fresh = readDisplayState();
        if (!fresh) return done("The display configuration service went away.");
        GVariantBuilder lms;
        g_variant_builder_init(&lms, G_VARIANT_TYPE("a(iiduba(ssa{sv}))"));
        for (const auto& c : config) {
            GVariantBuilder mons;
            g_variant_builder_init(&mons, G_VARIANT_TYPE("a(ssa{sv})"));
            for (const auto& [conn, mode] : c.monitors) {
                g_variant_builder_add(&mons, "(ss@a{sv})", conn.c_str(), mode.c_str(),
                                      g_variant_new_array(G_VARIANT_TYPE("{sv}"), nullptr, 0));
            }
            g_variant_builder_add(&lms, "(iiduba(ssa{sv}))", c.x, c.y, c.scale, c.transform, static_cast<gboolean>(c.primary),
                                  &mons);
        }
        // Read-only runs only ask Mutter to validate the configuration (method 0).
        if (gx::readOnly()) method = 0;
        gx::call(gx::sessionBus(), kMutterDisplay, kMutterDisplayPath, kMutterDisplay, "ApplyMonitorsConfig",
                 g_variant_new("(uua(iiduba(ssa{sv}))@a{sv})", fresh->serial, method, &lms,
                               g_variant_new_array(G_VARIANT_TYPE("{sv}"), nullptr, 0)),
                 [done](gx::Variant, const std::string& error) { done(error); });
    }

    [[nodiscard]] bool changed() const { return m_choices != m_original || m_primary != m_originalPrimary; }

    /// Applies temporarily, then asks to keep the change; it reverts by itself after 20 seconds, so a mode
    /// the screen can't show is never permanent.
    void apply() {
        const auto previous = layout(m_original, m_originalPrimary);
        applyConfig(layout(m_choices, m_primary), 1, [this, previous](const std::string& error) {
            if (!error.empty()) {
                ui::message(m_anchor, "Couldn't apply the display settings", error);
                return;
            }
            if (gx::readOnly()) return;
            confirmKeep(previous);
        });
    }


    void confirmKeep(std::vector<LogicalConfig> previous) {
        auto st = std::make_shared<KeepPrompt>();
        st->previous = std::move(previous);
        confirmKeepWith(st);
    }

    void confirmKeepWith(const std::shared_ptr<KeepPrompt>& st) {
        auto keep = Button("Keep Changes").style(ButtonStyle::Prominent).onClick([this, st] { keepChanges(st); });
        auto revertButton = Button("Revert Settings").onClick([this, st] { revert(st); });
        auto pop = Popover(VStack({Label("Keep these display settings?").font(TextStyle::Headline), st->label,
                                   HStack({Spacer(), revertButton, keep}).spacing(8)})
                               .spacing(8)
                               .width(330));
        st->popover = pop.node();
        pop.onDismiss([this, st] { revert(st); });  // Escape or a click outside means "no"
        pop.show(m_anchor);
        m_app.after(1.0, [this, st] { tick(st); });
    }

    void tick(const std::shared_ptr<KeepPrompt>& st) {
        if (st->decided) return;
        if (--st->seconds <= 0) return revert(st);
        st->label.setText("Reverting in " + std::to_string(st->seconds) + " seconds.");
        m_app.after(1.0, [this, st] { tick(st); });
    }

    void revert(const std::shared_ptr<KeepPrompt>& st) {
        if (st->decided) return;
        st->decided = true;
        if (auto p = st->popover.lock()) p->dismiss();
        applyConfig(st->previous, 1, [this](const std::string& e) {
            if (!e.empty()) ui::message(m_anchor, "Couldn't restore the display settings", e);
        });
    }

    void keepChanges(const std::shared_ptr<KeepPrompt>& st) {
        st->decided = true;
        if (auto p = st->popover.lock()) p->dismiss();
        // Make what is on screen now the stored configuration (monitors.xml).
        auto s = readState();
        if (!s) return;
        m_state = *s;
        std::map<std::string, MonitorChoice> now;
        std::string primary;
        for (const auto& lm : m_state.logical) {
            for (const auto& c : lm.connectors) {
                const DisplayMonitor* mon = monitorFor(m_state, c);
                std::string mode;
                if (mon) {
                    for (const auto& md : mon->modes) {
                        if (md.current) mode = md.id;
                    }
                }
                now[c] = {mode, lm.scale, lm.transform};
                if (lm.primary) primary = c;
            }
        }
        applyConfig(layout(now, primary), 2, [](const std::string&) {});
    }

    // UI ---------------------------------------------------------------------------------------------

    Element arrangement() {
        auto canvas = Canvas([this](Painter& p, const Rect& r) {
            const auto& c = theme().colors;
            const auto cfg = layout(m_choices, m_primary);
            float minX = 1e9f, minY = 1e9f, maxX = -1e9f, maxY = -1e9f;
            std::vector<std::pair<Rect, std::string>> rects;
            for (const auto& lc : cfg) {
                if (lc.monitors.empty()) continue;
                const DisplayMonitor* mon = monitorFor(m_state, lc.monitors[0].first);
                const DisplayMode* mode = mon ? modeFor(*mon, lc.monitors[0].second) : nullptr;
                if (!mode) continue;
                float w = static_cast<float>(mode->width);
                float h = static_cast<float>(mode->height);
                if (lc.transform % 2 == 1) std::swap(w, h);
                if (m_state.layoutMode == 1) {
                    w /= static_cast<float>(lc.scale);
                    h /= static_cast<float>(lc.scale);
                }
                Rect rr{static_cast<float>(lc.x), static_cast<float>(lc.y), w, h};
                rects.emplace_back(rr, lc.monitors[0].first);
                minX = std::min(minX, rr.x);
                minY = std::min(minY, rr.y);
                maxX = std::max(maxX, rr.right());
                maxY = std::max(maxY, rr.bottom());
            }
            if (rects.empty()) return;
            const float k = std::min((r.width - 40) / (maxX - minX), (r.height - 30) / (maxY - minY));
            const float ox = r.x + (r.width - (maxX - minX) * k) * 0.5f;
            const float oy = r.y + (r.height - (maxY - minY) * k) * 0.5f;
            m_hitRects.clear();
            for (const auto& [rr, conn] : rects) {
                const Rect d{ox + (rr.x - minX) * k + 2, oy + (rr.y - minY) * k + 2, rr.width * k - 4, rr.height * k - 4};
                const bool sel = conn == m_selected;
                p.fillRoundedRect(d, 6, sel ? c.accent.withAlpha(0.22f) : c.secondaryFill, sel ? c.accent : c.separator, sel ? 2.0f : 1.0f);
                const DisplayMonitor* mon = monitorFor(m_state, conn);
                p.drawText(mon ? mon->name : conn, d, theme().font(TextStyle::Subheadline), c.label, TextAlign::Center);
                if (conn == m_primary) p.fillRoundedRect({d.x + 8, d.y + 8, d.width * 0.3f, 4}, 2, c.secondaryLabel);
                m_hitRects.emplace_back(Rect{d.x - r.x, d.y - r.y, d.width, d.height}, conn);
            }
        });
        canvas.height(170).onClick([this](Point local) {
            // Canvas callbacks get local points; the hit rects are stored relative to the canvas too.
            for (const auto& [rect, conn] : m_hitRects) {
                if (rect.contains(local)) {
                    m_selected = conn;
                    rebuild();
                    return;
                }
            }
        });
        return FormRow(canvas);
    }

    void rebuild() {
        m_body.clear();
        if (m_choices.empty()) {
            m_body.add(ui::section({}, {FormRow(Label("No displays are turned on.").secondary())}));
            return;
        }
        std::vector<Element> top;
        if (m_choices.size() > 1) {
            top.push_back(arrangement());
            std::vector<std::string> names;
            std::vector<std::string> conns;
            int primaryIndex = 0;
            for (const auto& [conn, _] : m_choices) {
                const DisplayMonitor* mon = monitorFor(m_state, conn);
                if (conn == m_primary) primaryIndex = static_cast<int>(conns.size());
                names.push_back(mon ? mon->name : conn);
                conns.push_back(conn);
            }
            top.push_back(FormRow("Primary Display", Picker(names, primaryIndex).onChange([this, conns](int i) {
                m_primary = conns[static_cast<size_t>(i)];
                rebuild();
            })));
            std::vector<std::string> selNames = names;
            int selIndex = 0;
            for (size_t i = 0; i < conns.size(); ++i) {
                if (conns[i] == m_selected) selIndex = static_cast<int>(i);
            }
            top.push_back(FormRow("Configure", Picker(selNames, selIndex).onChange([this, conns](int i) {
                m_selected = conns[static_cast<size_t>(i)];
                rebuild();
            })));
            m_body.add(ui::section("Arrangement", top, "Displays are arranged side by side in the order shown."));
        }

        const DisplayMonitor* mon = monitorFor(m_state, m_selected);
        if (!mon) return;
        MonitorChoice& choice = m_choices[m_selected];
        const DisplayMode* mode = modeFor(*mon, choice.modeId);
        if (!mode && !mon->modes.empty()) mode = &mon->modes.front();
        if (!mode) return;

        // Resolutions: unique sizes, largest first.
        std::vector<std::pair<int, int>> sizes;
        for (const auto& m : mon->modes) {
            if (std::find(sizes.begin(), sizes.end(), std::pair{m.width, m.height}) == sizes.end()) sizes.emplace_back(m.width, m.height);
        }
        std::sort(sizes.begin(), sizes.end(), [](auto a, auto b) { return a.first * a.second > b.first * b.second; });
        std::vector<std::string> sizeNames;
        int sizeIndex = 0;
        for (size_t i = 0; i < sizes.size(); ++i) {
            auto [w, h] = sizes[i];
            std::string n = std::to_string(w) + " × " + std::to_string(h);
            if (auto a = aspect(w, h); !a.empty()) n += " (" + a + ")";
            for (const auto& m : mon->modes) {
                if (m.preferred && m.width == w && m.height == h) {
                    n += " — Recommended";
                    break;
                }
            }
            sizeNames.push_back(n);
            if (w == mode->width && h == mode->height) sizeIndex = static_cast<int>(i);
        }
        auto pickMode = [this, mon](int w, int h, double refresh, bool vrr) {
            // Closest refresh rate at the new size, keeping VRR if that variant exists.
            const DisplayMode* best = nullptr;
            for (const auto& m : mon->modes) {
                if (m.width != w || m.height != h) continue;
                auto score = [&](const DisplayMode& x) { return std::abs(x.refresh - refresh) + (x.vrr == vrr ? 0 : 0.001); };
                if (!best || score(m) < score(*best)) best = &m;
            }
            if (!best) return;
            MonitorChoice& c = m_choices[m_selected];
            c.modeId = best->id;
            if (std::none_of(best->scales.begin(), best->scales.end(), [&](double s) { return std::abs(s - c.scale) < 0.01; }))
                c.scale = best->preferredScale;
            rebuild();
        };
        auto resolution = Picker(sizeNames, sizeIndex).onChange([sizes, pickMode, mode](int i) {
            auto [w, h] = sizes[static_cast<size_t>(i)];
            pickMode(w, h, mode->refresh, mode->vrr);
        });

        std::vector<double> rates;
        bool hasVrr = false;
        for (const auto& m : mon->modes) {
            if (m.width != mode->width || m.height != mode->height) continue;
            if (m.vrr) hasVrr = true;
            if (std::none_of(rates.begin(), rates.end(), [&](double r) { return std::abs(r - m.refresh) < 0.01; })) rates.push_back(m.refresh);
        }
        std::sort(rates.rbegin(), rates.rend());
        std::vector<std::string> rateNames;
        int rateIndex = 0;
        for (size_t i = 0; i < rates.size(); ++i) {
            char buf[32];
            std::snprintf(buf, sizeof buf, "%.2f Hz", rates[i]);
            rateNames.emplace_back(buf);
            if (std::abs(rates[i] - mode->refresh) < 0.01) rateIndex = static_cast<int>(i);
        }
        auto refresh = Picker(rateNames, rateIndex).onChange([rates, pickMode, mode](int i) {
            pickMode(mode->width, mode->height, rates[static_cast<size_t>(i)], mode->vrr);
        });

        std::vector<std::string> scaleNames;
        int scaleIndex = 0;
        for (size_t i = 0; i < mode->scales.size(); ++i) {
            scaleNames.push_back(scaleText(mode->scales[i]) + (std::abs(mode->scales[i] - mode->preferredScale) < 0.01 ? " — Recommended" : ""));
            if (std::abs(mode->scales[i] - choice.scale) < 0.01) scaleIndex = static_cast<int>(i);
        }
        auto scale = Picker(scaleNames, scaleIndex).onChange([this, scales = mode->scales](int i) {
            m_choices[m_selected].scale = scales[static_cast<size_t>(i)];
            rebuild();
        });

        // Mutter transforms: 0 normal, 1 = 90°, 2 = 180°, 3 = 270° (counter-clockwise).
        const unsigned transforms[] = {0, 3, 1, 2};
        int orientIndex = 0;
        for (int i = 0; i < 4; ++i) {
            if (transforms[i] == choice.transform) orientIndex = i;
        }
        auto orientation = Picker({"Landscape", "Portrait Right", "Portrait Left", "Landscape (Flipped)"}, orientIndex)
                               .onChange([this, transforms](int i) {
                                   m_choices[m_selected].transform = transforms[i];
                                   rebuild();
                               });

        std::vector<Element> rows{FormRow("Resolution", resolution), FormRow("Refresh Rate", refresh)};
        if (hasVrr) {
            rows.push_back(FormRow("Variable Refresh Rate", Toggle("", mode->vrr).onChange([pickMode, mode](bool on) {
                                       pickMode(mode->width, mode->height, mode->refresh, on);
                                   }),
                                   "Lets the display follow the frame rate of games and video"));
        }
        rows.push_back(FormRow("Scale", scale));
        rows.push_back(FormRow("Orientation", orientation));
        if (changed()) {
            auto revert = Button("Revert").onClick([this] {
                m_choices = m_original;
                m_primary = m_originalPrimary;
                rebuild();
            });
            auto applyButton = Button("Apply").style(ButtonStyle::Prominent).onClick([this] { apply(); });
            rows.push_back(FormRow(HStack({Label("Changes are kept only if you confirm them.").secondary().font(TextStyle::Subheadline),
                                           Spacer(), revert, applyButton})
                                       .spacing(8)
                                       .align(Alignment::Center)));
        }
        m_body.add(ui::hero(Icon::Display, theme().colors.blue, mon->name,
                            mon->builtin ? "Built-in display · " + mon->connector : mon->connector));
        m_body.add(ui::section("Display", rows));
    }

    // Brightness -------------------------------------------------------------------------------------

    [[nodiscard]] float readBrightness() const {
        auto readInt = [](const fs::path& p) {
            std::ifstream in(p);
            long v = 0;
            in >> v;
            return v;
        };
        const long cur = readInt(fs::path("/sys/class/backlight") / m_backlight / "brightness");
        return m_maxBrightness > 0 ? 100.0f * static_cast<float>(cur) / static_cast<float>(m_maxBrightness) : 0;
    }

    Element brightnessSection() {
        std::error_code ec;
        for (const auto& e : fs::directory_iterator("/sys/class/backlight", ec)) {
            std::ifstream in(e.path() / "max_brightness");
            long max = 0;
            in >> max;
            if (max > m_maxBrightness) {
                m_maxBrightness = max;
                m_backlight = e.path().filename().string();
            }
        }
        if (m_backlight.empty()) return Element();
        m_brightness = Slider(readBrightness(), 1, 100).width(260);
        m_brightness.onChange([this](float pct) {
            // logind lets the active session set its backlight without root.
            const auto value = static_cast<guint32>(std::lround(pct / 100.0f * static_cast<float>(m_maxBrightness)));
            gx::perform(gx::systemBus(), "org.freedesktop.login1", "/org/freedesktop/login1/session/auto",
                        "org.freedesktop.login1.Session", "SetBrightness",
                        g_variant_new("(ssu)", "backlight", m_backlight.c_str(), std::max<guint32>(1, value)));
        });
        std::vector<Element> rows{FormRow("Brightness", HStack({ui::glyph(Icon::Sun, 14), m_brightness, ui::glyph(Icon::Sun, 20)})
                                                            .spacing(8)
                                                            .align(Alignment::Center))};
        gx::Key ambient("org.gnome.settings-daemon.plugins.power", "ambient-enabled");
        // Only offered when iio-sensor-proxy reports a light sensor, like GNOME Settings.
        auto sensor = gx::property(gx::systemBus(), "net.hadess.SensorProxy", "/net/hadess/SensorProxy", "net.hadess.SensorProxy",
                                   "HasAmbientLight");
        if (!desk::kde() && ambient && sensor && g_variant_get_boolean(sensor.get()))
            rows.push_back(ui::toggleRow("Automatic Brightness", ambient, "Adjusts to the light around you"));
        return ui::section("Brightness", rows);
    }

    Element nightLightSection() {
        constexpr const char* kColor = "org.gnome.settings-daemon.plugins.color";
        std::vector<ui::Option> hours;
        for (int h = 0; h < 48; ++h) {
            char buf[16];
            std::snprintf(buf, sizeof buf, "%02d:%02d", h / 2, (h % 2) * 30);
            char value[16];
            std::snprintf(value, sizeof value, "%g", h / 2.0);
            hours.push_back({buf, value});
        }
        gx::Key automatic(kColor, "night-light-schedule-automatic");
        auto from = ui::pickerRow("From", gx::Key(kColor, "night-light-schedule-from"), hours);
        auto to = ui::pickerRow("To", gx::Key(kColor, "night-light-schedule-to"), hours);
        auto sync = [automatic, from, to] {
            from.node()->setDisabled(automatic.getBool());
            to.node()->setDisabled(automatic.getBool());
        };
        sync();
        automatic.watch(sync);
        return ui::section("Night Light",
                           {ui::toggleRow("Night Light", gx::Key(kColor, "night-light-enabled"),
                                          "Makes the screen warmer to reduce eye strain at night"),
                            ui::pickerRow("Schedule", automatic, {{"Sunset to Sunrise", "true"}, {"Manual Schedule", "false"}}),
                            from, to,
                            ui::sliderRow("Color Temperature", gx::Key(kColor, "night-light-temperature"), 1700, 4700, 100,
                                          "Left is warmer")});
    }

    struct KeepPrompt {
        int seconds = 20;
        bool decided = false;
        std::vector<LogicalConfig> previous;
        Label label = Label("Reverting in 20 seconds.").secondary().font(TextStyle::Subheadline);
        std::weak_ptr<PopoverView> popover;
    };

    App& m_app;
    Element m_anchor;
    DisplayState m_state;
    std::map<std::string, MonitorChoice> m_choices;
    std::map<std::string, MonitorChoice> m_original;
    std::string m_primary;
    std::string m_originalPrimary;
    std::string m_selected;
    VStack m_body = VStack({}).spacing(22);
    std::vector<std::pair<Rect, std::string>> m_hitRects;
    std::string m_backlight;
    long m_maxBrightness = 0;
    Slider m_brightness{0, 0, 1};
    gx::Debounce m_kscreenChanged{0.3, [this] { reload(); }};
};

}  // namespace

Element displaysPanel(Context& ctx) {
    auto panel = std::make_shared<DisplaysPanel>(ctx.app, ctx.anchor);
    panel->reload();
    if (ctx.demo == "keep") ctx.app.after(0.6, [p = panel.get()] { p->demoKeepPrompt(); });
    ctx.onShown.emplace("displays", [p = panel.get()] { p->refreshBrightness(); });
    return ui::page(panel->sections(), panel);
}

// Sound -----------------------------------------------------------------------------------------------------

namespace {

struct AudioDevice {
    int index = -1;
    std::string name;
    std::string description;
    bool mute = false;
    int volume = 0;  ///< percent, loudest channel
    int channels = 0;
    double balance = 0;
    std::vector<std::pair<std::string, std::string>> ports;  ///< name, description
    std::string activePort;
};

struct AudioStream {
    int index = -1;
    std::string app;
    std::string media;
    bool mute = false;
    int volume = 0;
};

int volumePercent(const json::Value& volume) {
    int best = 0;
    for (const auto& [_, ch] : volume.object()) best = std::max(best, std::atoi(ch["value_percent"].string().c_str()));
    return best;
}

std::vector<AudioDevice> parseDevices(const std::string& text, bool sources) {
    std::vector<AudioDevice> out;
    const json::Value root = json::Value::parse(text);
    for (const auto& d : root.array()) {
        // Monitors of outputs are not microphones.
        if (sources && d["properties"]["device.class"].string() == "monitor") continue;
        AudioDevice a;
        a.index = static_cast<int>(d["index"].number(-1));
        a.name = d["name"].string();
        a.description = d["description"].str(a.name);
        a.mute = d["mute"].boolean();
        a.volume = volumePercent(d["volume"]);
        a.channels = static_cast<int>(d["volume"].object().size());
        a.balance = d["balance"].number();
        for (const auto& p : d["ports"].array()) {
            if (p["availability"].string() == "not available") continue;
            std::string desc = p["description"].str(p["name"].string());
            if (desc == "(null)") desc = p["name"].string();
            a.ports.emplace_back(p["name"].string(), desc);
        }
        a.activePort = d["active_port"].string();
        out.push_back(std::move(a));
    }
    return out;
}

std::vector<AudioStream> parseStreams(const std::string& text) {
    std::vector<AudioStream> out;
    const json::Value root = json::Value::parse(text);
    for (const auto& d : root.array()) {
        AudioStream s;
        s.index = static_cast<int>(d["index"].number(-1));
        const auto& props = d["properties"];
        s.app = props["application.name"].str(props["application.process.binary"].str("Unknown App"));
        s.media = props["media.name"].string();
        s.mute = d["mute"].boolean();
        s.volume = volumePercent(d["volume"]);
        out.push_back(std::move(s));
    }
    std::sort(out.begin(), out.end(), [](const AudioStream& a, const AudioStream& b) { return a.index < b.index; });
    return out;
}

std::vector<std::string> pactl(std::initializer_list<std::string> args) {
    std::vector<std::string> v{"pactl"};
    v.insert(v.end(), args);
    return v;
}

/// Runs at most one pactl at a time per control and coalesces slider drags into the latest value.
class Throttle {
public:
    void run(std::vector<std::string> argv) {
        if (m_busy) {
            m_next = std::move(argv);
            return;
        }
        m_busy = true;
        gx::run(argv, [this](const gx::ProcessResult&) {
            m_busy = false;
            if (m_next) {
                auto a = std::move(*m_next);
                m_next.reset();
                run(std::move(a));
            }
        });
    }

private:
    bool m_busy = false;
    std::optional<std::vector<std::string>> m_next;
};

/// Volume slider + mute button for one device or stream; updated in place so drags aren't interrupted.
class VolumeControl {
public:
    VolumeControl(std::function<std::vector<std::string>(int)> setVolume, std::function<std::vector<std::string>()> toggleMute)
        : m_setVolume(std::move(setVolume)), m_toggleMute(std::move(toggleMute)) {
        m_slider.onChange([this](float v) { m_throttle.run(m_setVolume(static_cast<int>(std::lround(v)))); });
        m_mute.onClick([this] { gx::run(m_toggleMute()); });
    }
    void update(int volume, bool mute, float max) {
        m_slider->setRange(0, max);
        if (!m_slider->isPressed()) m_slider->setValue(static_cast<float>(volume));
        m_mute->setIcon(mute ? Icon::SpeakerSlash : Icon::Speaker);
        m_slider.setDisabled(mute);
    }
    [[nodiscard]] Element element() const { return HStack({m_mute, m_slider}).spacing(6).align(Alignment::Center); }

private:
    std::function<std::vector<std::string>(int)> m_setVolume;
    std::function<std::vector<std::string>()> m_toggleMute;
    Throttle m_throttle;
    Slider m_slider = Slider(0, 0, 100).width(240);
    Button m_mute = Button(Icon::Speaker).style(ButtonStyle::Borderless).tooltip("Mute");
};

class SoundPanel {
public:
    SoundPanel() {
        m_outputPicker.onChange([this](int i) { choose(m_outputs, i, "set-default-sink"); });
        m_inputPicker.onChange([this](int i) { choose(m_inputs, i, "set-default-source"); });
        m_outputPort.onChange([this](int i) { choosePort(m_outputs, m_defaultSink, i, "set-sink-port"); });
        m_inputPort.onChange([this](int i) { choosePort(m_inputs, m_defaultSource, i, "set-source-port"); });
        m_balance.onChange([this](float b) { setBalance(b); });
        m_overAmplify.watch([this] { refresh(); });
        gx::stream({"pactl", "subscribe"}, [changed = m_changed](const std::string& line) mutable {
            if (line.find("'change'") != std::string::npos || line.find("'new'") != std::string::npos ||
                line.find("'remove'") != std::string::npos)
                changed();
        });
    }

    std::vector<Element> sections() {
        return {
            ui::section("Output", {FormRow("Output Device", m_outputPicker), m_outputPortRow, FormRow("Volume", m_outputVolume.element()),
                                   m_balanceRow}),
            ui::section("Input", {FormRow("Input Device", m_inputPicker), m_inputPortRow, FormRow("Volume", m_inputVolume.element())}),
            m_apps.element(),
            desk::kde() ? kde::soundsSection() :
            ui::section("Sounds", {ui::toggleRow("Alert Sounds", gx::Key("org.gnome.desktop.sound", "event-sounds")),
                                   ui::toggleRow("Input Feedback Sounds", gx::Key("org.gnome.desktop.sound", "input-feedback-sounds")),
                                   ui::toggleRow("Over-Amplification", m_overAmplify,
                                                 "Allows raising the volume above 100%; may distort the sound")}),
        };
    }

    void refresh() {
        // Five reads, then one update with consistent data.
        gx::run(pactl({"get-default-sink"}), [this](const gx::ProcessResult& sink) {
            m_defaultSink = ui::trim(sink.out);
            gx::run(pactl({"get-default-source"}), [this](const gx::ProcessResult& source) {
                m_defaultSource = ui::trim(source.out);
                gx::run(pactl({"-f", "json", "list", "sinks"}), [this](const gx::ProcessResult& sinks) {
                    m_outputs = parseDevices(sinks.out, false);
                    gx::run(pactl({"-f", "json", "list", "sources"}), [this](const gx::ProcessResult& sources) {
                        m_inputs = parseDevices(sources.out, true);
                        gx::run(pactl({"-f", "json", "list", "sink-inputs"}), [this](const gx::ProcessResult& streams) {
                            update(parseStreams(streams.out));
                        }, false);
                    }, false);
                }, false);
            }, false);
        }, false);
    }

private:
    static const AudioDevice* find(const std::vector<AudioDevice>& list, const std::string& name) {
        for (const auto& d : list) {
            if (d.name == name) return &d;
        }
        return nullptr;
    }

    void choose(const std::vector<AudioDevice>& list, int i, const char* command) {
        if (i < 0 || i >= static_cast<int>(list.size())) return;
        gx::run(pactl({command, list[static_cast<size_t>(i)].name}));
    }

    void choosePort(const std::vector<AudioDevice>& list, const std::string& device, int i, const char* command) {
        const AudioDevice* d = find(list, device);
        if (!d || i < 0 || i >= static_cast<int>(d->ports.size())) return;
        gx::run(pactl({command, d->name, d->ports[static_cast<size_t>(i)].first}));
    }

    void setBalance(float b) {
        const AudioDevice* d = find(m_outputs, m_defaultSink);
        if (!d || d->channels != 2) return;
        const double v = d->volume;
        const int left = static_cast<int>(std::lround(v * (1 - std::max(0.0f, b))));
        const int right = static_cast<int>(std::lround(v * (1 + std::min(0.0f, b))));
        m_balanceThrottle.run(pactl({"set-sink-volume", d->name, std::to_string(left) + "%", std::to_string(right) + "%"}));
    }

    static void fillPicker(Picker& picker, const std::vector<AudioDevice>& list, const std::string& current) {
        std::vector<std::string> names;
        int sel = 0;
        for (size_t i = 0; i < list.size(); ++i) {
            names.push_back(list[i].description);
            if (list[i].name == current) sel = static_cast<int>(i);
        }
        if (names.empty()) names.push_back("No Devices");
        picker->setOptions(names);
        picker->setSelected(sel);
        picker.setDisabled(list.empty());
    }

    static void fillPorts(Picker& picker, FormRow& row, const AudioDevice* d) {
        if (!d || d->ports.size() < 2) {
            row.setHidden(true);
            return;
        }
        row.setHidden(false);
        std::vector<std::string> names;
        int sel = 0;
        for (size_t i = 0; i < d->ports.size(); ++i) {
            names.push_back(d->ports[i].second);
            if (d->ports[i].first == d->activePort) sel = static_cast<int>(i);
        }
        picker->setOptions(names);
        picker->setSelected(sel);
    }

    void update(std::vector<AudioStream> streams) {
        const float max = m_overAmplify.getBool() ? 150.0f : 100.0f;
        fillPicker(m_outputPicker, m_outputs, m_defaultSink);
        fillPicker(m_inputPicker, m_inputs, m_defaultSource);
        const AudioDevice* out = find(m_outputs, m_defaultSink);
        const AudioDevice* in = find(m_inputs, m_defaultSource);
        fillPorts(m_outputPort, m_outputPortRow, out);
        fillPorts(m_inputPort, m_inputPortRow, in);
        m_outputVolume.update(out ? out->volume : 0, out && out->mute, max);
        m_inputVolume.update(in ? in->volume : 0, in && in->mute, max);
        m_balanceRow.setHidden(!out || out->channels != 2);
        if (out && !m_balance->isPressed()) m_balance->setValue(static_cast<float>(out->balance));

        // Rebuild app rows only when the set of streams changes; otherwise update in place.
        std::vector<int> ids;
        for (const auto& s : streams) ids.push_back(s.index);
        if (ids != m_streamIds) {
            m_streamIds = ids;
            m_streamControls.clear();
            std::vector<Element> rows;
            for (const auto& s : streams) {
                const std::string idx = std::to_string(s.index);
                auto control = std::make_unique<VolumeControl>(
                    [idx](int v) { return pactl({"set-sink-input-volume", idx, std::to_string(v) + "%"}); },
                    [idx] { return pactl({"set-sink-input-mute", idx, "toggle"}); });
                auto title = VStack({Label(s.app).lineLimit(1)}).spacing(1);
                if (!s.media.empty() && s.media != s.app) title.add(Label(s.media).font(TextStyle::Subheadline).secondary().lineLimit(1).maxWidth(200));
                rows.push_back(FormRow(HStack({ui::letterTile(s.app), title, Spacer(), control->element()}).spacing(10).align(Alignment::Center)));
                m_streamControls.push_back(std::move(control));
            }
            m_apps.setRows(rows);
        }
        for (size_t i = 0; i < streams.size() && i < m_streamControls.size(); ++i)
            m_streamControls[i]->update(streams[i].volume, streams[i].mute, max);
    }

    std::string m_defaultSink;
    std::string m_defaultSource;
    std::vector<AudioDevice> m_outputs;
    std::vector<AudioDevice> m_inputs;
    std::vector<int> m_streamIds;
    std::vector<std::unique_ptr<VolumeControl>> m_streamControls;
    gx::Key m_overAmplify = desk::kde() ? kcfg::key("plasmaparc", "General", "RaiseMaximumVolume", "b", "false")
                                        : gx::Key("org.gnome.desktop.sound", "allow-volume-above-100-percent");
    gx::Debounce m_changed{0.15, [this] { refresh(); }};
    Picker m_outputPicker{std::vector<std::string>{"—"}, 0};
    Picker m_inputPicker{std::vector<std::string>{"—"}, 0};
    Picker m_outputPort{std::vector<std::string>{"—"}, 0};
    Picker m_inputPort{std::vector<std::string>{"—"}, 0};
    FormRow m_outputPortRow{"Output Port", m_outputPort};
    FormRow m_inputPortRow{"Input Port", m_inputPort};
    VolumeControl m_outputVolume{[this](int v) { return pactl({"set-sink-volume", m_defaultSink, std::to_string(v) + "%"}); },
                                 [this] { return pactl({"set-sink-mute", m_defaultSink, "toggle"}); }};
    VolumeControl m_inputVolume{[this](int v) { return pactl({"set-source-volume", m_defaultSource, std::to_string(v) + "%"}); },
                                [this] { return pactl({"set-source-mute", m_defaultSource, "toggle"}); }};
    Slider m_balance = Slider(0, -1, 1).width(240).step(0.05f);
    FormRow m_balanceRow{"Balance", HStack({Label("L").secondary(), m_balance, Label("R").secondary()}).spacing(6).align(Alignment::Center)};
    Throttle m_balanceThrottle;
    ui::DynamicSection m_apps{"Apps", "No apps are playing sound."};
};

}  // namespace

Element soundPanel(Context& /*ctx*/) {
    if (!gx::haveProgram("pactl"))
        return ui::unavailable(Icon::Speaker, "Sound Settings Aren't Available",
                               "Install pactl (pulseaudio-utils or pipewire-pulse) to control audio devices.");
    auto panel = std::make_shared<SoundPanel>();
    panel->refresh();
    return ui::page(panel->sections(), panel);
}

// Power ------------------------------------------------------------------------------------------------------

namespace {

constexpr const char* kUPower = "org.freedesktop.UPower";
constexpr const char* kUPowerDevice = "org.freedesktop.UPower.Device";
constexpr const char* kPowerPlugin = "org.gnome.settings-daemon.plugins.power";

std::string hoursMinutes(std::int64_t seconds) {
    const std::int64_t h = seconds / 3600;
    const std::int64_t m = (seconds % 3600) / 60;
    if (h == 0) return std::to_string(m) + " min";
    return std::to_string(h) + " h " + std::to_string(m) + " min";
}

const char* stateName(std::int64_t state) {
    switch (state) {
        case 1: return "Charging";
        case 2: return "Discharging";
        case 3: return "Empty";
        case 4: return "Fully charged";
        case 5: return "Not charging";
        case 6: return "Discharging";
        default: return "Unknown";
    }
}

/// Picker for "suspend after" that drives two keys: the action type and its timeout.
FormRow suspendRow(const std::string& title, const std::string& prefix) {
    gx::Key type(kPowerPlugin, prefix + "-type");
    gx::Key timeout(kPowerPlugin, prefix + "-timeout");
    const std::vector<int> minutes{0, 15, 20, 25, 30, 45, 60, 80, 90, 100, 120};
    std::vector<std::string> names;
    for (int m : minutes) names.push_back(m == 0 ? "Off" : ui::formatDuration(m * 60));
    if (!type || !timeout) return ui::pickerRow(title, {}, {});
    auto backend = std::make_shared<gx::KeyBackend>();
    backend->type = "i";
    backend->fallback = "0";
    backend->read = [type, timeout, minutes]() -> std::optional<std::string> {
        if (type.getString() == "nothing") return "0";
        int best = 1;
        for (size_t i = 1; i < minutes.size(); ++i) {
            if (std::abs(minutes[i] * 60 - timeout.getInt()) <
                std::abs(minutes[static_cast<size_t>(best)] * 60 - timeout.getInt()))
                best = static_cast<int>(i);
        }
        return std::to_string(best);
    };
    backend->writable = [type, timeout] { return type.writable() && timeout.writable(); };
    backend->write = [type, timeout, minutes](const std::optional<std::string>& value, gx::WriteDone) {
        const int i = value ? std::atoi(value->c_str()) : 0;
        if (i == 0) return type.setString("nothing");
        if (i < 0 || i >= static_cast<int>(minutes.size())) return gx::WriteResult::failure("Invalid suspend timeout");
        auto result = timeout.setInt(minutes[static_cast<size_t>(i)] * 60);
        return result.ok() ? type.setString("suspend") : result;
    };
    backend->watch = [type, timeout](std::function<void()> fn) {
        type.watch(fn);
        timeout.watch(std::move(fn));
    };
    std::vector<ui::Option> options;
    for (size_t i = 0; i < names.size(); ++i) options.push_back({names[i], std::to_string(i)});
    return ui::pickerRow(title, gx::Key(backend), options);
}

class PowerPanel {
public:
    PowerPanel() {
        auto changed = m_changed;
        gx::subscribe(gx::systemBus(), kUPower, "org.freedesktop.DBus.Properties", "PropertiesChanged", nullptr,
                      [changed](const std::string&, const std::string&, GVariant*) mutable { changed(); });
        gx::subscribe(gx::systemBus(), kUPower, kUPower, nullptr, nullptr,
                      [changed](const std::string&, const std::string&, GVariant*) mutable { changed(); });
        for (const char* name : {"org.freedesktop.UPower.PowerProfiles", "net.hadess.PowerProfiles"}) {
            if (gx::hasName(gx::systemBus(), name) && gx::property(gx::systemBus(), name, profilesPath(name), name, "ActiveProfile")) {
                m_profiles = name;
                break;
            }
        }
        if (!m_profiles.empty()) {
            gx::subscribe(gx::systemBus(), m_profiles.c_str(), "org.freedesktop.DBus.Properties", "PropertiesChanged", nullptr,
                          [changed](const std::string&, const std::string&, GVariant*) mutable { changed(); });
        }
    }

    std::vector<Element> sections() {
        std::vector<Element> out{m_battery.element()};
        if (!m_profiles.empty()) out.push_back(m_modeSection);
        out.push_back(m_devices.element());
        if (desk::kde()) {
            for (auto& sec : kde::powerSections()) out.push_back(sec);
            return out;
        }
        out.push_back(ui::section("Power Saving",
                                  {ui::toggleRow("Dim Screen", gx::Key(kPowerPlugin, "idle-dim"), "Reduces brightness after a short time of inactivity"),
                                   ui::pickerRow("Screen Blank", gx::Key("org.gnome.desktop.session", "idle-delay"),
                                                 {{"1 minute", "60"}, {"2 minutes", "120"}, {"3 minutes", "180"}, {"4 minutes", "240"},
                                                  {"5 minutes", "300"}, {"8 minutes", "480"}, {"10 minutes", "600"},
                                                  {"12 minutes", "720"}, {"15 minutes", "900"}, {"Never", "0"}},
                                                 "Turns the screen off after this much inactivity"),
                                   ui::toggleRow("Automatic Power Saver", gx::Key(kPowerPlugin, "power-saver-profile-on-low-battery"),
                                                 "Turns on power saver mode when the battery is low")}));
        out.push_back(ui::section("Suspend & Power Button",
                                  {suspendRow("Automatic Suspend on Battery", "sleep-inactive-battery"),
                                   suspendRow("Automatic Suspend When Plugged In", "sleep-inactive-ac"),
                                   ui::pickerRow("Power Button Behavior", gx::Key(kPowerPlugin, "power-button-action"),
                                                 {{"Suspend", "suspend"}, {"Power Off", "interactive"}, {"Hibernate", "hibernate"},
                                                  {"Nothing", "nothing"}}),
                                   ui::toggleRow("Show Battery Percentage", gx::Key("org.gnome.desktop.interface", "show-battery-percentage"),
                                                 "In the top bar")}));
        return out;
    }

    void refresh() {
        refreshBattery();
        refreshDevices();
        refreshProfiles();
    }

private:
    static const char* profilesPath(const std::string& name) {
        return name == "net.hadess.PowerProfiles" ? "/net/hadess/PowerProfiles" : "/org/freedesktop/UPower/PowerProfiles";
    }

    void refreshBattery() {
        auto d = gx::properties(gx::systemBus(), kUPower, "/org/freedesktop/UPower/devices/DisplayDevice", kUPowerDevice);
        if (!d || !gx::lookupBool(d.get(), "IsPresent")) {
            m_battery.setTitle({});
            m_battery.setRows({FormRow(HStack({ui::tile(Icon::Bolt, theme().colors.green, 28),
                                               Label("This computer runs on external power.").secondary()})
                                           .spacing(10)
                                           .align(Alignment::Center))});
            return;
        }
        const double pct = gx::lookupDouble(d.get(), "Percentage");
        const auto state = gx::lookupInt(d.get(), "State");
        std::string detail = stateName(state);
        if (state == 2 || state == 6) {
            if (auto t = gx::lookupInt(d.get(), "TimeToEmpty"); t > 0) detail += " · " + hoursMinutes(t) + " remaining";
        } else if (state == 1) {
            if (auto t = gx::lookupInt(d.get(), "TimeToFull"); t > 0) detail += " · full in " + hoursMinutes(t);
        }
        char buf[16];
        std::snprintf(buf, sizeof buf, "%.0f%%", pct);
        std::vector<Element> rows{FormRow(HStack({ui::batteryIcon(pct / 100, state == 1 || state == 4, 36),
                                                  VStack({Label(buf).font(TextStyle::Title3), Label(detail).secondary().font(TextStyle::Subheadline)}).spacing(1)})
                                              .spacing(12)
                                              .align(Alignment::Leading))};
        // Health and charge limiting come from the laptop battery itself.
        auto list = gx::callSync(gx::systemBus(), kUPower, "/org/freedesktop/UPower", kUPower, "EnumerateDevices", nullptr, "(ao)");
        if (list) {
            gx::Variant paths(g_variant_get_child_value(list.get(), 0));
            GVariantIter it;
            g_variant_iter_init(&it, paths.get());
            const char* path = nullptr;
            while (g_variant_iter_next(&it, "&o", &path)) {
                auto b = gx::properties(gx::systemBus(), kUPower, path, kUPowerDevice);
                if (!b || gx::lookupInt(b.get(), "Type") != 2 || !gx::lookupBool(b.get(), "PowerSupply")) continue;
                const double full = gx::lookupDouble(b.get(), "EnergyFull");
                const double design = gx::lookupDouble(b.get(), "EnergyFullDesign");
                if (design > 0) {
                    std::snprintf(buf, sizeof buf, "%.0f%%", std::min(100.0, 100 * full / design));
                    rows.push_back(ui::infoRow("Battery Health", buf));
                }
                if (auto cycles = gx::lookupInt(b.get(), "ChargeCycles", -1); cycles > 0) rows.push_back(ui::infoRow("Charge Cycles", std::to_string(cycles)));
                if (gx::lookupBool(b.get(), "ChargeThresholdSupported")) {
                    const std::string p = path;
                    rows.push_back(FormRow("Preserve Battery Health",
                                           Toggle("", gx::lookupBool(b.get(), "ChargeThresholdEnabled")).onChange([p](bool on) {
                                               gx::perform(gx::systemBus(), kUPower, p.c_str(), kUPowerDevice, "EnableChargeThreshold",
                                                           g_variant_new("(b)", on));
                                           }),
                                           "Stops charging before full to slow battery wear"));
                }
                break;
            }
        }
        m_battery.setTitle("Battery");
        m_battery.setRows(rows);
    }

    void refreshDevices() {
        std::vector<Element> rows;
        auto list = gx::callSync(gx::systemBus(), kUPower, "/org/freedesktop/UPower", kUPower, "EnumerateDevices", nullptr, "(ao)");
        if (list) {
            gx::Variant paths(g_variant_get_child_value(list.get(), 0));
            GVariantIter it;
            g_variant_iter_init(&it, paths.get());
            const char* path = nullptr;
            while (g_variant_iter_next(&it, "&o", &path)) {
                auto b = gx::properties(gx::systemBus(), kUPower, path, kUPowerDevice);
                // Peripherals: anything with a battery that doesn't power the computer.
                if (!b || gx::lookupBool(b.get(), "PowerSupply") || gx::lookupInt(b.get(), "Type") == 1) continue;
                const double pct = gx::lookupDouble(b.get(), "Percentage", -1);
                if (pct < 0 || !gx::lookupBool(b.get(), "IsPresent", true)) continue;
                std::string name = gx::lookupString(b.get(), "Model");
                if (name.empty()) name = gx::lookupString(b.get(), "NativePath");
                char buf[16];
                std::snprintf(buf, sizeof buf, "%.0f%%", pct);
                rows.push_back(FormRow(HStack({ui::batteryIcon(pct / 100, gx::lookupInt(b.get(), "State") == 1), Label(name), Spacer(),
                                               Label(buf).secondary()})
                                           .spacing(10)
                                           .align(Alignment::Center)));
            }
        }
        m_devices.setRows(rows);
        m_devices.setHidden(rows.empty());
    }

    void refreshProfiles() {
        if (m_profiles.empty()) return;
        const char* path = profilesPath(m_profiles);
        auto active = gx::property(gx::systemBus(), m_profiles.c_str(), path, m_profiles.c_str(), "ActiveProfile");
        auto list = gx::property(gx::systemBus(), m_profiles.c_str(), path, m_profiles.c_str(), "Profiles");
        std::vector<std::string> ids;
        if (list) {
            GVariantIter it;
            g_variant_iter_init(&it, list.get());
            GVariant* dict = nullptr;
            while (g_variant_iter_next(&it, "@a{sv}", &dict)) {
                gx::Variant d(dict);
                ids.push_back(gx::lookupString(dict, "Profile"));
            }
        }
        // Power saver, balanced, performance in that order, like GNOME.
        std::vector<std::string> order;
        for (const char* p : {"power-saver", "balanced", "performance"}) {
            if (std::find(ids.begin(), ids.end(), p) != ids.end()) order.emplace_back(p);
        }
        if (order != m_profileIds) {
            m_profileIds = order;
            std::vector<Element> rows;
            m_profileRows.clear();
            for (const auto& id : order) {
                const char* title = id == "power-saver" ? "Power Saver" : id == "balanced" ? "Balanced" : "Performance";
                const char* sub = id == "power-saver" ? "Reduced performance and power usage"
                                  : id == "balanced" ? "Standard performance and power usage"
                                                     : "High performance and power usage";
                auto check = ui::glyph(Icon::Checkmark, 16, false);
                auto row = ui::ClickRow({VStack({Label(title), Label(sub).font(TextStyle::Subheadline).secondary()}).spacing(1), Spacer(), check});
                row.onClick([this, id] {
                    gx::setProperty(gx::systemBus(), m_profiles.c_str(), profilesPath(m_profiles), m_profiles.c_str(), "ActiveProfile",
                                    g_variant_new_string(id.c_str()), [this](gx::Variant, const std::string& e) {
                                        if (!e.empty()) ui::message(m_modeSection, "Couldn't change the power mode", e);
                                        refreshProfiles();
                                    });
                });
                m_profileRows.emplace_back(id, check);
                rows.push_back(row);
            }
            m_modeCard.setRows(rows);
        }
        const std::string current = active ? gx::str(active.get()) : "";
        for (auto& [id, check] : m_profileRows) check.setHidden(id != current);
    }

    std::string m_profiles;
    std::vector<std::string> m_profileIds;
    std::vector<std::pair<std::string, Canvas>> m_profileRows;
    gx::Debounce m_changed{0.5, [this] { refresh(); }};
    ui::DynamicSection m_battery{"Battery", ""};
    ui::DynamicSection m_modeCard{"Power Mode", "No power modes available."};
    Element m_modeSection = m_modeCard.element();
    ui::DynamicSection m_devices{"Devices", ""};
};

}  // namespace

Element powerPanel(Context& /*ctx*/) {
    auto panel = std::make_shared<PowerPanel>();
    panel->refresh();
    return ui::page(panel->sections(), panel);
}
