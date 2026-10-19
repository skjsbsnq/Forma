#include <doctest/doctest.h>

#include <mayonnaise/mayonnaise.hpp>

#include "test_support.hpp"

using namespace mayo;

namespace {
struct ScrollFixture {
    test::TestHost host;
    std::shared_ptr<test::Box> content = std::make_shared<test::Box>(Size{300, 2000});
    std::shared_ptr<ScrollViewView> scroll = std::make_shared<ScrollViewView>(content);
    ScrollFixture() { host.attach(scroll, {300, 400}); }
    void run(double seconds) {
        for (double t = 0; t < seconds; t += 1.0 / 60.0) {
            host.clock += 1.0 / 60.0;
            scroll->step(host.clock);
        }
    }
    ScrollEvent precise(float dy) { return ScrollEvent{{10, 10}, {0, dy}, true, Modifiers::None}; }
};
}  // namespace

TEST_CASE("ScrollView: wheel notches glide to a clamped target") {
    ScrollFixture f;
    CHECK(f.scroll->onScroll(ScrollEvent{{10, 10}, {0, 120}, false, Modifiers::None}));
    f.run(0.5);
    CHECK(f.scroll->offset().y == doctest::Approx(120));
    CHECK(f.content->frame().y == doctest::Approx(-120));
    f.scroll->onScroll(ScrollEvent{{10, 10}, {0, -1000}, false, Modifiers::None});
    f.run(0.5);
    CHECK(f.scroll->offset().y == doctest::Approx(0));
    // Already at the top: further upward scrolling is passed to the parent.
    CHECK_FALSE(f.scroll->onScroll(ScrollEvent{{10, 10}, {0, -40}, false, Modifiers::None}));
}

TEST_CASE("ScrollView: touchpad momentum continues after the gesture and stops") {
    ScrollFixture f;
    for (int i = 0; i < 6; ++i) {
        f.host.clock += 0.016;
        f.scroll->onScroll(f.precise(20));
    }
    float afterGesture = f.scroll->offset().y;
    CHECK(afterGesture == doctest::Approx(120));
    f.run(3.0);
    CHECK(f.scroll->offset().y > afterGesture + 50);  // coasted
    CHECK_FALSE(f.scroll->isAnimating());
    CHECK(f.scroll->offset().y <= 1600.0f);
}

TEST_CASE("ScrollView: a finger that slows down and rests doesn't fling") {
    ScrollFixture f;
    // Fast, then decelerating to a stop before the fingers lift.
    for (float d : {30.0f, 30.0f, 25.0f, 15.0f, 6.0f, 2.0f, 0.5f}) {
        f.host.clock += 0.016;
        f.scroll->onScroll(f.precise(d));
    }
    f.host.clock += 0.09;  // resting, then lifted: no events for a while
    for (float d : {0.3f}) {
        f.scroll->onScroll(f.precise(d));
    }
    const float atRest = f.scroll->offset().y;
    f.run(2.0);
    CHECK(f.scroll->offset().y == doctest::Approx(atRest).epsilon(0.001));
}

TEST_CASE("ScrollView: pausing during a slow drag doesn't coast") {
    ScrollFixture f;
    for (int i = 0; i < 5; ++i) {
        f.host.clock += 0.016;
        f.scroll->onScroll(f.precise(1));
    }
    const float before = f.scroll->offset().y;
    f.run(0.3);  // longer than the gesture-end timeout, fingers still down
    CHECK(f.scroll->offset().y == doctest::Approx(before));
    CHECK_FALSE(f.scroll->isAnimating());
}

TEST_CASE("ScrollView: rubber-band overscroll springs back") {
    ScrollFixture f;
    for (int i = 0; i < 5; ++i) {
        f.host.clock += 0.016;
        f.scroll->onScroll(f.precise(-30));
    }
    CHECK(f.scroll->offset().y < 0);
    CHECK(f.scroll->offset().y > -80);  // resistance applied: 150 pt of finger travel, about half of it shown
    f.run(2.0);
    CHECK(f.scroll->offset().y == doctest::Approx(0).epsilon(0.01));
    CHECK_FALSE(f.scroll->isAnimating());
}

