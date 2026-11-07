#define DOCTEST_CONFIG_IMPLEMENT_WITH_MAIN
#include <doctest/doctest.h>
#include "editor.hpp"
#include "file_io.hpp"
#include "i18n.hpp"

#include <fcntl.h>
#include <sys/stat.h>
#include <unistd.h>
#include <filesystem>
#include <fstream>

namespace forma::text {
struct EditorTestAccess {
    static size_t historyBytes(const TextEditor& editor) { return editor.m_historyBytes; }
    static size_t layoutBuilds(const TextEditor& editor) { return editor.m_layoutBuilds; }
    static size_t caretBuilds(const TextEditor& editor) { return editor.m_caretBuilds; }
    static size_t hit(const TextEditor& editor, mayo::Point point) { return editor.at(point); }
    static mayo::Point caretPosition(const TextEditor& editor) { return editor.caretPosition(); }
    static mayo::Point inputPosition(const TextEditor& editor) { return editor.inputPosition(); }
    static std::pair<size_t, size_t> preeditRange(const TextEditor& editor) {
        return {editor.m_preeditStart, editor.m_preeditEnd};
    }
    static const std::string& preedit(const TextEditor& editor) { return editor.m_preedit; }
    static const mayo::Font& font(const TextEditor& editor) { return editor.font(); }
};
}  // namespace forma::text

namespace {
namespace fs = std::filesystem;
struct Scratch {
    fs::path root;
    Scratch() {
        char pattern[] = "/tmp/forma-editor-test-XXXXXX";
        auto* dir = ::mkdtemp(pattern);
        REQUIRE(dir != nullptr);
        root = dir;
    }
    ~Scratch() {
        std::error_code ignored;
        fs::remove_all(root, ignored);
    }
    fs::path path(std::string_view name = "document.txt") const { return root / name; }
};
void fixture(const fs::path& path, std::string_view value) {
    std::ofstream stream(path, std::ios::binary);
    stream.write(value.data(), static_cast<std::streamsize>(value.size()));
    stream.close();
    REQUIRE(stream.good());
}
std::string contents(const fs::path& path) {
    std::string value;
    REQUIRE(forma::text::readFile(path, value));
    return value;
}
struct stat metadata(const fs::path& path) {
    struct stat value{};
    REQUIRE(::stat(path.c_str(), &value) == 0);
    return value;
}
void noTemporaries(const Scratch& scratch) {
    for (const auto& entry : fs::directory_iterator(scratch.root))
        CHECK_FALSE(entry.path().filename().string().starts_with(".forma-save-"));
}
}  // namespace
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

TEST_CASE("atomic save failures preserve original bytes, inode, permissions and baseline") {
    using namespace forma::text;
    Scratch scratch;
    const auto path = scratch.path();
    fixture(path, "original\r\n你好\rfinal\n");
    REQUIRE(::chmod(path.c_str(), 0640) == 0);
    const auto initial = inspectFile(path);
    REQUIRE(initial);
    const auto old = metadata(path);
    for (auto stage : {SaveStage::Write, SaveStage::Flush, SaveStage::Sync, SaveStage::Close, SaveStage::Rename}) {
        CAPTURE(stage);
        auto result =
            saveFile(path, std::string(80000, 'x'), initial.version, [stage](SaveStage at) { return at == stage; });
        CHECK(result.status == FileStatus::Error);
        CHECK(contents(path) == "original\r\n你好\rfinal\n");
        CHECK(metadata(path).st_ino == old.st_ino);
        CHECK((metadata(path).st_mode & 0777) == 0640);
        CHECK(inspectFile(path).version == initial.version);
        noTemporaries(scratch);
    }
    auto result = saveFile(path, "new\r\n文\r", initial.version);
    REQUIRE(result);
    CHECK(contents(path) == "new\r\n文\r");
    CHECK(metadata(path).st_ino != old.st_ino);
    CHECK(metadata(path).st_uid == old.st_uid);
    CHECK(metadata(path).st_gid == old.st_gid);
    CHECK((metadata(path).st_mode & 0777) == 0640);
    CHECK(result.version == inspectFile(path).version);
    CHECK(saveFile(path, "second", result.version));
    noTemporaries(scratch);
}

