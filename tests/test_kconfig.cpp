// KConfig files as the System Settings example reads and writes them for KDE Plasma.
#include <doctest/doctest.h>

#include <sys/resource.h>
#include <csignal>
#include <cstdlib>

#define G_SETTINGS_ENABLE_BACKEND
#include <gio/gsettingsbackend.h>

#include <filesystem>
#include <fstream>
#include <sstream>
#include <string>
#include "test_support.hpp"
#include "ui.hpp"

#include "kconfig.hpp"

namespace fs = std::filesystem;

namespace {

/// A fresh XDG_CONFIG_HOME for the whole run (kcfg caches files by name, so each test uses its own file).
const fs::path& configHome() {
    static const fs::path dir = [] {
        std::string tmpl = (fs::temp_directory_path() / "mayo-kconfig-XXXXXX").string();
        const fs::path d = mkdtemp(tmpl.data());
        setenv("XDG_CONFIG_HOME", d.c_str(), 1);
        setenv("GSETTINGS_BACKEND", "memory", 1);
        // Test notifications cannot reach the real session bus.
        setenv("DBUS_SESSION_BUS_ADDRESS", "unix:path=/nonexistent/forma-settings-test", 1);
        setenv("XDG_CONFIG_DIRS", (d / "xdg").c_str(), 1);
        return d;
    }();
    return dir;
}

void put(const std::string& name, const std::string& text) {
    fs::create_directories(configHome());
    std::ofstream(configHome() / name) << text;
}

std::string contents(const std::string& name) {
    std::ifstream in(configHome() / name);
    std::stringstream ss;
    ss << in.rdbuf();
    return ss.str();
}

}  // namespace

TEST_CASE("kconfig: escaping round-trips") {
    for (const std::string v : {"plain", " leading", "trailing ", "tab\there", "line\nbreak", "back\\slash", "a\\,b"}) {
        CHECK(kcfg::unescape(kcfg::escape(v)) == v);
    }
    CHECK(kcfg::unescape("\\sx\\x41") == " xA");
    CHECK(gx::splitList("us,tr,de") == std::vector<std::string>{"us", "tr", "de"});
    CHECK(gx::splitList(gx::joinList({"a,b", "c"})) == std::vector<std::string>{"a,b", "c"});
    CHECK(gx::splitList("").empty());
}

TEST_CASE("kconfig: reading groups, nested groups and markers") {
    put("readrc",
        "# comment\n"
        "[General]\n"
        "ColorScheme=BreezeDark\n"
        "Name[de]=Allgemein\n"
        "Name=General\n"
        "folders[$e]=$HOME/Music\n"
        "\n"
        "[Containments][1][Wallpaper][org.kde.image][General]\n"
        "Image = file:///usr/share/wallpapers/Next/\n");
    fs::create_directories(configHome() / "xdg");
    std::ofstream(configHome() / "xdg" / "readrc") << "[General]\nAccentColor=61,174,233\n";

    CHECK(kcfg::read("readrc", "General", "ColorScheme") == "BreezeDark");
    CHECK(kcfg::read("readrc", "General", "Name") == "General");
    CHECK(kcfg::read("readrc", "General", "folders") == std::string(std::getenv("HOME")) + "/Music");
    CHECK(kcfg::read("readrc", "Containments][1][Wallpaper][org.kde.image][General", "Image") ==
          "file:///usr/share/wallpapers/Next/");
    CHECK(kcfg::read("readrc", "General", "AccentColor") == "61,174,233");  // system default
    CHECK_FALSE(kcfg::read("readrc", "General", "Missing"));
    CHECK(kcfg::groups("readrc") ==
          std::vector<std::string>{"General", "Containments][1][Wallpaper][org.kde.image][General"});
}

TEST_CASE("kconfig: writing keeps the rest of the file") {
    put("writerc", "# keep me\n[Windows]\nFocusPolicy=ClickToFocus\nPlacement=Smart\n\n[Other]\nx=1\n");
    kcfg::write("writerc", "Windows", "Placement", "Centered");
    kcfg::write("writerc", "Windows", "ElectricBorderTiling", "false");
    kcfg::write("writerc", "NightColor", "Active", "true");
    kcfg::write("writerc", "Other", "x", std::nullopt);
    kcfg::write("writerc", "Other", "y", " spaced ");
    CHECK(contents("writerc") ==
          "# keep me\n[Windows]\nFocusPolicy=ClickToFocus\nPlacement=Centered\nElectricBorderTiling=false\n\n"
          "[Other]\ny=\\sspaced\\s\n\n[NightColor]\nActive=true\n");
    CHECK(kcfg::read("writerc", "Other", "y") == " spaced ");
    CHECK_FALSE(kcfg::read("writerc", "Other", "x"));
}