TEST_CASE("ScrollView: bounces at the bottom edge too") {
    ScrollFixture f;
    f.scroll->scrollTo({0, 1600}, false);
    CHECK(f.scroll->offset().y == doctest::Approx(1600));
    for (int i = 0; i < 5; ++i) {
        f.host.clock += 0.016;
        CHECK(f.scroll->onScroll(f.precise(30)));
    }
    CHECK(f.scroll->offset().y > 1600);
    CHECK(f.scroll->offset().y < 1680);
    f.run(2.0);
    CHECK(f.scroll->offset().y == doctest::Approx(1600).epsilon(0.001));
}

TEST_CASE("ScrollView: a fling into the edge bounces and settles") {
    ScrollFixture f;
    f.scroll->scrollTo({0, 1500}, false);
    for (int i = 0; i < 6; ++i) {
        f.host.clock += 0.016;
        f.scroll->onScroll(f.precise(40));
    }
    float maxSeen = 0;
    for (double t = 0; t < 3.0; t += 1.0 / 60.0) {
        f.host.clock += 1.0 / 60.0;
        f.scroll->step(f.host.clock);
        maxSeen = std::max(maxSeen, f.scroll->offset().y);
    }
    CHECK(maxSeen > 1610);  // went past the end ...
    CHECK(maxSeen < 1720);  // ... but not absurdly far
    CHECK(f.scroll->offset().y == doctest::Approx(1600).epsilon(0.001));
    CHECK_FALSE(f.scroll->isAnimating());
}

TEST_CASE("ScrollView: an explicit end starts momentum at once; resting fingers hold") {
    ScrollFixture f;
    auto ev = [](float dy, ScrollPhase phase = ScrollPhase::Changed) {
        return ScrollEvent{{10, 10}, {0, dy}, true, Modifiers::None, phase, true};
    };
    for (int i = 0; i < 5; ++i) {
        f.host.clock += 0.016;
        f.scroll->onScroll(ev(-30));  // stretch past the top
    }
    const float stretched = f.scroll->offset().y;
    CHECK(stretched < 0);
    f.run(0.3);  // fingers resting: longer than the guessing timeout, but the platform reports the end
    CHECK(f.scroll->offset().y == doctest::Approx(stretched));
    f.scroll->onScroll(ev(0, ScrollPhase::Ended));
    CHECK(f.scroll->isAnimating());
    f.run(1.5);
    CHECK(f.scroll->offset().y == doctest::Approx(0).epsilon(0.01));
}

TEST_CASE("ScrollView: content that fits still bounces, unless nested in another scroller") {
    test::TestHost host;
    auto small = std::make_shared<test::Box>(Size{300, 100});
    auto scroll = std::make_shared<ScrollViewView>(small);
    host.attach(scroll, {300, 400});
    CHECK(scroll->onScroll(ScrollEvent{{10, 10}, {0, 20}, true, Modifiers::None}));
    CHECK(scroll->offset().y > 0);
    // Wheel notches have nothing to do.
    CHECK_FALSE(scroll->onScroll(ScrollEvent{{10, 10}, {0, 40}, false, Modifiers::None}));

    test::TestHost host2;
    auto inner = std::make_shared<ScrollViewView>(std::make_shared<test::Box>(Size{300, 100}));
    auto outer = std::make_shared<ScrollViewView>(inner);
    host2.attach(outer, {300, 400});
    CHECK_FALSE(inner->onScroll(ScrollEvent{{10, 10}, {0, 20}, true, Modifiers::None}));
}

TEST_CASE("TextField editing") {
    test::TestHost host;
    auto tf = std::make_shared<TextFieldView>("placeholder");
    host.attach(tf, {300, 24});
    tf->insert("hello world");
    CHECK(tf->text() == "hello world");
    tf->deleteBackward(true);
    CHECK(tf->text() == "hello ");
    tf->deleteBackward(false);
    CHECK(tf->text() == "hello");
    tf->moveCaret(0, false);
    tf->insert("> ");
    CHECK(tf->text() == "> hello");
    tf->selectAll();
    tf->insert("é");
    CHECK(tf->text() == "é");
    tf->deleteBackward(false);  // removes a 2-byte code point
    CHECK(tf->text().empty());

    tf->insert("one\ntwo");  // newlines are stripped in single-line fields
    CHECK(tf->text() == "onetwo");

    // Clipboard through the host.
    host.dispatcher.setFocus(tf.get(), false);
    tf->selectAll();
    CHECK(tf->onKeyDown(KeyEvent{Key::C, Modifiers::Ctrl}));
    CHECK(host.clip == "onetwo");
    tf->moveCaret(tf->text().size(), false);
    tf->onKeyDown(KeyEvent{Key::V, Modifiers::Ctrl});
    CHECK(tf->text() == "onetwoonetwo");
    tf->onKeyDown(KeyEvent{Key::Left, Modifiers::Shift});
    CHECK(tf->selectionAnchor() == 12);
    CHECK(tf->caret() == 11);
}

