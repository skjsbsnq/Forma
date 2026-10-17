#include <algorithm>
#include <cstdlib>
#include <filesystem>
#include <fstream>
#include <mayonnaise/mayonnaise.hpp>
#include <sstream>
#include "editor.hpp"
#include "i18n.hpp"

using namespace mayo;
using forma::text::message;
using forma::text::TextEditor;
using forma::text::translate;
namespace fs = std::filesystem;

namespace {
const char* sample = R"SVG(<?xml version="1.0" encoding="utf-8"?>
<!DOCTYPE svg PUBLIC "-//W3C//DTD SVG 1.1//EN"
    "http://www.w3.org/Graphics/SVG/1.1/DTD/svg11.dtd">

<svg version="1.1" xmlns="http://www.w3.org/2000/svg" viewBox="0 0 64 64">

<title>besides editor</title>

<style><![CDATA[
    #editor,
    #viewer { stroke-width: 2px; }
    .fill   { fill: white; }
    .shape  { stroke: black; }
    #text   { stroke-width: 1px; }
]]></style>

<defs>
    <clipPath id="clipPath">
        <rect width="64" height="64"/>
        <rect width="56" height="64"/>
    </clipPath>
</defs>

<g id="editor" class="shape" clip-path="url(#clipPath)">
    <rect x="8" y="8" width="48" height="48" rx="6"/>
    <path d="M18 22h28 M18 32h20 M18 42h24"/>
</g>

</svg>
)SVG";

// The two narrow strips use the same metrics as the official CotEditor screenshot.
class Strip final : public Widget {
public:
    bool tabs = false;
    std::string title, status, compactStatus;
    std::function<void()> onNew;
    Size measureContent(Size available) override { return {available.width, tabs ? 36.0f : 32.0f}; }
    bool expandsContent(Axis axis) const override { return axis == Axis::Horizontal; }
    bool isInteractive() const override { return tabs; }
    void draw(Painter& p) override {
        const auto r = frame();
        const auto& c = theme().colors;
        p.fillRect(r, c.windowBackground);
        const Font font{12, FontWeight::Regular, 16};
        if (tabs) {
            p.fillRoundedRect({r.x + 8, r.y + 4, r.width - 48, 27}, CornerRadii(14), c.fill);
            p.fillRoundedRect({r.x + 10, r.y + 6, r.width - 52, 23}, CornerRadii(12), c.controlBackground,
                              c.windowHighlight, 1);
            auto displayTitle = title;
            while (displayTitle.size() > 3 && p.measureText(displayTitle, font) > r.width - 90) {
                size_t end = displayTitle.size() - 1;
                while (end && (static_cast<unsigned char>(displayTitle[end]) & 192) == 128) --end;
                displayTitle.resize(end);
            }
            p.pushClip({r.x + 20, r.y + 4, r.width - 72, 27});
            p.drawText(displayTitle, {r.x + 20, r.y + 4, r.width - 72, 27}, font, c.label, TextAlign::Center);
            p.popClip();
            p.fillCircle({r.right() - 21, r.y + 17}, 13, c.controlBackground);
            p.drawIcon(Icon::Plus, {r.right() - 29, r.y + 9, 16, 16}, c.label);
            p.drawLine({r.x, r.bottom()}, {r.right(), r.bottom()}, c.separator, .5f);
        } else {
            p.drawLine({r.x, r.y}, {r.right(), r.y}, c.separator, .5f);
            const std::string encoding = "UTF-8 · LF";
            const float encodingWidth = p.measureText(encoding, font);
            const float available = r.width - encodingWidth - 56;
            const auto& value = p.measureText(status, font) <= available ? status : compactStatus;
            p.pushClip({r.x + 18, r.y, available, r.height});
            p.drawText(value, {r.x + 18, r.y, available, r.height}, font, c.secondaryLabel);
            p.popClip();
            p.drawLine({r.right() - encodingWidth - 28, r.y + 9}, {r.right() - encodingWidth - 28, r.bottom() - 9},
                       c.separator, .5f);
            p.drawText(encoding, {r.right() - encodingWidth - 18, r.y, encodingWidth, r.height}, font, c.label);
        }
    }
    void onPointerMove(const PointerEvent& e) override {
        const bool overNew = tabs && e.position.x > frame().right() - 40;
        setTooltip(overNew ? translate("New document (Ctrl+N)") : "");
        if (host()) host()->setCursor(overNew ? CursorShape::PointingHand : CursorShape::Arrow);
    }
    bool onPointerDown(const PointerEvent& e) override {
        if (tabs && e.button == MouseButton::Left && e.position.x > frame().right() - 40 && onNew) {
            onNew();
            return true;
        }
        return false;
    }
};
}  // namespace