TEST_CASE("save conflicts cover changed content, replacement identity, deletion and save-as overwrite") {
    using namespace forma::text;
    Scratch scratch;
    auto path = scratch.path();
    fixture(path, "first");
    auto initial = inspectFile(path);
    REQUIRE(initial);
    fixture(path, "other");  // same byte count
    // Restore mtime; ctime/version still detects the external write.
    const struct timespec times[] = {{0, UTIME_OMIT}, {initial.version.modifiedSeconds, initial.version.modifiedNanos}};
    REQUIRE(::utimensat(AT_FDCWD, path.c_str(), times, 0) == 0);
    CHECK(saveFile(path, "editor", initial.version).status == FileStatus::Conflict);
    CHECK(contents(path) == "other");
    auto external = inspectFile(path);
    REQUIRE(external);
    CHECK(saveFile(path, "editor").status == FileStatus::Conflict);
    CHECK(saveFile(path, "editor", external.version));  // explicit fresh overwrite consent
    auto replacementBaseline = inspectFile(path);
    fs::rename(path, scratch.path("old.txt"));
    fixture(path, "replacement");
    CHECK(saveFile(path, "editor", replacementBaseline.version).status == FileStatus::Conflict);
    CHECK(contents(path) == "replacement");
    fs::remove(path);
    CHECK(saveFile(path, "editor", replacementBaseline.version).status == FileStatus::Conflict);
    CHECK_FALSE(fs::exists(path));
    CHECK(saveFile(path, "new document"));
    CHECK((metadata(path).st_mode & 0777) == 0600);
    noTemporaries(scratch);
}

TEST_CASE("a mid-save external writer or new destination is never silently overwritten") {
    using namespace forma::text;
    Scratch scratch;
    const auto path = scratch.path();
    fixture(path, "first");
    const auto initial = inspectFile(path);
    auto result = saveFile(path, "editor", initial.version, [&](SaveStage stage) {
        if (stage == SaveStage::Write) fixture(path, "outside");
        return false;
    });
    CHECK(result.status == FileStatus::Conflict);
    CHECK(contents(path) == "outside");
    const auto newPath = scratch.path("new.txt");
    result = saveFile(newPath, "editor", {}, [&](SaveStage stage) {
        // Inject after the final inspection to exercise the no-replace commit.
        if (stage == SaveStage::Rename) fixture(newPath, "created outside");
        return false;
    });
    CHECK(result.status == FileStatus::Conflict);
    CHECK(contents(newPath) == "created outside");
    noTemporaries(scratch);
}

TEST_CASE("symlinks retain their link and target permissions; retargeting conflicts") {
    using namespace forma::text;
    Scratch scratch;
    auto target = scratch.path("target.txt"), link = scratch.path("alias.txt");
    fixture(target, "你好\r\n");
    REQUIRE(::chmod(target.c_str(), 0444) == 0);
    REQUIRE(::symlink("target.txt", link.c_str()) == 0);
    std::string value;
    auto opened = readFile(link, value);
    REQUIRE(opened);
    CHECK(value == "你好\r\n");
    CHECK(saveFile(link, "changed", opened.version));
    CHECK(fs::is_symlink(link));
    CHECK(fs::read_symlink(link) == "target.txt");
    CHECK(contents(target) == "changed");
    CHECK((metadata(target).st_mode & 0777) == 0444);
    auto baseline = inspectFile(link);
    fixture(scratch.path("other.txt"), "other");
    fs::remove(link);
    REQUIRE(::symlink("other.txt", link.c_str()) == 0);
    CHECK(saveFile(link, "bad", baseline.version).status == FileStatus::Conflict);
    CHECK(contents(target) == "changed");
    CHECK(contents(link) == "other");
    REQUIRE(::symlink("missing.txt", scratch.path("dangling.txt").c_str()) == 0);
    CHECK(saveFile(scratch.path("dangling.txt"), "bad").status == FileStatus::Error);
    CHECK(fs::is_symlink(scratch.path("dangling.txt")));
    noTemporaries(scratch);
}

TEST_CASE("hard links require a separate copy and never silently split") {
    using namespace forma::text;
    Scratch scratch;
    auto path = scratch.path(), alias = scratch.path("hard.txt");
    fixture(path, "original");
    REQUIRE(::link(path.c_str(), alias.c_str()) == 0);
    const auto initial = inspectFile(path);
    REQUIRE(initial);
    CHECK(saveFile(path, "bad", initial.version).status == FileStatus::HardLink);
    CHECK(contents(path) == "original");
    CHECK(contents(alias) == "original");
    CHECK(metadata(path).st_ino == metadata(alias).st_ino);
    CHECK(metadata(path).st_nlink == 2);
    CHECK(saveFile(scratch.path("copy.txt"), "editor copy"));
    noTemporaries(scratch);
}