TEST_CASE("Secure fields never copy") {
    test::TestHost host;
    auto tf = std::make_shared<TextFieldView>("");
    host.attach(tf, {300, 24});
    tf->setSecure(true);
    tf->insert("hunter2");
    tf->selectAll();
    tf->onKeyDown(KeyEvent{Key::C, Modifiers::Ctrl});
    CHECK(host.clip.empty());
}

TEST_CASE("Secure field input purpose follows focus and live mode changes") {
    test::TestHost host;
    auto tf = std::make_shared<TextFieldView>("");
    host.attach(tf, {300, 24});
    tf->setSecure(true);
    CHECK_FALSE(host.secureTextInput); // An unfocused field cannot alter the active input method.
    host.dispatcher.setFocus(tf.get(), false);
    CHECK(host.textInputActive);
    CHECK(host.secureTextInput);
    CHECK(host.secureWhenStarted);
    tf->setSecure(false);
    CHECK_FALSE(host.secureTextInput);
    tf->setSecure(true);
    CHECK(host.secureTextInput);
    host.dispatcher.setFocus(nullptr, false);
    CHECK_FALSE(host.textInputActive);
    CHECK_FALSE(host.secureTextInput);
    tf->setSecure(false);
    host.dispatcher.setFocus(tf.get(), false);
    CHECK(host.textInputActive);
    CHECK_FALSE(host.secureWhenStarted);
}

TEST_CASE("Toggle, checkbox, slider and segmented control state") {
    test::TestHost host;
    bool last = false;
    auto toggle = Toggle("x").onChange([&](bool on) { last = on; });
    host.attach(toggle.node(), {200, 30});
    toggle->onKeyDown(KeyEvent{Key::Space});
    CHECK(toggle.on());
    CHECK(last);

    float value = 0;
    auto slider = Slider(0.5f).step(0.1f).onChange([&](float v) { value = v; });
    host.attach(slider.node(), {200, 30});
    slider->onKeyDown(KeyEvent{Key::Right});
    CHECK(slider.value() == doctest::Approx(0.6f));
    CHECK(value == doctest::Approx(0.6f));
    slider.setValue(5.0f);
    CHECK(slider.value() == doctest::Approx(1.0f));

    int seg = -1;
    auto sc = SegmentedControl({"a", "b", "c"}).onChange([&](int i) { seg = i; });
    host.attach(sc.node(), {300, 30});
    sc->onKeyDown(KeyEvent{Key::Right});
    CHECK(seg == 1);
    sc->onKeyDown(KeyEvent{Key::Left});
    sc->onKeyDown(KeyEvent{Key::Left});
    CHECK(sc.selectedIndex() == 0);
}

TEST_CASE("Label wraps to the proposed width") {
    auto label = Label("A fairly long sentence that will need to wrap onto several lines here");
    Size one = label.node()->measure({kInfinity, kInfinity});
    Size wrapped = label.node()->measure({120, kInfinity});
    CHECK(wrapped.width <= 120);
    CHECK(wrapped.height > one.height * 2);
    auto single = Label("A fairly long sentence").lineLimit(1);
    CHECK(single.node()->measure({50, kInfinity}).height == doctest::Approx(one.height));
}

TEST_CASE("Theme tokens differ between appearances") {
    Theme light = Theme::make(Appearance::Light);
    Theme dark = Theme::make(Appearance::Dark);
    CHECK(light.colors.windowBackground.luminance() > 0.8f);
    CHECK(dark.colors.windowBackground.luminance() < 0.1f);
    CHECK(light.colors.label.a > 0.5f);
    CHECK(light.font(TextStyle::LargeTitle).size > light.font(TextStyle::Body).size);
    CHECK(light.font(TextStyle::Headline).weight == FontWeight::Semibold);
    Theme custom = Theme::make(Appearance::Light, Color::hex(0xFF0000));
    CHECK(custom.colors.accent == Color::hex(0xFF0000));
    CHECK(custom.colors.focusRing.a < 1.0f);
}

