// Catalogue parsing and search, plus an end-to-end parse of the machine's AppStream cache when present.
#define DOCTEST_CONFIG_IMPLEMENT_WITH_MAIN
#include <doctest/doctest.h>

#include <filesystem>
#include <fstream>
#include <sstream>

#include "catalog.hpp"
#include "json_lite.hpp"

using namespace forma::store;

namespace {

const char* kSample = R"XML(<?xml version="1.0" encoding="UTF-8"?>
<components version="0.8" origin="flatpak">
  <component type="desktop-application">
    <id>org.example.Drawing</id>
    <name>Drawing</name>
    <name xml:lang="zh_CN">绘图</name>
    <summary>Paint things</summary>
    <summary xml:lang="zh_CN">绘制图形</summary>
    <developer_name>Example Inc.</developer_name>
    <project_license>GPL-3.0-or-later</project_license>
    <description><p>First paragraph.</p><p>Second &amp; last.</p></description>
    <launchable type="desktop-id">org.example.Drawing.desktop</launchable>
    <icon height="128" type="cached" width="128">org.example.Drawing.png</icon>
    <url type="homepage">https://example.org/</url>
    <categories>
      <category>Graphics</category>
      <category>2DGraphics</category>
    </categories>
    <screenshots>
      <screenshot type="default">
        <caption xml:lang="zh_CN">主窗口</caption>
        <image height="1080" type="source" width="1920">https://example.org/shot.png</image>
        <image height="351" type="thumbnail" width="624">https://example.org/shot-small.png</image>
      </screenshot>
    </screenshots>
    <branding><color scheme_preference="light" type="primary">#3366CC</color></branding>
    <releases>
      <release timestamp="1700000000" type="stable" version="2.1"><description><p>Fixes.</p></description></release>
    </releases>
    <content_rating type="oars-1.1">
      <content_attribute id="violence-cartoon">mild</content_attribute>
    </content_rating>
    <bundle type="flatpak" runtime="org.gnome.Platform/x86_64/50">app/org.example.Drawing/x86_64/stable</bundle>
  </component>
  <component type="runtime">
    <id>org.gnome.Platform</id>
    <name>Runtime</name>
  </component>
</components>
)XML";

}  // namespace

TEST_CASE("catalogue parses AppStream components") {
    const auto apps = parseAppStream(kSample);
    REQUIRE(apps.size() == 1);
    const AppEntry& app = apps.front();
    CHECK(app.id == "org.example.Drawing");
    CHECK(app.name == "绘图");          // zh_CN wins
    CHECK(app.summary == "绘制图形");
    CHECK(app.developer == "Example Inc.");
    CHECK(app.description.find("First paragraph.") != std::string::npos);
    CHECK(app.description.find("Second & last.") != std::string::npos);
    CHECK(app.primaryCategory() == "Graphics");
    CHECK(app.version == "2.1");
    CHECK(app.accent == "#3366CC");
    CHECK(app.runtime == "org.gnome.Platform/x86_64/50");
    CHECK(app.ageRating == 9);
    REQUIRE(app.screenshots.size() == 1);
    CHECK(app.screenshots.front().caption == "主窗口");
    CHECK(app.screenshots.front().thumbnail == "https://example.org/shot-small.png");
    CHECK(app.screenshots.front().source == "https://example.org/shot.png");
    CHECK_FALSE(app.installed);
    CHECK_FALSE(app.verified);
}

TEST_CASE("app ids are validated before use as arguments") {
    CHECK(validAppId("org.example.App"));
    CHECK(validAppId("com.tenzen.photon"));
    CHECK_FALSE(validAppId(""));
    CHECK_FALSE(validAppId("--help"));
    CHECK_FALSE(validAppId("org.example.App; rm -rf /"));
    CHECK_FALSE(validAppId("nodots"));
}

TEST_CASE("search matches names, summaries and keywords") {
    Catalog catalog;
    AppEntry a;
    a.id = "org.example.Drawing";
    a.name = "Drawing";
    a.summary = "Paint things";
    a.categories = {"Graphics"};
    catalog.apps.push_back(a);
    AppEntry b;
    b.id = "org.example.Terminal";
    b.name = "Term";
    b.keywords = {"shell"};
    b.installed = true;
    catalog.apps.push_back(b);
    catalog.byId.emplace(a.id, 0);
    catalog.byId.emplace(b.id, 1);

    auto found = catalog.search("draw");
    REQUIRE(found.size() == 1);
    CHECK(found.front()->id == a.id);
    CHECK(catalog.search("shell").size() == 1);   // keyword
    CHECK(catalog.search("graphics").size() == 1);  // category
    CHECK(catalog.search("nothing-here").empty());
    CHECK(catalog.search("").empty());

    auto installed = catalog.installedApps();
    REQUIRE(installed.size() == 1);
    CHECK(installed.front()->id == b.id);
}

TEST_CASE("sizes and dates format for the UI") {
    CHECK(formatBytes(0) == "—");
    CHECK(formatBytes(1536) == "1.5 KB");
    CHECK(formatBytes(1500000).find("MB") != std::string::npos);
    CHECK(categoryName("Graphics") == "图形与设计");
    CHECK(categoryName("UnknownCat") == "UnknownCat");
    CHECK(sectionName("discover") == "精选");
}

TEST_CASE("JSON reader handles flatpak's output") {
    const auto value = json::Value::parse(R"([{"application_id":"a.b","n":2},{"application_id":"c.d"}])");
    REQUIRE(value.isArray());
    REQUIRE(value.array().size() == 2);
    CHECK(value.array()[0]["application_id"].str() == "a.b");
    CHECK(value.array()[0]["n"].number() == 2);
    CHECK(value.array()[1]["missing"].str("fallback") == "fallback");
    CHECK(json::Value::parse("{broken").isNull());
}

TEST_CASE("matches highlight subsequences") {
    CHECK(matchPositions("Telegram", "tgm").size() == 3);
    CHECK(matchPositions("Telegram", "xyz").empty());
}

TEST_CASE("the machine's AppStream cache parses") {
    const std::string path = findAppStreamFile();
    if (path.empty()) {
        MESSAGE("no Flatpak AppStream cache on this machine; skipping");
        return;
    }
    std::ifstream in(path, std::ios::binary);
    REQUIRE(in);
    std::ostringstream buffer;
    buffer << in.rdbuf();
    const auto apps = parseAppStream(buffer.str(), iconRootFor(path));
    CHECK(apps.size() > 100);
    std::size_t withIcon = 0;
    std::size_t withScreenshot = 0;
    for (const auto& app : apps) {
        if (!app.iconPath.empty()) ++withIcon;
        if (!app.screenshots.empty()) ++withScreenshot;
    }
    CHECK(withIcon > 10);
    CHECK(withScreenshot > 10);
}