TEST_CASE("kconfig: keys bound to entries") {
    put("keyrc", "[Keyboard]\nRepeatDelay=250\n");
    auto delay = kcfg::key("keyrc", "Keyboard", "RepeatDelay", "i", "600");
    auto rate = kcfg::key("keyrc", "Keyboard", "RepeatRate", "d", "25");
    auto repeat = kcfg::flagKey("keyrc", "Keyboard", "KeyRepeat", "repeat", "nothing", true);
    auto layouts = kcfg::key("keyrc", "Layout", "LayoutList", "as", "");
    CHECK(delay.getInt() == 250);
    CHECK(rate.getDouble() == doctest::Approx(25));
    CHECK(rate.isDefault());
    CHECK(repeat.getBool());

    int fired = 0;
    delay.watch([&] { ++fired; });
    delay.setInt(400);
    CHECK(fired == 1);
    CHECK(delay.getInt() == 400);
    delay.setInt(400);  // unchanged: no write, no notification
    CHECK(fired == 1);

    repeat.set(false);
    CHECK(kcfg::read("keyrc", "Keyboard", "KeyRepeat") == "nothing");
    CHECK_FALSE(repeat.getBool());
    rate.setDouble(30.5);
    CHECK(kcfg::read("keyrc", "Keyboard", "RepeatRate") == "30.5");
    layouts.setStrv({"us", "tr"});
    CHECK(kcfg::read("keyrc", "Layout", "LayoutList") == "us,tr");
    CHECK(layouts.getStrv() == std::vector<std::string>{"us", "tr"});
    delay.reset();
    CHECK(delay.getInt() == 600);
    CHECK(delay.isDefault());
}

namespace {
struct ReadOnlyScope {
    bool before = gx::readOnly();
    explicit ReadOnlyScope(bool value) { gx::setReadOnly(value); }
    ~ReadOnlyScope() { gx::setReadOnly(before); }
};
// A real write(2) error even as root; restore both the resource limit and signal handler.
struct FileSizeLimit {
    rlimit before{};
    using Handler = void (*)(int);
    Handler handler;
    FileSizeLimit() : handler(std::signal(SIGXFSZ, SIG_IGN)) {
        REQUIRE(getrlimit(RLIMIT_FSIZE, &before) == 0);
        auto limit = before;
        limit.rlim_cur = 0;
        REQUIRE(setrlimit(RLIMIT_FSIZE, &limit) == 0);
    }
    ~FileSizeLimit() {
        setrlimit(RLIMIT_FSIZE, &before);
        std::signal(SIGXFSZ, handler);
    }
};
template <class T>
std::shared_ptr<T> control(const mayo::FormRow& row) {
    return std::dynamic_pointer_cast<T>(row->children().back());
}
std::string detail(const mayo::FormRow& row) {
    auto label = std::dynamic_pointer_cast<mayo::LabelView>(row->children().front()->children().back());
    return label ? label->text() : std::string();
}
void prepareSchema() {
    static const bool prepared = [] {
        const auto dir = configHome() / "schemas";
        fs::create_directories(dir);
        std::ofstream(dir / "org.forma.settings.test.gschema.xml") << R"(
<schemalist><schema id="org.forma.settings.test" path="/org/forma/settings/test/">
<key name="enabled" type="b"><default>false</default></key>
<key name="count" type="i"><range min="0" max="10"/><default>2</default></key>
<key name="text" type="s"><default>'original'</default></key>
</schema></schemalist>)";
        REQUIRE(gx::runSync({"glib-compile-schemas", dir.string()}).ok());
        setenv("GSETTINGS_SCHEMA_DIR", dir.c_str(), 1);
        return true;
    }();
    (void)prepared;
}
}  // namespace

TEST_CASE("kconfig: failed storage preserves cached values, lines and notifications") {
    ReadOnlyScope writable(false);
    put("failrc", "# retained\n[General]\nx=old\n");
    CHECK(kcfg::read("failrc", "General", "x") == "old");
    auto notifications = std::make_shared<int>(0);
    kcfg::watch("failrc", "General", "x", [notifications] { ++*notifications; });
    {
        FileSizeLimit limit;
        auto result = kcfg::write("failrc", "General", "x", "new");
        CHECK(result.status == gx::WriteStatus::Failure);
        CHECK_FALSE(result.error.empty());
        CHECK(kcfg::read("failrc", "General", "x") == "old");
        CHECK(kcfg::write("failrc", "Ghost", "entry", "uncommitted").status == gx::WriteStatus::Failure);
    }
    CHECK(*notifications == 0);
    CHECK(contents("failrc") == "# retained\n[General]\nx=old\n");
    CHECK(kcfg::write("failrc", "General", "x", "good").ok());
    CHECK(contents("failrc") == "# retained\n[General]\nx=good\n");
    CHECK(kcfg::groups("failrc") == std::vector<std::string>{"General"});
    CHECK(*notifications == 1);
}

