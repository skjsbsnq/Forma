#include <doctest/doctest.h>

#include "test_support.hpp"
#include "core/window_root.hpp"

using namespace mayo;
using test::Box;

namespace {
std::shared_ptr<Box> box(float w, float h, bool eh = false, bool ev = false) {
    return std::make_shared<Box>(Size{w, h}, eh, ev);
}
}  // namespace

TEST_CASE("Fullscreen window content occupies the hidden title bar") {
    WindowRootView root;
    auto content = box(100, 100);
    root.setContent(content);
    root.setWindowState(false, true, false, false);
    root.measure({800, 600});
    root.arrange({0, 0, 800, 600});
    CHECK(content->frame().y == 32);
    root.setWindowState(false, true, false, true);
    root.measure({800, 600});
    root.arrange({0, 0, 800, 600});
    CHECK(root.titleBarHeight() == 0);
    CHECK(content->frame() == Rect{0, 0, 800, 600});
    root.setWindowState(false, true, false, false);
    root.measure({800, 600});
    root.arrange({0, 0, 800, 600});
    CHECK(content->frame().y == 32);
}

TEST_CASE("VStack stacks children with spacing and leading alignment") {
    auto a = box(100, 20);
    auto b = box(50, 30);
    auto stack = VStack({a, b}).spacing(10);
    auto node = stack.node();
    Size s = node->measure({kInfinity, kInfinity});
    CHECK(s == Size{100, 60});
    node->arrange({0, 0, 300, 300});
    CHECK(a->frame() == Rect{0, 0, 100, 20});
    CHECK(b->frame() == Rect{0, 30, 50, 30});
}

TEST_CASE("Center alignment centres the group on both axes") {
    auto a = box(100, 20);
    auto b = box(50, 30);
    auto stack = VStack({a, b}).spacing(10).align(Alignment::Center);
    stack.node()->measure({300, 300});
    stack.node()->arrange({0, 0, 300, 300});
    CHECK(a->frame() == Rect{100, 120, 100, 20});
    CHECK(b->frame() == Rect{125, 150, 50, 30});
}

TEST_CASE("Padding insets content") {
    auto a = box(100, 20);
    auto stack = VStack({a}).padding(24);
    Size s = stack.node()->measure({kInfinity, kInfinity});
    CHECK(s == Size{148, 68});
    stack.node()->arrange({0, 0, 148, 68});
    CHECK(a->frame() == Rect{24, 24, 100, 20});
}

TEST_CASE("Spacer pushes siblings apart in an HStack") {
    auto a = box(40, 20);
    auto b = box(60, 10);
    auto stack = HStack({a, Spacer(), b});
    Size s = stack.node()->measure({400, 100});
    CHECK(s.width == 400);
    CHECK(s.height == 20);
    stack.node()->arrange({0, 0, 400, 20});
    CHECK(a->frame().x == 0);
    CHECK(b->frame().right() == doctest::Approx(400));
    // Cross axis: HStack centres vertically by default.
    CHECK(b->frame().y == 5);
}

TEST_CASE("Expanding children share free space by flex") {
    auto a = box(0, 10, true);
    auto b = box(0, 10, true);
    auto fixed = box(100, 10);
    auto stack = HStack({a, fixed, b}).spacing(0);
    b->setFlex(3);
    stack.node()->measure({500, 10});
    stack.node()->arrange({0, 0, 500, 10});
    CHECK(a->frame().width == doctest::Approx(100));
    CHECK(b->frame().width == doctest::Approx(300));
    CHECK(fixed->frame().x == doctest::Approx(100));
}

TEST_CASE("Frame modifiers fix and clamp sizes") {
    auto a = box(100, 20);
    a->setFixedWidth(30);
    CHECK(a->measure({kInfinity, kInfinity}) == Size{30, 20});
    auto b = box(10, 10);
    b->setMinSize(50, 40);
    CHECK(b->measure({kInfinity, kInfinity}) == Size{50, 40});
    auto c = box(500, 10);
    c->setMaxSize(200, std::nullopt);
    CHECK(c->measure({kInfinity, kInfinity}).width == 200);
    auto d = box(10, 10);
    d->setMaxSize(kInfinity, std::nullopt);
    CHECK(d->expands(Axis::Horizontal));
    CHECK_FALSE(d->expands(Axis::Vertical));
}

TEST_CASE("Cross-expanding children stretch; Divider orientation follows stack") {
    auto a = box(100, 20);
    Divider div;
    auto stack = VStack({a, div}).spacing(0);
    Size s = stack.node()->measure({300, 300});
    CHECK(s.width == 300);  // divider wants the full width
    stack.node()->arrange({0, 0, 300, 300});
    CHECK(div.node()->frame().width == 300);
    CHECK(div.node()->frame().height == 1);

    Divider vdiv;
    auto h = HStack({box(10, 30), vdiv});
    h.node()->measure({300, 30});
    h.node()->arrange({0, 0, 300, 30});
    CHECK(vdiv.node()->frame().width == 1);
    CHECK(vdiv.node()->frame().height == 30);
}

TEST_CASE("Hidden children take no space") {
    auto a = box(100, 20);
    auto b = box(100, 20);
    auto c = box(100, 20);
    auto stack = VStack({a, b, c}).spacing(10);
    b->setHidden(true);
    CHECK(stack.node()->measure({kInfinity, kInfinity}).height == 50);
}

TEST_CASE("ZStack overlays and aligns") {
    auto big = box(200, 100);
    auto small = box(20, 10);
    auto z = ZStack({big, small});
    CHECK(z.node()->measure({kInfinity, kInfinity}) == Size{200, 100});
    z.node()->arrange({0, 0, 200, 100});
    CHECK(small->frame() == Rect{90, 45, 20, 10});
}

TEST_CASE("Measure cache is invalidated by layout changes") {
    auto a = box(100, 20);
    auto stack = VStack({a});
    CHECK(stack.node()->measure({kInfinity, kInfinity}).height == 20);
    stack.add(box(10, 30));
    CHECK(stack.node()->measure({kInfinity, kInfinity}).height == 58);
}

TEST_CASE("Nested stacks propagate expansion") {
    auto inner = HStack({box(10, 10), Spacer()});
    auto outer = VStack({inner, box(50, 50)});
    CHECK(inner.node()->expands(Axis::Horizontal));
    outer.node()->measure({400, 400});
    outer.node()->arrange({0, 0, 400, 400});
    CHECK(inner.node()->frame().width == 400);
}