TEST_CASE("Toolbar: trailing actions move into overflow and invoke the original command") {
    int invoked = 0;
    auto first = Button("First").width(80);
    auto second = Button("Second").width(80);
    auto last = Button("Last").width(80).onClick([&] { ++invoked; });
    auto bar = Toolbar({first, second, last});
    test::TestHost host;
    host.attach(bar.node(), {220, 40});
    CHECK(first.node()->isVisible());
    CHECK(second.node()->isVisible());
    CHECK_FALSE(last.node()->isVisible());
    auto items = bar.node()->overflowItems();
    REQUIRE(items.size() == 1);
    CHECK(items[0].title == "Last");
    items[0].action();
    CHECK(invoked == 1);
    last.setDisabled(true);
    CHECK_FALSE(bar.node()->overflowItems()[0].enabled);
    items[0].action();
    CHECK(invoked == 1);
    last.setDisabled(false);
    last.node()->setHidden(true);
    items[0].action();
    CHECK(invoked == 1);
    last.node()->setHidden(false);
    host.attach(bar.node(), {400, 40});
    CHECK(last.node()->isVisible());
    CHECK(bar.node()->overflowItems().empty());
}

TEST_CASE("Toolbar: application-hidden actions stay hidden when resizing and disabling overflow") {
    auto hidden = Button("Hidden").width(80).hidden();
    auto action = Button("Action").width(120);
    auto bar = Toolbar({hidden, action});
    test::TestHost host;
    host.attach(bar.node(), {60, 40});
    REQUIRE(bar.node()->overflowItems().size() == 1);
    CHECK_FALSE(hidden.node()->isVisible());
    bar.overflow(false);
    host.attach(bar.node(), {300, 40});
    CHECK(action.node()->isVisible());
    CHECK_FALSE(hidden.node()->isVisible());
    CHECK(bar.node()->overflowItems().empty());
}

TEST_CASE("Toolbar: grouped toggles and segments retain live state and original callbacks") {
    int changes = 0;
    auto toggle = Toggle("Notify", true).onChange([&](bool) { ++changes; });
    auto segments = SegmentedControl({"List", "Grid"}).onChange([&](int) { ++changes; });
    auto group = ToolbarGroup({toggle, segments});
    auto bar = Toolbar({group});
    test::TestHost host;
    host.attach(bar.node(), {60, 40});
    auto items = bar.node()->overflowItems();
    REQUIRE(items.size() == 3);
    CHECK(items[0].checked);
    CHECK(items[1].checked);
    items[0].action();
    items[2].action();
    CHECK_FALSE(toggle.on());
    CHECK(segments.selectedIndex() == 1);
    CHECK(changes == 2);
    items = bar.node()->overflowItems();
    CHECK_FALSE(items[0].checked);
    CHECK(items[2].checked);
    group.setDisabled(true);
    for (const auto& item : bar.node()->overflowItems()) CHECK_FALSE(item.enabled);
}

TEST_CASE("Toolbar: removed controls recover visibility and stale menu actions do not run") {
    int invoked = 0;
    auto action = Button("Action").width(120).onClick([&] { ++invoked; });
    auto bar = Toolbar({action});
    test::TestHost host;
    host.attach(bar.node(), {60, 40});
    auto items = bar.node()->overflowItems();
    REQUIRE(items.size() == 1);
    bar.remove(action);
    CHECK(action.node()->isVisible());
    items[0].action();
    CHECK(invoked == 0);
    bar.clear();
    bar.add(action);
    host.attach(bar.node(), {300, 40});
    CHECK(action.node()->isVisible());
    CHECK(bar.node()->overflowItems().empty());
}

TEST_CASE("Toolbar: overflow menu restores focus and collapsed controls leave keyboard navigation") {
    auto first = Button("First").width(80);
    auto last = Button("Last").width(80);
    auto bar = Toolbar({first, last});
    test::TestHost host;
    host.attach(bar.node(), {300, 40});
    host.setFocus(last.node().get(), true);
    host.attach(bar.node(), {120, 40});
    auto more = std::dynamic_pointer_cast<ButtonView>(bar.node()->children().back());
    REQUIRE(more);
    CHECK(host.focusedWidget() == more.get());
    host.dispatcher.keyDown({Key::Enter});
    REQUIRE(host.dispatcher.overlays().size() == 1);
    host.dispatcher.keyDown({Key::Escape});
    CHECK(host.dispatcher.overlays().empty());
    CHECK(host.focusedWidget() == more.get());
    host.dispatcher.keyDown({Key::Enter});
    REQUIRE(host.dispatcher.overlays().size() == 1);
    host.attach(bar.node(), {300, 40});
    CHECK(host.dispatcher.overlays().empty());
    CHECK(host.focusedWidget() == first.node().get());
    host.dispatcher.keyDown({Key::Right});
    CHECK(host.focusedWidget() == last.node().get());
    host.dispatcher.keyDown({Key::Home});
    CHECK(host.focusedWidget() == first.node().get());
}