TEST_CASE("kconfig: readonly rejects keys, flags, groups and tools without writing") {
    put("readonlyrc", "[A]\nx=1\n");
    auto key = kcfg::key("readonlyrc", "A", "x", "i", "0");
    auto flag = kcfg::flagKey("readonlyrc", "A", "x", "1", "0", false);
    auto multi = kcfg::multiKey("readonlyrc", {"A", "B"}, "x", "i", "0");
    int applied = 0;
    auto tool = kcfg::appliedKey("readonlyrc", "A", "x", "i", "0", [&applied](const std::string&, gx::WriteDone) {
        ++applied;
        return gx::WriteResult{};
    });
    ReadOnlyScope readonly(true);
    CHECK(key.available());
    CHECK(key.readOnly());
    CHECK_FALSE(key.writable());
    auto row = ui::toggleRow("Flag", flag);
    REQUIRE(control<mayo::ToggleView>(row));
    CHECK_FALSE(control<mayo::ToggleView>(row)->isEnabled());
    CHECK(detail(row).find("Read-only") != std::string::npos);
    bool done = false;
    const auto rejected = key.setInt(2, [&done](gx::WriteResult r) {
        done = true;
        CHECK(r.status == gx::WriteStatus::ReadOnly);
    });
    CHECK(rejected.status == gx::WriteStatus::ReadOnly);
    CHECK(done);
    CHECK(flag.set(false).status == gx::WriteStatus::ReadOnly);
    CHECK(multi.setInt(3).status == gx::WriteStatus::ReadOnly);
    CHECK(tool.reset().status == gx::WriteStatus::ReadOnly);
    CHECK(kcfg::write("readonlyrc", "A", "x", "4").status == gx::WriteStatus::ReadOnly);
    CHECK(gx::runWrite({"/bin/sh", "-c", "exit 0"}, {}).status == gx::WriteStatus::ReadOnly);
    CHECK(applied == 0);
    CHECK(contents("readonlyrc") == "[A]\nx=1\n");
}

TEST_CASE("kconfig: multiKey is atomic and respects immutable entries and groups") {
    ReadOnlyScope writable(false);
    put("multirc", "[A]\nx=1\n[B]\nx=2\n");
    auto multi = kcfg::multiKey("multirc", {"A", "B"}, "x", "i", "0");
    {
        FileSizeLimit limit;
        CHECK(multi.setInt(7).status == gx::WriteStatus::Failure);
    }
    CHECK(kcfg::read("multirc", "A", "x") == "1");
    CHECK(kcfg::read("multirc", "B", "x") == "2");
    CHECK(multi.setInt(7).ok());
    CHECK(kcfg::read("multirc", "A", "x") == "7");
    CHECK(kcfg::read("multirc", "B", "x") == "7");
    CHECK_FALSE(kcfg::multiKey("multirc", {}, "x", "i", "0").available());
    put("lockedrc", "[A]\nx=1\n[B]\nx[$i]=2\n[C][$i]\nx=3\n");
    auto locked = kcfg::multiKey("lockedrc", {"A", "B"}, "x", "i", "0");
    CHECK_FALSE(locked.writable());
    CHECK(locked.setInt(9).status == gx::WriteStatus::ReadOnly);
    CHECK(kcfg::read("lockedrc", "A", "x") == "1");
    CHECK(kcfg::read("lockedrc", "C", "x") == "3");
    CHECK_FALSE(kcfg::key("lockedrc", "C", "other", "i", "0").writable());
    fs::create_directories(configHome() / "xdg");
    std::ofstream(configHome() / "xdg" / "systemlockedrc") << "[A]\nx[$i]=1\n";
    CHECK_FALSE(kcfg::key("systemlockedrc", "A", "x", "i", "0").writable());
}