TEST_CASE("read errors leave the supplied document unchanged") {
    using namespace forma::text;
    Scratch scratch;
    std::string value = "keep editor buffer";
    CHECK_FALSE(readFile(scratch.path("missing"), value));
    CHECK(value == "keep editor buffer");
    CHECK_FALSE(readFile(scratch.root, value));
    CHECK(value == "keep editor buffer");
    CHECK_FALSE(saveFile(scratch.root, "bad"));
    CHECK_FALSE(saveFile(scratch.path("missing-parent") / "file.txt", "bad"));
    noTemporaries(scratch);
}

TEST_CASE("range undo is proportional to edits and restores both selection ends") {
    using forma::text::EditorTestAccess;
    TextEditor editor(std::string(2 * 1024 * 1024, 'a'));
    editor.setSelection(100, 100);
    for (size_t i = 0; i < 150; ++i) editor.insert("文");
    CHECK(EditorTestAccess::historyBytes(editor) < 100000);
    for (size_t i = 0; i < 150; ++i) editor.onKeyDown({Key::Z, Modifiers::Ctrl});
    CHECK(editor.value() == std::string(2 * 1024 * 1024, 'a'));
    CHECK(editor.caret() == 100);
    CHECK(editor.anchor() == 100);
    editor.onKeyDown({Key::Y, Modifiers::Ctrl});
    CHECK(editor.value().substr(100, 3) == "文");
    editor.insert("x");
    auto edited = editor.value();
    editor.onKeyDown({Key::Y, Modifiers::Ctrl});
    CHECK(editor.value() == edited);  // new edit discards redo
    editor.setValue("A你好🙂Z");
    editor.setSelection(7, 1);  // backwards selection
    editor.insert("文");
    CHECK(editor.value() == "A文🙂Z");
    editor.onKeyDown({Key::Z, Modifiers::Ctrl});
    CHECK(editor.value() == "A你好🙂Z");
    CHECK(editor.anchor() == 7);
    CHECK(editor.caret() == 1);
    editor.onKeyDown({Key::Y, Modifiers::Ctrl});
    CHECK(editor.caret() == 4);
    CHECK(editor.anchor() == 4);
}

TEST_CASE("replace all grows/shrinks/deletes in one transaction with exact undo selection") {
    TextEditor editor("文 x 文 x 文\r\n");
    editor.setSelection(7, 2);  // UTF-8 snapping is part of the before state
    const auto anchor = editor.anchor(), caret = editor.caret();
    const auto original = editor.value();
    CHECK(editor.replaceAll("文", "你好") == 3);
    CHECK(editor.value() == "你好 x 你好 x 你好\r\n");
    editor.onKeyDown({Key::Z, Modifiers::Ctrl});
    CHECK(editor.value() == original);
    CHECK(editor.anchor() == anchor);
    CHECK(editor.caret() == caret);
    editor.onKeyDown({Key::Y, Modifiers::Ctrl});
    CHECK(editor.caret() == 0);
    CHECK(editor.anchor() == 0);
    CHECK(editor.replaceAll("你好", "") == 3);
    CHECK(editor.value() == " x  x \r\n");
    editor.onKeyDown({Key::Z, Modifiers::Ctrl});
    CHECK(editor.value() == "你好 x 你好 x 你好\r\n");
    editor.onKeyDown({Key::Z, Modifiers::Ctrl});
    CHECK(editor.value() == original);
    editor.setValue("reset");
    editor.onKeyDown({Key::Z, Modifiers::Ctrl});
    CHECK(editor.value() == "reset");
}

TEST_CASE("readonly documents reject every editing entry point") {
    TextEditor editor("你好", false);
    editor.selectAll();
    editor.insert("bad");
    CHECK_FALSE(editor.onTextInput({"bad"}));
    CHECK(editor.replaceAll("你好", "bad") == 0);
    CHECK_FALSE(editor.onKeyDown({Key::Backspace}));
    CHECK_FALSE(editor.onKeyDown({Key::Enter}));
    CHECK(editor.value() == "你好");
}