TEST_CASE("Toolbar: custom inputs remain available and unnamed actions are not discarded") {
    auto search = SearchField("Search").width(90);
    auto unnamed = Button(Icon::Plus).width(30);
    auto action = Button("Action").width(80);
    auto bar = Toolbar({search, unnamed, action});
    test::TestHost host;
    host.attach(bar.node(), {160, 40});
    CHECK(search.node()->isVisible());
    CHECK(unnamed.node()->isVisible());
    REQUIRE(bar.node()->overflowItems().size() == 1);
    CHECK(bar.node()->overflowItems()[0].title == "Action");
}

TEST_CASE("Menu: keyboard activation cannot invoke a disabled item") {
    int invoked = 0;
    MenuItem disabled("Disabled", [&] { ++invoked; });
    disabled.enabled = false;
    auto menu = std::make_shared<MenuView>(std::vector<MenuItem>{disabled, MenuItem("Enabled", [&] { ++invoked; })});
    test::TestHost host;
    host.attach(menu);
    host.setFocus(menu.get(), true);
    menu->setHighlighted(0);
    host.dispatcher.keyDown({Key::Enter});
    CHECK(invoked == 0);
    host.dispatcher.keyDown({Key::Down});
    host.dispatcher.keyDown({Key::Enter});
    CHECK(invoked == 1);
}

TEST_CASE("Glass panels share their optical surface in both appearances") {
    for (auto appearance : {Appearance::Light, Appearance::Dark}) {
        const auto t = Theme::make(appearance);
        const auto glass = t.material(Material::Glass);
        for (auto role : {Material::Menu, Material::Popover, Material::Sidebar, Material::Titlebar}) {
            const auto panel = t.material(role);
            CHECK(panel.tint == glass.tint);
            CHECK(panel.bezelWidth == glass.bezelWidth);
            CHECK(panel.refraction == glass.refraction);
            CHECK(panel.rimStrength == glass.rimStrength);
            CHECK(panel.toneLow == glass.toneLow);
            CHECK(panel.toneHigh == glass.toneHigh);
            CHECK(panel.noise == glass.noise);
            CHECK(panel.blurRadius >= glass.blurRadius);
        }
    }
}

TEST_CASE("Menu columns do not shift when checks or icons are added") {
    test::TestHost host;
    auto menu = std::make_shared<MenuView>(std::vector<MenuItem>{MenuItem("Same title")});
    host.attach(menu);
    const auto plain = menu->measureContent({800, 800});
    menu->setItems({MenuItem("Same title").setChecked(true)});
    CHECK(menu->measureContent({800, 800}).width == plain.width);
    menu->setItems({MenuItem("Same title").withIcon(Icon::Document)});
    CHECK(menu->measureContent({800, 800}).width == plain.width);
    menu->setItems({MenuItem("Same title").withIcon(Icon::Document).setChecked(true)});
    CHECK(menu->measureContent({800, 800}).width == plain.width);
}

TEST_CASE("Label: .verbatim() shows the given text even when a UI translator is set") {
    // Display-only translation rewrites what the label draws; data (printer names, effect metadata, file
    // names) must come through unchanged, whether .verbatim() is applied before or after setText().
    mayo::setUiTranslator([](std::string_view text) -> std::string {
        return text == "Close" ? "关闭" : std::string(text);
    });
    auto translated = Label("Close");
    CHECK(translated->text() == "关闭");

    auto verbatim = Label("Close").verbatim();
    CHECK(verbatim->text() == "Close");

    auto later = Label("Close");
    later.verbatim();
    CHECK(later->text() == "Close");

    later.setText("Close");
    CHECK(later->text() == "Close");
    CHECK(later->sourceText() == "Close");

    mayo::setUiTranslator(nullptr);
}