TEST_CASE("kconfig: applied failure propagates asynchronously and reads actual disk state") {
    ReadOnlyScope writable(false);
    put("appliedrc", "[A]\nx=old\n");
    gx::WriteDone complete;
    auto applied =
        kcfg::appliedKey("appliedrc", "A", "x", "s", "default", [&complete](const std::string&, gx::WriteDone done) {
            complete = std::move(done);
            return gx::WriteResult{gx::WriteStatus::Pending, {}};
        });
    auto row = ui::pickerRow("Applied", applied, {{"Old", "old"}, {"New", "new"}, {"Actual", "actual"}});
    auto picker = control<mayo::PickerView>(row);
    REQUIRE(picker);
    picker->setSelected(1);
    int finished = 0;
    const auto pending = applied.setString("new", [&finished](gx::WriteResult r) {
        ++finished;
        CHECK(r.status == gx::WriteStatus::Failure);
    });
    CHECK(pending.pending());
    CHECK(applied.pending());
    CHECK_FALSE(picker->isEnabled());
    CHECK(detail(row).find("Applying") != std::string::npos);
    CHECK(finished == 0);
    bool rejected = false;
    CHECK(applied.setString("another", [&rejected](gx::WriteResult r) { rejected = !r.ok(); }).status ==
          gx::WriteStatus::Failure);
    CHECK(rejected);
    CHECK(applied.pending());
    put("appliedrc", "[A]\nx=actual\n");
    auto result = gx::runSync({"/bin/sh", "-c", "printf 'policy denied' >&2; exit 7"});
    CHECK(result.status == 7);
    complete(gx::WriteResult::failure(result.err));
    CHECK(finished == 1);
    CHECK_FALSE(applied.pending());
    CHECK(applied.getString() == "actual");
    CHECK(picker->selected() == 2);
    CHECK(picker->isEnabled());
    CHECK(detail(row) == "policy denied");
    complete(gx::WriteResult::failure("duplicate callback"));
    CHECK(finished == 1);
}

TEST_CASE("kconfig: bound controls rollback failed writes without looping watchers") {
    ReadOnlyScope writable(false);
    put("controlsrc", "[A]\nflag=true\ncount=2\ntext=old\n");
    auto flag = kcfg::flagKey("controlsrc", "A", "flag", "true", "false", false);
    auto count = kcfg::key("controlsrc", "A", "count", "i", "0");
    auto text = kcfg::key("controlsrc", "A", "text", "s", "");
    auto flagRow = ui::toggleRow("Flag", flag);
    auto sliderRow = ui::sliderRow("Count", count, 0, 10);
    auto textRow = ui::textRow("Text", text);
    auto toggle = control<mayo::ToggleView>(flagRow);
    auto slider = control<mayo::SliderView>(sliderRow);
    auto field = control<mayo::TextFieldView>(textRow);
    REQUIRE(toggle);
    REQUIRE(slider);
    REQUIRE(field);
    auto notifications = std::make_shared<int>(0);
    count.watch([notifications] { ++*notifications; });
    mayo::test::TestHost host;
    host.attach(textRow.node());
    host.dispatcher.setFocus(field.get(), false);
    REQUIRE(field->isFocused());
    {
        FileSizeLimit limit;
        toggle->onKeyDown(mayo::KeyEvent{mayo::Key::Space});
        slider->setValue(8, true);
        field->selectAll();
        field->insert("attempted");
    }
    CHECK(toggle->isOn());
    CHECK(slider->value() == 2);
    CHECK(field->text() == "old");
    CHECK_FALSE(detail(textRow).empty());
    CHECK(*notifications == 0);
    CHECK(contents("controlsrc") == "[A]\nflag=true\ncount=2\ntext=old\n");
    CHECK(count.setInt(4).ok());
    CHECK(slider->value() == 4);
    CHECK(*notifications == 1);
}

TEST_CASE("kconfig: bound pages do not retain widgets and legacy actions report errors") {
    ReadOnlyScope writable(false);
    put("lifetimerc", "[A]\nx=old\n");
    auto key = kcfg::key("lifetimerc", "A", "x", "s", "");
    std::weak_ptr<mayo::TextFieldView> weak;
    {
        auto row = ui::textRow("Text", key);
        weak = control<mayo::TextFieldView>(row);
    }
    CHECK(weak.expired());
    CHECK(key.setString("updated").ok());
    auto standalone = kcfg::key("lifetimerc", "A", "count", "i", "2");
    int errors = 0;
    gx::setWriteErrorHandler([&errors](const gx::Key& k, const gx::WriteResult& result) {
        ++errors;
        CHECK(k.name() == "count");
        CHECK(result.status == gx::WriteStatus::Failure);
        CHECK(result.error.find("numeric") != std::string::npos);
    });
    CHECK(standalone.setText("invalid").status == gx::WriteStatus::Failure);
    CHECK(errors == 1);
    gx::setWriteErrorHandler({});
}