TEST_CASE("wrap and lazy caret layouts cache by version width font, not scroll coordinates") {
    using forma::text::EditorTestAccess;
    TextEditor editor;
    std::string large;
    for (size_t i = 0; i < 10000; ++i) large += "你好 some words\n";
    editor.setValue(large);
    editor.measure({300, 500});
    editor.arrange({0, 0, 300, 300000});
    const auto wrappedBuilds = EditorTestAccess::layoutBuilds(editor);
    const auto firstHit = EditorTestAccess::hit(editor, {50, 12});
    const auto caretBuilds = EditorTestAccess::caretBuilds(editor);
    CHECK(caretBuilds == 1);  // no caret array for the other 10000 rows
    CHECK(EditorTestAccess::layoutBuilds(editor) == wrappedBuilds);
    CHECK(EditorTestAccess::hit(editor, {50, 12}) == firstHit);
    CHECK(EditorTestAccess::caretBuilds(editor) == caretBuilds);
    editor.arrange({-20, -500, 300, 300000});
    CHECK(EditorTestAccess::hit(editor, {30, -488}) == firstHit);
    CHECK(EditorTestAccess::layoutBuilds(editor) == wrappedBuilds);
    CHECK(EditorTestAccess::caretBuilds(editor) == caretBuilds);
    editor.arrange({0, 0, 200, 300000});
    EditorTestAccess::hit(editor, {50, 12});
    CHECK(EditorTestAccess::layoutBuilds(editor) == wrappedBuilds + 1);
    editor.setEditorFont(18);
    EditorTestAccess::hit(editor, {50, 12});
    CHECK(EditorTestAccess::layoutBuilds(editor) == wrappedBuilds + 2);
    editor.setSelection(0, 0);
    editor.insert("文");
    EditorTestAccess::hit(editor, {50, 12});
    CHECK(EditorTestAccess::layoutBuilds(editor) == wrappedBuilds + 3);
    editor.onKeyDown({Key::Z, Modifiers::Ctrl});
    EditorTestAccess::hit(editor, {50, 12});
    CHECK(EditorTestAccess::layoutBuilds(editor) == wrappedBuilds + 4);
    editor.setWrap(false);
    EditorTestAccess::hit(editor, {50, 12});
    const auto unwrappedBuilds = EditorTestAccess::layoutBuilds(editor);
    editor.arrange({0, 0, 600, 300000});
    EditorTestAccess::hit(editor, {50, 12});
    CHECK(EditorTestAccess::layoutBuilds(editor) == unwrappedBuilds);
    editor.setSelection(0, 3);
    auto position = EditorTestAccess::caretPosition(editor);
    CHECK(position.x > editor.contentRect().x);
    CHECK(editor.lineCount() == 10001);
    CHECK(editor.characterCount() == 140000);
}

TEST_CASE("mixed line endings remain verbatim through navigation deletion and undo") {
    using forma::text::EditorTestAccess;
    TextEditor editor("A\r\n你好\rZ\n");
    const auto original = editor.value();
    CHECK(editor.lineCount() == 4);
    editor.measure({300, 500});
    editor.arrange({0, 0, 300, 500});
    editor.setSelection(3, 3);
    CHECK(editor.location() == std::pair<size_t, size_t>{2, 1});
    const auto position = EditorTestAccess::caretPosition(editor);
    CHECK(position.y > editor.contentRect().y);
    editor.onKeyDown({Key::Left});
    CHECK(editor.caret() == 1);
    editor.onKeyDown({Key::Right});
    CHECK(editor.caret() == 3);
    editor.onKeyDown({Key::Backspace});
    CHECK(editor.value() == "A你好\rZ\n");
    CHECK(editor.lineCount() == 3);
    editor.onKeyDown({Key::Z, Modifiers::Ctrl});
    CHECK(editor.value() == original);
    CHECK(editor.caret() == 3);
    CHECK(editor.anchor() == 3);
}

