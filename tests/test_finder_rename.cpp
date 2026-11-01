#include "core/app_impl.hpp"
#include "file_ops.hpp"
#include "file_panels.hpp"
#include "panels.hpp"
#include "test_support.hpp"
#include "views.hpp"

#include <cstdlib>
#include <filesystem>
#include <fstream>
#include <iostream>

namespace fs = std::filesystem;
using namespace mayo;
using namespace finder;

namespace {
void require(bool ok, const char* message) {
    if (!ok) {
        std::cerr << "rename: " << message << '\n';
        std::exit(1);
    }
}

struct Host : test::TestHost {
    std::shared_ptr<Widget> top;
    std::vector<std::string> dragged;
    bool startFileDrag(const std::vector<std::string>& paths) override { dragged = paths; return true; }
    Host() {
        dispatcher.onOverlayDismissed = [](Widget* widget) {
            if (auto* popup = dynamic_cast<PopupView*>(widget)) popup->popupDismissed();
        };
    }
    void showOverlay(std::shared_ptr<Widget> overlay) override {
        top = overlay;
        TestHost::showOverlay(overlay);
        const auto measured = overlay->measure(windowSize());
        auto* popup = dynamic_cast<PopupHost*>(overlay.get());
        overlay->arrange(popup->placePopup(windowSize(), measured));
    }
    void dismissOverlay(Widget* overlay) override {
        TestHost::dismissOverlay(overlay);
        if (top.get() == overlay) top.reset();
    }
    void click(Point point) {
        dispatcher.pointerDown({point, MouseButton::Left, 1});
        dispatcher.pointerUp({point, MouseButton::Left, 1});
    }
};

ButtonView* pencilIn(Widget& widget) {
    if (auto* button = dynamic_cast<ButtonView*>(&widget); button && button->icon() == Icon::Pencil) return button;
    for (const auto& child : widget.children()) {
        if (auto* button = pencilIn(*child)) return button;
    }
    return nullptr;
}
}  // namespace

