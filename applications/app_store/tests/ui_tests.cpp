#include <doctest/doctest.h>

#include "store_ui.hpp"

using namespace mayo;
using namespace forma::store;

namespace {
template <typename T, typename Predicate>
T* findWidget(Widget& root, Predicate matches) {
    if (auto* widget = dynamic_cast<T*>(&root); widget && matches(*widget)) return widget;
    for (const auto& child : root.children())
        if (auto* widget = findWidget<T>(*child, matches)) return widget;
    return nullptr;
}

void addApp(StoreModel& model, std::string id) {
    AppEntry entry;
    entry.id = std::move(id);
    entry.name = "Example";
    entry.summary = "Example application";
    for (int i = 0; i < 30; ++i) entry.description += "A paragraph that describes this application.\n";
    entry.releaseNotes = entry.description;
    model.catalog.byId[entry.id] = model.catalog.apps.size();
    model.catalog.apps.push_back(std::move(entry));
}
}

TEST_CASE("Store description can collapse after expanding") {
    StoreModel model;
    addApp(model, "org.example.App");
    const auto page = appPage(model, "org.example.App");
    auto layout = [&] {
        const auto size = page->measure({600, kInfinity});
        page->arrange({0, 0, 600, size.height});
        return size.height;
    };
    const float collapsed = layout();
    auto* toggle = findWidget<ButtonView>(*page, [](const auto& b) { return b.title() == "更多"; });
    REQUIRE(toggle);
    REQUIRE(toggle->isVisible());
    toggle->click();
    CHECK(layout() > collapsed);
    CHECK(toggle->title() == "收起");
    CHECK(toggle->isVisible());
    toggle->click();
    CHECK(layout() == doctest::Approx(collapsed));
    CHECK(toggle->title() == "更多");
}

TEST_CASE("Store release notes reserve their measured text height") {
    StoreModel model;
    addApp(model, "org.example.App");
    const auto page = appPage(model, "org.example.App");
    const auto& text = model.catalog.apps.front().releaseNotes;
    auto* notes = findWidget<LabelView>(*page, [&](const auto& l) { return l.text() == text && l.parent() &&
        l.parent()->children().size() == 3; });
    REQUIRE(notes);
    for (const float width : {260.0f, 900.0f}) {
        const auto textSize = notes->measure({width, kInfinity});
        const auto blockSize = notes->parent()->measure({width, kInfinity});
        CHECK(blockSize.height >= textSize.height + 30);
    }
}

TEST_CASE("Store hero action near the left edge opens the app instead of changing slides") {
    StoreModel model;
    const auto& ids = heroOrder();
    REQUIRE_FALSE(ids.empty());
    addApp(model, ids.front());
    for (int i = 0; i < 8; ++i) addApp(model, "org.example.App" + std::to_string(i));
    std::string opened;
    model.callbacks.openApp = [&](std::string id) { opened = std::move(id); };
    const auto page = discoverPage(model);
    page->measure({600, kInfinity});
    page->arrange({0, 0, 600, 2000});
    REQUIRE_FALSE(page->children().empty());
    const auto& hero = page->children().front();
    PointerEvent event{};
    event.button = MouseButton::Left;
    event.position = {hero->frame().x + 35, hero->frame().bottom() - 45};
    hero->onPointerDown(event);
    hero->onPointerUp(event);
    CHECK(opened == ids.front());
    CHECK(model.heroIndex == 0);
}
