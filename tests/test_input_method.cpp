#include <doctest/doctest.h>

#include "mayonnaise/mayonnaise.hpp"
#include "test_support.hpp"

using namespace mayo;

namespace {
struct PopupTestHost : test::TestHost {
    PopupTestHost() {
        dispatcher.onOverlayDismissed = [](Widget* widget) {
            if (auto* popup = dynamic_cast<PopupHost*>(widget)) popup->popupDismissed();
        };
    }
    void showOverlay(std::shared_ptr<Widget> overlay) override {
        TestHost::showOverlay(overlay);
        const auto size = overlay->measure({kInfinity, kInfinity});
        auto* popup = dynamic_cast<PopupHost*>(overlay.get());
        overlay->arrange(popup ? popup->placePopup(windowSize(), size) : Rect{100, 100, size.width, size.height});
    }
};
}

TEST_CASE("Input method preedit is separate from committed text and form submission") {
    test::TestHost host;
    auto field = TextField().text("旧文稿");
    int changes = 0, submissions = 0;
    field.onChange([&](const auto&) { ++changes; }).onSubmit([&](const auto&) { ++submissions; });
    host.attach(field.node());
    host.setFocus(field.node().get(), true);
    field.node()->selectAll();
    host.dispatcher.textEditing({"新的拼音", 1, 2});
    CHECK(field.node()->isComposing());
    CHECK(field.value() == "旧文稿");
    CHECK(changes == 0);
    CHECK(host.dispatcher.keyDown({Key::Enter}));
    CHECK(host.dispatcher.keyDown({Key::Escape}));
    CHECK(host.dispatcher.keyDown({Key::Left}));
    CHECK(host.dispatcher.keyDown({Key::S, Modifiers::Ctrl}));
    CHECK(submissions == 0);
    host.dispatcher.textInput({"新文稿"});
    CHECK_FALSE(field.node()->isComposing());
    CHECK(field.value() == "新文稿");
    CHECK(changes == 1);
    host.dispatcher.keyDown({Key::Enter});
    CHECK(submissions == 1);
}

TEST_CASE("Cancelling preedit preserves the original selection and document") {
    test::TestHost host;
    auto field = TextField().text("中文文件名");
    host.attach(field.node());
    host.setFocus(field.node().get(), true);
    field.node()->selectAll();
    host.dispatcher.textEditing({"候选", -5, 999});
    host.dispatcher.textEditing({});
    CHECK_FALSE(field.node()->isComposing());
    CHECK(field.value() == "中文文件名");
    CHECK(field.node()->caret() == field.value().size());
    CHECK(field.node()->selectionAnchor() == 0);
    host.dispatcher.textEditing({"另一个候选", 2, 1});
    field.setText("程序更新");
    CHECK_FALSE(field.node()->isComposing());
    CHECK(field.value() == "程序更新");
}

TEST_CASE("Focus changes and unavailable ancestors cancel composition immediately") {
    test::TestHost host;
    auto first = TextField().text("保留"), second = TextField();
    auto root = VStack({first, second});
    host.attach(root.node());
    host.setFocus(first.node().get(), true);
    host.dispatcher.textEditing({"输入中", 3, 0});
    first.node()->setDisabled(true);
    CHECK_FALSE(first.node()->isComposing());
    CHECK(host.dispatcher.focused() == second.node().get());
    CHECK(first.value() == "保留");
    host.dispatcher.textEditing({"第二处", 1, 1});
    root.node()->setHidden(true);
    CHECK_FALSE(second.node()->isComposing());
    CHECK(host.dispatcher.focused() == nullptr);
    host.dispatcher.textInput({"不能写入隐藏字段"});
    CHECK(second.value().empty());
}

