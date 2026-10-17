#define DOCTEST_CONFIG_IMPLEMENT_WITH_MAIN
#include <doctest/doctest.h>
#include "editor.hpp"
#include "i18n.hpp"
using namespace mayo;
using forma::text::TextEditor;

TEST_CASE("UTF-8 navigation, selection boundaries and deletion") {
    TextEditor editor;
    editor.setValue("A你好🙂Z");
    editor.setSelection(editor.value().size(), editor.value().size());
    CHECK(editor.characterCount() == 5);
    CHECK(editor.location() == std::pair<size_t, size_t>{1, 6});
    editor.onKeyDown({Key::Left});
    editor.onKeyDown({Key::Backspace});
    CHECK(editor.value() == "A你好Z");
    editor.onKeyDown({Key::Z, Modifiers::Ctrl});
    CHECK(editor.value() == "A你好🙂Z");
    editor.setSelection(2, 4);  // Inside multi-byte sequences: snap to code point starts.
    CHECK(editor.anchor() == 1);
    CHECK(editor.caret() == 4);
    editor.insert("文");
    CHECK(editor.value() == "A文好🙂Z");
}
TEST_CASE("literal find wraps, replacement is one undo step") {
    TextEditor editor;
    editor.setValue("one two one\n你好 one");
    CHECK(editor.find("one"));
    CHECK(editor.anchor() == 0);
    CHECK(editor.find("one"));
    CHECK(editor.anchor() == 8);
    CHECK(editor.find("one"));
    CHECK(editor.anchor() == 19);
    CHECK(editor.find("one"));
    CHECK(editor.anchor() == 0);
    CHECK_FALSE(editor.find("missing"));
    CHECK(editor.replaceAll("one", "once") == 3);
    CHECK(editor.value() == "once two once\n你好 once");
    editor.onKeyDown({Key::Z, Modifiers::Ctrl});
    CHECK(editor.value() == "one two one\n你好 one");
    editor.onKeyDown({Key::Z, Modifiers::Ctrl | Modifiers::Shift});
    CHECK(editor.value() == "once two once\n你好 once");
    CHECK(editor.replaceAll("", "x") == 0);
    CHECK(editor.replaceAll("once", "once once") == 3);
}
TEST_CASE("line and column count Unicode characters and selection replacement") {
    TextEditor editor;
    editor.setValue("first\n你好\n");
    CHECK(editor.lineCount() == 3);
    editor.setSelection(12, 12);
    CHECK(editor.location() == std::pair<size_t, size_t>{2, 3});
    editor.selectAll();
    editor.onTextInput({"new\n文"});
    CHECK(editor.value() == "new\n文");
    CHECK(editor.lineCount() == 2);
    CHECK(editor.characterCount() == 5);
    editor.onKeyDown({Key::Enter});
    editor.onKeyDown({Key::Tab});
    CHECK(editor.value() == "new\n文\n    ");
}
TEST_CASE("unwrapped documents measure their real width") {
    TextEditor editor;
    editor.setValue(std::string(1000, 'W'));
    editor.setWrap(false);
    const auto unwrapped = editor.measure({300, 500});
    CHECK(unwrapped.width > 1500);
    editor.setWrap(true);
    const auto wrapped = editor.measure({300, 500});
    CHECK(wrapped.width <= 300);
    CHECK(wrapped.height > unwrapped.height);
}

TEST_CASE("Chinese UI translation never rewrites document text or verbatim labels") {
    setUiTranslator(forma::text::translate);
    TextEditor editor("Settings");
    CHECK(editor.value() == "Settings");
    editor.setValue("Save");
    CHECK(editor.value() == "Save");
    editor.selectAll();
    editor.insert("Plain Text");
    CHECK(editor.value() == "Plain Text");
    editor.onKeyDown({Key::Z, Modifiers::Ctrl});
    CHECK(editor.value() == "Save");
    auto filename = Label("").verbatim();
    filename.setText("Settings");
    CHECK(filename.text() == "Settings");
    CHECK(uiText("Find & Replace") == "查找与替换");
    CHECK(uiText("Wrap lines") == "自动换行");
    CHECK(forma::text::message("Replaced {count} matches", {{"count", "12"}}) == "已替换 12 处");
    CHECK(forma::text::message("Saved {name}", {{"name", "Settings"}}) == "已保存 Settings");
    setUiTranslator(nullptr);
}