TEST_CASE("IME preedit never enters value history or document layout; commit is one undo") {
    using forma::text::EditorTestAccess;
    TextEditor editor("A你好Z");
    editor.measure({300, 500});
    editor.arrange({0, 0, 300, 500});
    editor.setSelection(7, 1);
    const auto original = editor.value();
    const auto revision = editor.revision();
    const auto builds = EditorTestAccess::layoutBuilds(editor);
    const auto history = EditorTestAccess::historyBytes(editor);
    REQUIRE(editor.onTextEditing({"ni", 2, 0}));
    CHECK(editor.isComposing());
    CHECK(editor.value() == original);
    CHECK(editor.revision() == revision);
    CHECK(editor.caret() == 1);
    CHECK(editor.anchor() == 7);
    CHECK(EditorTestAccess::historyBytes(editor) == history);
    EditorTestAccess::inputPosition(editor);
    CHECK(EditorTestAccess::layoutBuilds(editor) == builds);
    REQUIRE(editor.onTextEditing({"你好", 0, 2}));
    CHECK(editor.onKeyDown({Key::Backspace}));
    CHECK(editor.onKeyDown({Key::Enter}));
    CHECK(editor.onKeyDown({Key::Z, Modifiers::Ctrl}));
    CHECK(editor.onKeyDown({Key::A, Modifiers::Ctrl}));
    CHECK(editor.value() == original);
    CHECK(editor.caret() == 1);
    CHECK(editor.anchor() == 7);
    CHECK(editor.onTextInput({"新"}));
    CHECK_FALSE(editor.isComposing());
    CHECK(editor.value() == "A新Z");
    CHECK(editor.revision() == revision + 1);
    editor.onKeyDown({Key::Z, Modifiers::Ctrl});
    CHECK(editor.value() == original);
    CHECK(editor.caret() == 1);
    CHECK(editor.anchor() == 7);
    editor.onKeyDown({Key::Y, Modifiers::Ctrl});
    CHECK(editor.value() == "A新Z");
}

TEST_CASE("IME cancellation focus loss and programmatic movement keep document and undo intact") {
    using forma::text::EditorTestAccess;
    TextEditor editor("文");
    editor.setSelection(3, 3);
    editor.insert("x");
    const auto original = editor.value();
    const auto history = EditorTestAccess::historyBytes(editor);
    REQUIRE(editor.onTextEditing({"候选", 1, 1}));
    editor.cancelComposition();
    CHECK_FALSE(editor.isComposing());
    CHECK(EditorTestAccess::preedit(editor).empty());
    CHECK(editor.value() == original);
    CHECK(EditorTestAccess::historyBytes(editor) == history);
    REQUIRE(editor.onTextEditing({"候选", 1, 1}));
    CHECK(editor.onTextEditing({"", 0, 0}));
    CHECK_FALSE(editor.isComposing());
    REQUIRE(editor.onTextEditing({"候选", 1, 1}));
    editor.onFocusChanged(false);
    CHECK_FALSE(editor.isComposing());
    REQUIRE(editor.onTextEditing({"候选", 1, 1}));
    editor.setSelection(0, 0);
    CHECK_FALSE(editor.isComposing());
    REQUIRE(editor.onTextEditing({"候选", 1, 1}));
    CHECK(editor.onTextInput({""}));
    CHECK_FALSE(editor.isComposing());
    CHECK(editor.value() == original);
    CHECK(EditorTestAccess::historyBytes(editor) == history);
    editor.onKeyDown({Key::Z, Modifiers::Ctrl});
    CHECK(editor.value() == "文");
    editor.setValue("new");
    REQUIRE(editor.onTextEditing({"候选", 1, 1}));
    editor.setValue("reset");
    CHECK_FALSE(editor.isComposing());
    CHECK(editor.value() == "reset");
    TextEditor readonly("消息", false);
    CHECK_FALSE(readonly.onTextEditing({"候选", 1, 1}));
    CHECK_FALSE(readonly.isComposing());
}

TEST_CASE("IME Unicode selection offsets and candidate position are codepoint based and bounded") {
    using forma::text::EditorTestAccess;
    TextEditor editor("xyz");
    editor.measure({400, 500});
    editor.arrange({0, 0, 400, 500});
    editor.setSelection(1, 1);
    const auto origin = EditorTestAccess::caretPosition(editor);
    REQUIRE(editor.onTextEditing({"A你🙂Z", 1, 2}));
    CHECK(EditorTestAccess::preeditRange(editor) == std::pair<size_t, size_t>{1, 8});
    const auto candidate = EditorTestAccess::inputPosition(editor);
    const auto& font = EditorTestAccess::font(editor);
    const float expected = glyphAdvance(U'A', font) + glyphAdvance(U'你', font) + glyphAdvance(U'🙂', font);
    CHECK(candidate.x - origin.x == doctest::Approx(expected));
    CHECK(candidate.y == origin.y);
    REQUIRE(editor.onTextEditing({"A你🙂Z", -4, -2}));
    CHECK(EditorTestAccess::preeditRange(editor) == std::pair<size_t, size_t>{0, 0});
    CHECK(EditorTestAccess::inputPosition(editor).x == origin.x);
    REQUIRE(editor.onTextEditing({"A你🙂Z", 1000000, 1000000}));
    CHECK(EditorTestAccess::preeditRange(editor) == std::pair<size_t, size_t>{9, 9});
    CHECK(editor.value() == "xyz");
}