TEST_CASE("Secure preedit never calls change callbacks and is cleared with sensitive input") {
    test::TestHost host;
    auto field = TextField().secure().text("test-only-secret");
    int changes = 0;
    field.onChange([&](const auto&) { ++changes; });
    host.attach(field.node());
    host.setFocus(field.node().get(), true);
    host.dispatcher.textEditing({"fabricated-preedit-secret", 2, 1});
    CHECK(field.node()->isComposing());
    CHECK(changes == 0);
    field.node()->clearSensitive();
    CHECK_FALSE(field.node()->isComposing());
    CHECK(field.value().empty());
    CHECK(changes == 0);
    host.dispatcher.textEditing({"new-fabricated-preedit", 1, 0});
    field.node()->setSecure(false);
    CHECK_FALSE(field.node()->isComposing());
}

TEST_CASE("An unrelated popup owns focus and restores the previous field") {
    PopupTestHost host;
    auto first = TextField().text("original");
    host.attach(first.node());
    host.setFocus(first.node().get(), true);
    host.dispatcher.textEditing({"cancel-on-popup", 0, 0});
    auto inner = TextField();
    auto popup = std::make_shared<PopoverView>(VStack({inner}).node());
    popup->showFrom(*first.node());
    CHECK_FALSE(first.node()->isComposing());
    CHECK(host.dispatcher.focused() == inner.node().get());
    host.setFocus(first.node().get(), true);
    CHECK(host.dispatcher.focused() == inner.node().get());
    host.dispatcher.textInput({"popup only"});
    CHECK(first.value() == "original");
    CHECK(inner.value() == "popup only");
    popup->dismiss();
    CHECK(host.dispatcher.focused() == first.node().get());
    CHECK(host.dispatcher.focusVisible());
}

TEST_CASE("Keyboard enters submenus, selects an action, and restores the opening control") {
    PopupTestHost host;
    auto button = Button("菜单");
    host.attach(button.node());
    host.setFocus(button.node().get(), true);
    int first = 0, second = 0;
    auto menu = std::make_shared<MenuView>(std::vector<MenuItem>{
        MenuItem("子菜单").withSubmenu({MenuItem("第一项", [&] { ++first; }), MenuItem("第二项", [&] { ++second; })})});
    menu->showFrom(*button.node());
    host.dispatcher.keyDown({Key::Down});
    CHECK(host.dispatcher.overlays().size() == 2);
    CHECK(host.dispatcher.focused() == menu.get());
    host.dispatcher.keyDown({Key::Right});
    CHECK(host.dispatcher.focused() == host.dispatcher.overlays().back().get());
    host.dispatcher.keyDown({Key::Down});
    host.dispatcher.keyDown({Key::Enter});
    CHECK(first == 0);
    CHECK(second == 1);
    CHECK(host.dispatcher.overlays().empty());
    CHECK(host.dispatcher.focused() == button.node().get());
    CHECK(host.dispatcher.focusVisible());
}

TEST_CASE("Left and Escape dismiss child menus without losing parent focus") {
    PopupTestHost host;
    auto button = Button("菜单");
    host.attach(button.node());
    host.setFocus(button.node().get(), true);
    auto menu = std::make_shared<MenuView>(
        std::vector<MenuItem>{MenuItem("子菜单").withSubmenu({MenuItem("项目")})});
    menu->showFrom(*button.node());
    host.dispatcher.keyDown({Key::Down});
    host.dispatcher.keyDown({Key::Right});
    host.dispatcher.keyDown({Key::Left});
    CHECK(host.dispatcher.overlays().size() == 1);
    CHECK(host.dispatcher.focused() == menu.get());
    host.dispatcher.keyDown({Key::Right});
    host.dispatcher.keyDown({Key::Escape});
    CHECK(host.dispatcher.overlays().size() == 1);
    CHECK(host.dispatcher.focused() == menu.get());
    host.dispatcher.keyDown({Key::Right});
    host.dispatcher.pointerDown({{790, 580}, MouseButton::Left, 1});
    CHECK(host.dispatcher.overlays().empty());
    CHECK(host.dispatcher.focused() == button.node().get());
}