void testFinderRename(const fs::path& root) {
    App app("finder-rename-tests", {}, {.background = true});
    app.settings().setReadOnly(true);
    std::string error;
    FileOps ops(app, [&](const std::string& message) { error = message; });
    const auto folder = root / "rename";
    fs::create_directory(folder);

    // All three conflict actions remain inside the popup, including long Chinese labels.
    {
        Host host;
        auto alert = std::make_shared<AlertView>("目标位置已有同名项目",
            "替换会将原项目移到回收站；保留两者会为新项目自动重命名。",
            std::vector<AlertButton>{{"取消", {}}, {"保留两者（重命名）", {}}, {"替换", {}}}, true);
        alert->show(host);
        int buttons = 0;
        std::function<void(Widget&)> check = [&](Widget& widget) {
            if (dynamic_cast<ButtonView*>(&widget)) {
                ++buttons;
                const auto r = widget.frame(), panel = alert->frame();
                require(r.x >= panel.x && r.right() <= panel.right() && r.y >= panel.y && r.bottom() <= panel.bottom(),
                        "conflict button fits within popup");
            }
            for (const auto& child : widget.children()) check(*child);
        };
        check(*alert);
        require(buttons == 3, "all conflict actions are laid out");
        alert->dismiss();
    }
    // A drag continues outside a short file view and hands off when it leaves the window.
    {
        Host host;
        const auto source = folder / "drag-source.txt";
        std::ofstream(source) << "drag contents";
        auto browser = std::make_shared<Browser>();
        browser->path = folder.string();
        browser->entries = {*statEntry(source.string())};
        auto view = std::make_shared<ListRowsView>(browser, std::make_shared<ListColumns>());
        view->reload();
        host.attach(view);
        const Point down{100, 14};
        host.dispatcher.pointerDown({down, MouseButton::Left, 1});
        // The first movement jumps below the single row, before a native drag has started.
        host.dispatcher.pointerMove({{110, 100}, MouseButton::None, 0});
        host.dispatcher.pointerMove({{810, 100}, MouseButton::None, 0});
        require(host.dragged == std::vector<std::string>{source.string()}, "drag outside a short view reaches native host");
        host.dispatcher.pointerUp({{110, 14}, MouseButton::Left, 1});
    }

    // Real file views: menu activation, UTF-8 input, extension selection, submit, cancellation and blur.
    for (int mode = 0; mode < 3; ++mode) {
        Host host;
        const auto original = folder / ("原文件" + std::to_string(mode) + ".txt");
        std::ofstream(original) << "keep contents";
        auto browser = std::make_shared<Browser>();
        browser->path = folder.string();
        browser->entries = {*statEntry(original.string())};
        browser->select(original.string());
        int commits = 0;
        browser->rename = [&](const std::string& path, const std::string& name) {
            ++commits;
            require(!ops.rename(path, name).empty(), "inline rename reaches filesystem");
        };
        std::shared_ptr<FileView> view;
        if (mode == 0)
            view = std::make_shared<IconGridView>(browser);
        else if (mode == 1)
            view = std::make_shared<ListRowsView>(browser, std::make_shared<ListColumns>());
        else {
            auto column = std::make_shared<ColumnListView>(browser, folder.string());
            column->setEntries(browser->entries, {});
            view = column;
        }
        view->reload();
        host.attach(view);
        auto menu = std::make_shared<MenuView>(
            std::vector<MenuItem>{MenuItem("重命名", [&] { view->beginRename(original.string()); })});
        menu->showFrom(*view);
        host.setFocus(menu.get(), false);
        host.click(menu->frame().center());
        app.impl().runDueTimers();
        view->arrange(view->frame());
        auto* editor = dynamic_cast<RenameField*>(host.focusedWidget());
        require(editor && host.textInputActive, "menu opens and focuses editable filename");
        require(editor->selectionAnchor() == 0 && editor->caret() == original.stem().string().size(),
                "only UTF-8 stem is selected");
        host.dispatcher.textInput({"新名称" + std::to_string(mode)});
        host.dispatcher.keyDown({Key::Enter});
        app.impl().runDueTimers();
        require(commits == 1 && !view->isRenaming(), "Enter commits exactly once and removes editor");
        const auto renamed = folder / ("新名称" + std::to_string(mode) + ".txt");
        require(fs::exists(renamed) && !fs::exists(original), "new name preserves extension");
        fs::rename(renamed, original);

        view->beginRename(original.string());
        view->arrange(view->frame());
        host.dispatcher.textInput({"取消"});
        host.dispatcher.keyDown({Key::Escape});
        app.impl().runDueTimers();
        require(commits == 1 && fs::exists(original) && !view->isRenaming(), "Escape leaves original intact");

        view->beginRename(original.string());
        view->arrange(view->frame());
        host.dispatcher.textInput({"点击别处" + std::to_string(mode)});
        host.setFocus(view.get(), false);
        app.impl().runDueTimers();
        require(commits == 2 && !view->isRenaming(), "losing focus commits exactly once");
    }

    auto openPropertyEditor = [&](Host& host, const fs::path& path) {
        presentProperties(host, *statEntry(path.string()), Button("更改…"), {},
                          [&, path](const std::string& name) { return !ops.rename(path.string(), name).empty(); });
        auto properties = host.top;
        auto* pencil = pencilIn(*properties);
        require(pencil && pencil->isEnabled(), "properties pencil is enabled");
        host.click(pencil->frame().center());
        auto* field = dynamic_cast<TextFieldView*>(host.focusedWidget());
        require(field && host.textInputActive && host.top != properties, "properties pencil opens name editor");
        return field;
    };

    const auto original = folder / "属性.txt";
    std::ofstream(original) << "properties";
    {
        Host host;
        auto* editor = openPropertyEditor(host, original);
        require(editor->caret() == std::string("属性").size() && editor->selectionAnchor() == 0,
                "properties selects name without extension");
        host.dispatcher.textInput({"已改名"});
        host.dispatcher.keyDown({Key::Enter});
        require(fs::exists(folder / "已改名.txt") && !fs::exists(original), "properties saves filename");
        require(!host.top, "successful rename closes dialog");
        fs::rename(folder / "已改名.txt", original);
    }
    {
        Host host;
        auto* editor = openPropertyEditor(host, original);
        const auto occupied = folder / "已存在.txt";
        std::ofstream(occupied) << "do not overwrite";
        for (const auto& name : {std::string("已存在.txt"), std::string(), std::string("../其他.txt")}) {
            error.clear();
            editor->setText(name);
            host.dispatcher.keyDown({Key::Enter});
            require(!error.empty() && fs::exists(original) && host.top,
                    "failed rename reports error and retains editor");
        }
        host.dispatcher.keyDown({Key::Escape});
        require(fs::exists(original), "cancelling properties rename preserves file");
    }
    {
        Host host;
        const auto directory = folder / "目录.v1";
        fs::create_directory(directory);
        auto* editor = openPropertyEditor(host, directory);
        require(editor->caret() == directory.filename().string().size(), "directory name including dots is selected");
        host.dispatcher.textInput({"新目录"});
        host.dispatcher.keyDown({Key::Enter});
        require(fs::is_directory(folder / "新目录") && !fs::exists(directory), "properties renames directories");
    }

    fs::create_symlink(folder / "missing", folder / "broken-link");
    require(ops.rename(original.string(), "broken-link").empty(), "dangling symlink name is occupied");
    require(fs::is_symlink(folder / "broken-link"), "rename preserves existing dangling symlink");
    require(ops.rename(original.string(), std::string("bad\0name", 8)).empty(), "embedded null is rejected");
    require(fs::exists(original), "invalid rename preserves original file");
    app.impl().runDueTimers();
}