int main(int argc, char** argv) {
    setUiTranslator(translate);
    App app("text_editor", MAYO_DESKTOP_ID);
    auto& prefs = app.settings();
    auto& window = app.createWindow(
        {.title = translate("Untitled"), .width = 860, .height = 700, .minWidth = 620, .minHeight = 420});
    window.setTitleVisible(false);
    auto editor = std::make_shared<TextEditor>();
    bool highlight = prefs.getBool("highlight", true);
    std::string syntaxMode = prefs.getString("syntaxMode", "auto");
    bool numbers = prefs.getBool("lineNumbers", true), wrap = prefs.getBool("wrap", true);
    float fontSize = static_cast<float>(std::clamp(prefs.getInt("fontSize", 13), 10, 24));
    editor->setEditorFont(fontSize);
    editor->setLineNumbers(numbers);
    editor->setWrap(wrap);
    auto tabs = std::make_shared<Strip>();
    tabs->tabs = true;
    auto status = std::make_shared<Strip>();
    auto name = Label("Untitled").verbatim().weight(FontWeight::Semibold).lineLimit(1).maxWidth(260);
    auto outline = Label("Plain Text")
                       .verbatim()
                       .weight(FontWeight::Semibold)
                       .font(Font{12, FontWeight::Semibold, 16})
                       .lineLimit(1)
                       .maxWidth(240);
    auto format = Button("Plain Text").style(ButtonStyle::Glass);
    auto notice = Label("").font(Font{12, FontWeight::Regular, 16}).lineLimit(1).maxWidth(250);
    auto query = SearchField("Find");
    auto replacement = TextField("Replace with");
    fs::path path;
    std::string saved;
    Window* dialog = nullptr;
    Window* findWindow = nullptr;
    Window* settingsWindow = nullptr;
    std::function<void()> refresh;
    auto focusEditor = [&] {
        if (editor->host()) editor->host()->setFocus(editor.get(), false);
    };
    auto error = [&](std::string message) { notice.setText(uiText(message)); };
    refresh = [&] {
        bool dirty = editor->value() != saved;
        auto title = path.empty() ? translate("Untitled") : path.filename().string();
        name.setText(title + (dirty ? "  •" : ""));
        tabs->title = title + (dirty ? " •" : "");
        tabs->invalidate();
        window.setTitle(title + (dirty ? " — " + translate("Edited") : "") + " — " + translate("Forma Text"));
        auto [line, column] = editor->location();
        const auto counts = std::initializer_list<std::pair<std::string_view, std::string>>{
            {"lines", std::to_string(editor->lineCount())},
            {"chars", std::to_string(editor->characterCount())},
            {"line", std::to_string(line)},
            {"column", std::to_string(column)}};
        status->status = message("{lines} lines · {chars} characters · Ln {line}, Col {column}", counts);
        status->compactStatus = message("{lines} lines · {chars} chars · {line}:{column}", counts);
        status->invalidate();
        auto ext = path.extension().string();
        std::string type = ext == ".svg"                    ? "SVG"
                           : ext == ".md"                   ? "Markdown"
                           : ext == ".cpp" || ext == ".hpp" ? "C++"
                                                            : "Plain Text";
        if (syntaxMode != "auto") type = syntaxMode == "svg" ? "SVG" : "Plain Text";
        format.setTitle(translate(type) + "  ⌄");
        std::string heading = translate(type);
        if (type == "SVG") {
            const auto begin = editor->value().find("<title>");
            const auto end =
                begin == std::string::npos ? std::string::npos : editor->value().find("</title>", begin + 7);
            if (end != std::string::npos && end - begin < 100)
                heading = editor->value().substr(begin + 7, end - begin - 7);
        }
        outline.setText(heading);
        editor->setHighlight(highlight && type == "SVG");
    };
    editor->setOnChange([&](const std::string&) { refresh(); });
    auto write = [&](const fs::path& destination) {
        if (destination.empty()) {
            error(translate("Please enter a file path."));
            return false;
        }
        std::ofstream out(destination, std::ios::binary | std::ios::trunc);
        if (!out) {
            error(message("Could not save: {path}", {{"path", destination.string()}}));
            return false;
        }
        out << editor->value();
        out.close();
        if (!out) {
            error("Writing failed. Check available disk space.");
            return false;
        }
        path = destination;
        saved = editor->value();
        error(message("Saved {name}", {{"name", path.filename().string()}}));
        refresh();
        return true;
    };
    Button pendingSave, confirmSave;
    std::function<void(bool, std::function<void()>)> save;
    save = [&](bool saveAs, std::function<void()> then) {
        if (!saveAs && !path.empty()) {
            if (write(path) && then) then();
            return;
        }
        if (dialog) {
            dialog->raise();
            return;
        }
        auto& w = app.createWindow(
            {.title = translate("Save Document"), .width = 520, .height = 235, .minWidth = 440, .minHeight = 235});
        dialog = &w;
        auto destination = TextField("Absolute file path")
                               .text(path.empty() ? (fs::path(std::getenv("HOME") ? std::getenv("HOME") : "/tmp") /
                                                     (translate("Untitled") + ".txt"))
                                                        .string()
                                                  : fs::absolute(path).string());
        auto saveError = Label("").font(Font{12, FontWeight::Regular, 16}).color(LabelColor::Destructive).lineLimit(2);
        auto perform = [&, destination, then, saveError]() mutable {
            if (write(destination.value())) {
                if (dialog) dialog->close();
                if (then) then();
                focusEditor();
            } else {
                saveError.setText(notice.text());
            }
        };
        if (const char* smokePath = std::getenv("MAYO_TEST_SAVE_PATH");
            smokePath && std::getenv("MAYO_DEMO") && std::string(std::getenv("MAYO_DEMO")) == "save-flow")
            destination.setText(smokePath);
        confirmSave = Button("Save").style(ButtonStyle::Prominent).onClick(perform);
        destination.onSubmit([perform](const std::string&) mutable { perform(); });
        w.setContent(VStack({Label("Save location").weight(FontWeight::Semibold), destination.height(30),
                             Label("Enter the full path, including the file extension.")
                                 .font(Font{12, FontWeight::Regular, 16})
                                 .secondary(),
                             saveError,
                             HStack({Spacer(), Button("Cancel").onClick([&w] { w.close(); }), confirmSave}).spacing(8)})
                         .spacing(10)
                         .padding(20));
        w.onClosed([&, current = &w] {
            if (dialog == current) dialog = nullptr;
        });
        if (destination.node()->host()) destination.node()->host()->setFocus(destination.node().get(), true);
    };
    std::function<void(std::function<void()>)> guard;
    guard = [&](std::function<void()> action) {
        if (editor->value() == saved) {
            action();
            return;
        }
        if (dialog) {
            dialog->raise();
            return;
        }
        auto& w = app.createWindow(
            {.title = translate("Save Changes?"), .width = 490, .height = 240, .minWidth = 490, .minHeight = 240});
        dialog = &w;
        pendingSave = Button("Save…").style(ButtonStyle::Prominent).onClick([&, action] {
            dialog->close();
            dialog = nullptr;
            save(false, action);
        });
        w.setContent(VStack({Label("Do you want to save your changes?").weight(FontWeight::Semibold),
                             Label("Your changes will be lost if you don’t save them."),
                             HStack({Button("Don’t Save").onClick([&, action] {
                                         dialog->close();
                                         action();
                                     }),
                                     Spacer(), Button("Cancel").onClick([&w] { w.close(); }), pendingSave})
                                 .spacing(8)})
                         .spacing(20)
                         .padding(24));
        w.onClosed([&, current = &w] {
            if (dialog == current) dialog = nullptr;
        });
    };
    auto newDocument = [&] {
        guard([&] {
            path.clear();
            editor->setValue("");
            saved.clear();
            error("");
            refresh();
            focusEditor();
        });
    };
    auto load = [&](const std::string& filename) {
        std::ifstream in(filename, std::ios::binary);
        if (!in) {
            error(message("Could not open {path}", {{"path", filename}}));
            return;
        }
        std::ostringstream buffer;
        buffer << in.rdbuf();
        auto value = buffer.str();
        if (value.find('\0') != std::string::npos) {
            error("This is a binary file. Open a UTF-8 text document.");
            return;
        }
        // Normalize CRLF and old Mac CR so the status bar's LF promise is accurate.
        std::string normalized;
        for (size_t i = 0; i < value.size(); ++i) {
            if (value[i] == '\r') {
                normalized += '\n';
                if (i + 1 < value.size() && value[i + 1] == '\n') ++i;
            } else
                normalized += value[i];
        }
        path = filename;
        editor->setValue(normalized);
        saved = normalized;
        error("");
        refresh();
        focusEditor();
    };
    auto openDocument = [&] {
        guard([&] {
            app.openFiles(
                {}, false,
                [&](std::vector<std::string> files) {
                    if (!files.empty()) load(files.front());
                },
                &window);
        });
    };
    auto openFind = [&] {
        if (findWindow) {
            findWindow->raise();
            return;
        }
        auto& w = app.createWindow(
            {.title = translate("Find & Replace"), .width = 540, .height = 202, .minWidth = 480, .minHeight = 202});
        findWindow = &w;
        auto result = Label("Enter text to find").font(Font{12, FontWeight::Regular, 16}).secondary().lineLimit(1);
        query.height(32);
        replacement.height(32);
        auto next = [&, result]() mutable {
            bool found = editor->find(query.value());
            result.setText(translate(query.value().empty() ? "Enter text to find"
                                     : found               ? "Match selected in document"
                                                           : "No matches"));
        };
        query.onSubmit([next](const std::string&) mutable { next(); });
        query.tooltip("Literal search, case-sensitive. Matches wrap around the document.");
        auto updateMatches = [&, result](const std::string& value) mutable {
            size_t count = 0, at = 0;
            if (!value.empty())
                while ((at = editor->value().find(value, at)) != std::string::npos) {
                    ++count;
                    at += value.size();
                }
            result.setText(value.empty() ? translate("Enter text to find")
                           : count       ? message("Found {count} matches", {{"count", std::to_string(count)}})
                                         : translate("No matches"));
        };
        query.onChange(updateMatches);
        updateMatches(query.value());
        w.setContent(VStack({HStack({Label("Find").width(42), query}).spacing(10),
                             HStack({Label("Replace with").width(42), replacement}).spacing(10), result,
                             HStack({Button("Replace All").onClick([&, result]() mutable {
                                         auto n = editor->replaceAll(query.value(), replacement.value());
                                         result.setText(
                                             message("Replaced {count} matches", {{"count", std::to_string(n)}}));
                                     }),
                                     Spacer(), Button("Replace").onClick([&, next]() mutable {
                                         if (!query.value().empty() && editor->selection() == query.value())
                                             editor->insert(replacement.value());
                                         next();
                                     }),
                                     Button(Icon::ChevronLeft).tooltip("Previous match").onClick([&, result]() mutable {
                                         const bool found = editor->find(query.value(), true);
                                         result.setText(translate(query.value().empty() ? "Enter text to find"
                                                                  : found               ? "Match selected in document"
                                                                                        : "No matches"));
                                     }),
                                     Button(Icon::ChevronRight).tooltip("Next match").onClick(next)})
                                 .spacing(8)})
                         .spacing(10)
                         .padding(16));
        w.onClosed([&] { findWindow = nullptr; });
        w.onKey([&w](const KeyEvent& e) {
            if (e.key == Key::Escape) {
                w.close();
                return true;
            }
            return false;
        });
        if (query.node()->host()) query.node()->host()->setFocus(query.node().get(), true);
    };
    auto scroller = ScrollView(Element(editor)).axes(wrap ? ScrollAxes::Vertical : ScrollAxes::Both);
    auto openSettings = [&] {
        if (settingsWindow) {
            settingsWindow->raise();
            return;
        }
        auto& w = app.createWindow({.title = translate("Text Editor Settings"),
                                    .width = 540,
                                    .height = 480,
                                    .minWidth = 480,
                                    .minHeight = 400});
        settingsWindow = &w;
        w.setContent(ScrollView(
            VStack(
                {Form({AppearanceSettings(app),
                       FormSection("Editor", {FormRow("Line numbers", Toggle("", numbers).onChange([&](bool on) {
                                                  numbers = on;
                                                  prefs.set("lineNumbers", on);
                                                  editor->setLineNumbers(on);
                                              })),
                                              FormRow("Wrap lines", Toggle("", wrap).onChange([&](bool on) {
                                                  wrap = on;
                                                  prefs.set("wrap", on);
                                                  editor->setWrap(on);
                                                  scroller.axes(on ? ScrollAxes::Vertical : ScrollAxes::Both);
                                              })),
                                              FormRow("Font size", Picker({"12 pt", "13 pt", "14 pt", "16 pt", "18 pt"},
                                                                          fontSize == 12   ? 0
                                                                          : fontSize == 14 ? 2
                                                                          : fontSize == 16 ? 3
                                                                          : fontSize == 18 ? 4
                                                                                           : 1)
                                                                       .onChange([&](int i) {
                                                                           const int sizes[] = {12, 13, 14, 16, 18};
                                                                           fontSize = static_cast<float>(sizes[i]);
                                                                           prefs.set("fontSize", sizes[i]);
                                                                           editor->setEditorFont(fontSize);
                                                                       }))})})})
                .padding(20)));
        w.onClosed([&] { settingsWindow = nullptr; });
    };
    auto information = [&] { openSettings(); };
    window.setToolbar(
        Toolbar({Button(Icon::Folder).style(ButtonStyle::Borderless).tooltip("Open… (Ctrl+O)").onClick(openDocument),
                 name, Spacer(),
                 Button(Icon::Import).style(ButtonStyle::Borderless).tooltip("Save (Ctrl+S)").onClick([&] {
                     save(false, {});
                 }),
                 Button(Icon::Magnifier)
                     .style(ButtonStyle::Borderless)
                     .tooltip("Find & Replace (Ctrl+F)")
                     .onClick(openFind),
                 format.tooltip("Syntax and display").onClick([&] {
                     auto choose = [&](std::string mode) {
                         syntaxMode = std::move(mode);
                         prefs.set("syntaxMode", syntaxMode);
                         refresh();
                     };
                     auto menu = std::make_shared<MenuView>(std::vector<MenuItem>{
                         MenuItem("Automatic", [choose] { choose("auto"); }).setChecked(syntaxMode == "auto"),
                         MenuItem("Plain Text", [choose] { choose("plain"); }).setChecked(syntaxMode == "plain"),
                         MenuItem("SVG", [choose] { choose("svg"); }).setChecked(syntaxMode == "svg"),
                         MenuItem::separator(),
                         MenuItem("Syntax highlighting",
                                  [&] {
                                      highlight = !highlight;
                                      prefs.set("highlight", highlight);
                                      refresh();
                                  })
                             .setChecked(highlight),
                         MenuItem("Line numbers",
                                  [&] {
                                      numbers = !numbers;
                                      prefs.set("lineNumbers", numbers);
                                      editor->setLineNumbers(numbers);
                                  })
                             .setChecked(numbers),
                         MenuItem("Wrap lines", [&] {
                             wrap = !wrap;
                             prefs.set("wrap", wrap);
                             editor->setWrap(wrap);
                             scroller.axes(wrap ? ScrollAxes::Vertical : ScrollAxes::Both);
                         }).setChecked(wrap)});
                     menu->setOnDismiss([&] { app.post(focusEditor); });
                     menu->showFrom(*format.node());
                     if (editor->host()) editor->host()->setFocus(menu.get(), false);
                 }),
                 Button(Icon::Gear).style(ButtonStyle::Glass).tooltip("Editor settings (Ctrl+,)").onClick(information)})
            .spacing(8));
    auto navigation =
        HStack({Button(Icon::ChevronUp).style(ButtonStyle::Borderless).tooltip("Document start").onClick([&] {
                    editor->setSelection(0, 0);
                    focusEditor();
                }),
                Button(Icon::ChevronDown).style(ButtonStyle::Borderless).tooltip("Document end").onClick([&] {
                    editor->setSelection(editor->value().size(), editor->value().size());
                    focusEditor();
                }),
                outline, Spacer(), notice})
            .spacing(4)
            .padding(Insets(0, 10))
            .height(25);
    tabs->onNew = newDocument;
    window.setContent(VStack({Element(tabs), navigation, Divider(), scroller, Element(status)}).spacing(0));
    window.onKey([&](const KeyEvent& e) {
        if (!e.primary()) return false;
        switch (e.key) {
            case Key::N: newDocument(); return true;
            case Key::O: openDocument(); return true;
            case Key::S: save(e.shift(), {}); return true;
            case Key::F: openFind(); return true;
            case Key::G:
                editor->find(query.value(), e.shift());
                focusEditor();
                return true;
            case Key::Comma: openSettings(); return true;
            case Key::Q: guard([&] { app.quit(); }); return true;
            default: return false;
        }
    });
    window.onCloseRequested([&] {
        if (editor->value() == saved) return true;
        guard([&] { window.close(); });
        return false;
    });
    window.onFilesDropped([&](const std::vector<std::string>& files) {
        if (!files.empty()) guard([&, filename = files.front()] { load(filename); });
    });
    if (argc > 1)
        load(argv[1]);
    else if (std::getenv("MAYO_DEMO")) {
        path = "editor.svg";
        editor->setValue(sample);
        saved = sample;
    }
    refresh();
    focusEditor();
    if (const char* demo = std::getenv("MAYO_DEMO")) {
        std::string mode = demo;
        app.after(.2, [&, mode] {
            if (mode == "find" || mode == "find-narrow") {
                query.setText("stroke-width");
                replacement.setText("stroke-width");
                openFind();
                if (mode == "find-narrow" && findWindow) findWindow->setSize({480, 202});
            } else if (mode == "settings") {
                openSettings();
            } else if (mode == "display") {
                format.node()->click();
            } else if (mode == "chinese") {
                path = "写作笔记.txt";
                editor->setValue(
                    "写作，让想法慢慢成形。\n\n清晨的光落在书页上，窗外的树影轻轻摇晃。\n先记下此刻想到的句子，不必急着"
                    "修改。\n\n今天想完成的事\n\n    整理昨天的灵感与片段\n    写下一个清楚、完整的开头\n    "
                    "留一点空白，等待新的念头\n\n一段文字，应该像一扇窗。\n让读者看见风景，也看见文字背后的生活。\n\nEn"
                    "glish 与中文可以自然地写在一起。\n标点测试：你好，世界！“慢慢来”，也是一种节奏。\n");
                saved = editor->value();
                refresh();
            } else if (mode == "save") {
                save(true, {});
            } else if (mode == "narrow") {
                window.setSize({620, 460});
                path = "中文文稿：一段需要耐心书写的长标题.txt";
                editor->setValue(
                    "你好，世界！\n这是一份中文文稿，用来核对最窄窗口中的行号、正文与状态栏。\n中文与 English "
                    "的混排不应改变文稿内容。\n");
                saved = editor->value();
                refresh();
            } else if (mode == "unsaved") {
                editor->insert("<!-- 中文编辑 -->\n");
                newDocument();
            } else if (mode == "save-flow") {
                const char* target = std::getenv("MAYO_TEST_SAVE_PATH");
                if (!target || fs::exists(target)) {
                    app.quit(2);
                    return;
                }
                path.clear();
                saved.clear();
                editor->setValue("UTF-8 save regression: 你好🙂\n");
                guard([&] {
                    app.after(.05, [&] {
                        std::ifstream in(path, std::ios::binary);
                        std::ostringstream read;
                        read << in.rdbuf();
                        app.quit(in && read.str() == editor->value() && saved == editor->value() ? 0 : 3);
                    });
                });
                pendingSave.node()->click();
                // Reaping the old confirmation must not clear the newly opened Save dialog.
                app.after(.1, [&] {
                    if (!dialog)
                        app.quit(4);
                    else
                        confirmSave.node()->click();
                });
            }
        });
    }
    return app.run();
}