TEST_CASE("kconfig: GSettings memory writes, validation and policy locks") {
    prepareSchema();
    ReadOnlyScope writable(false);
    gx::Object<GSettingsBackend> memory(g_memory_settings_backend_new());
    gx::Key count("org.forma.settings.test", "count", {}, memory.get());
    gx::Key enabled("org.forma.settings.test", "enabled", {}, memory.get());
    CHECK(count.available());
    CHECK(count.writable());
    CHECK(count.getInt() == 2);
    CHECK(count.setInt(5).ok());
    CHECK(count.setInt(20).status == gx::WriteStatus::Failure);
    CHECK(count.getInt() == 5);
    CHECK(count.setText("junk").status == gx::WriteStatus::Failure);
    CHECK(count.setString("wrong type").status == gx::WriteStatus::Failure);
    CHECK(count.reset().ok());
    CHECK(count.isDefault());
    CHECK(count.getInt() == 2);
    CHECK(enabled.set(true).ok());
    // The null backend advertises get_writable=false, like policy-locked dconf.
    gx::Object<GSettingsBackend> locked(g_null_settings_backend_new());
    gx::Key policy("org.forma.settings.test", "enabled", {}, locked.get());
    CHECK(policy.available());
    CHECK_FALSE(policy.writable());
    auto row = ui::toggleRow("Locked", policy);
    CHECK_FALSE(control<mayo::ToggleView>(row)->isEnabled());
    CHECK(policy.set(true).status == gx::WriteStatus::ReadOnly);
    CHECK_FALSE(policy.getBool());
}

TEST_CASE("kconfig: subprocess completion reaches bound UI on event-only loop") {
    configHome();
    ReadOnlyScope writable(false);
    mayo::App app("forma-settings-write-test", {}, {.background = true});
    app.settings().setReadOnly(true);
    app.setKeepAlive(true);
    gx::start(app);
    put("processrc", "[A]\nx=old\n");
    put("process-successrc", "[A]\nx=old\n");
    auto key = kcfg::appliedKey("processrc", "A", "x", "s", "old", [](const std::string&, gx::WriteDone done) {
        return gx::runWrite({"/bin/sh", "-c", "printf 'mock tool refused' >&2; exit 9"}, std::move(done));
    });
    auto missing = kcfg::appliedKey("processrc", "A", "other", "s", "old", [](const std::string&, gx::WriteDone done) {
        return gx::runWrite({"/nonexistent/forma-settings-mock"}, std::move(done));
    });
    const std::string destination = (configHome() / "process-successrc").string();
    auto success = kcfg::appliedKey(
        "process-successrc", "A", "x", "s", "old", [destination](const std::string&, gx::WriteDone done) {
            return gx::runWrite({"/bin/sh", "-c", "printf '[A]\\nx=new\\n' > \"$1\"", "--", destination},
                                std::move(done));
        });
    auto row = ui::textRow("Tool", key);
    auto successRow = ui::textRow("Success", success);
    auto field = control<mayo::TextFieldView>(row);
    int finished = 0;
    bool timedOut = false;
    auto finish = [&] {
        if (++finished == 3) app.quit();
    };
    const auto pending = key.setString("new", [finish](gx::WriteResult result) {
        CHECK(result.status == gx::WriteStatus::Failure);
        CHECK(result.error.find("mock tool refused") != std::string::npos);
        finish();
    });
    CHECK(pending.pending());
    const auto spawnPending = missing.setString("new", [finish](gx::WriteResult result) {
        CHECK(result.status == gx::WriteStatus::Failure);
        CHECK_FALSE(result.error.empty());
        finish();
    });
    CHECK(spawnPending.pending());
    const auto successPending = success.setString("new", [finish](gx::WriteResult result) {
        CHECK(result.ok());
        finish();
    });
    CHECK(successPending.pending());
    CHECK_FALSE(field->isEnabled());
    CHECK(finished == 0);
    app.after(5, [&app, &timedOut] {
        timedOut = true;
        app.quit();
    });
    CHECK(app.run() == 0);
    gx::stop();
    CHECK_FALSE(timedOut);
    CHECK(finished == 3);
    CHECK(field->isEnabled());
    CHECK(field->text() == "old");
    CHECK(detail(row).find("mock tool refused") != std::string::npos);
    CHECK(control<mayo::TextFieldView>(successRow)->text() == "new");
    CHECK(success.getString() == "new");
}
