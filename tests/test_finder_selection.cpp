#include "test_support.hpp"
#include "views.hpp"

#include <cstdlib>
#include <filesystem>
#include <fstream>
#include <functional>
#include <iostream>

namespace fs = std::filesystem;
using namespace mayo;
using namespace finder;

namespace {
void require(bool ok, const char* message) {
    if (!ok) {
        std::cerr << "selection: " << message << '\n';
        std::exit(1);
    }
}

template <class Base>
class SelectionView : public Base {
public:
    using Base::Base;
    Point point(const std::string& path) const { return this->itemRect(path).center(); }
};
}  // namespace

void testFinderSelection(const fs::path& root) {
    const auto folder = root / "selection";
    fs::create_directory(folder);
    std::vector<std::string> paths;
    for (const auto* name : {"文稿.txt", "照片.png", "第三项.txt", "文件夹"}) {
        const auto path = folder / name;
        if (path.extension().empty()) fs::create_directory(path);
        else std::ofstream(path) << "test contents";
        paths.push_back(path.string());
    }

    for (int mode = 0; mode < 3; ++mode) {
        test::TestHost host;
        auto browser = std::make_shared<Browser>();
        browser->path = folder.string();
        for (const auto& path : paths) browser->entries.push_back(*statEntry(path));
        std::vector<std::string> opened;
        browser->open = [&](const FileEntry& entry) { opened.push_back(entry.path); };
        int navigations = 0;
        browser->navigate = [&](const std::string& parent, const std::string& selected) {
            ++navigations;
            browser->path = parent;
            browser->select(selected);
        };
        std::shared_ptr<FileView> view;
        std::function<Point(size_t)> point;
        if (mode == 0) {
            auto grid = std::make_shared<SelectionView<IconGridView>>(browser);
            view = grid;
            point = [&, grid](size_t index) { return grid->point(paths[index]); };
        } else if (mode == 1) {
            auto rows = std::make_shared<SelectionView<ListRowsView>>(browser, std::make_shared<ListColumns>());
            view = rows;
            point = [&, rows](size_t index) { return rows->point(paths[index]); };
        } else {
            auto column = std::make_shared<SelectionView<ColumnListView>>(browser, folder.string());
            column->setEntries(browser->entries, {});
            // ColumnBrowser refreshes selection decorations after each click without changing the listing.
            browser->selectionChanged = [browser, column] { column->setEntries(browser->entries, {}); };
            view = column;
            point = [&, column](size_t index) { return column->point(paths[index]); };
        }
        view->reload();
        host.attach(view);
        auto reset = [&] {
            browser->path = folder.string();
            browser->select({});
            view->reload();
            host.setFocus(view.get(), false);
            opened.clear();
            navigations = 0;
        };
        auto click = [&](size_t index, int count, Modifiers mods = Modifiers::None) {
            host.clock += .05;
            const PointerEvent event{point(index), MouseButton::Left, count, mods};
            host.dispatcher.pointerDown(event);
            host.dispatcher.pointerUp(event);
        };

        reset();
        click(0, 1);
        click(2, 2, Modifiers::Shift);
        require(opened.empty() && browser->selection.size() == 3, "Shift double-click selects a range, not opens");
        require(browser->anchor == paths[0] && browser->lead == paths[2], "Shift preserves the range anchor");
        click(1, 3, Modifiers::Shift);
        require(opened.empty() && browser->selection == std::set<std::string>{paths[0], paths[1]},
                "Shift triple-click adjusts the range without opening");

        reset();
        click(0, 1);
        click(2, 2, Modifiers::Ctrl);
        require(opened.empty() && browser->selection == std::set<std::string>{paths[0], paths[2]},
                "Ctrl double-click adds to the selection");
        click(2, 3, Modifiers::Ctrl);
        require(opened.empty() && browser->selection == std::set<std::string>{paths[0]},
                "Ctrl triple-click toggles without opening");

        reset();
        click(0, 1);
        click(2, 2, Modifiers::Ctrl);
        click(3, 3, Modifiers::Ctrl | Modifiers::Shift);
        require(opened.empty() && browser->selection == std::set<std::string>{paths[0], paths[2], paths[3]},
                "Ctrl+Shift adds a range including a folder without opening it");

        reset();
        click(0, 1);
        host.dispatcher.pointerDown({point(0), MouseButton::Left, 2});
        require(opened.empty(), "double-click opens only after release");
        host.dispatcher.pointerUp({point(0), MouseButton::Left, 2});
        click(0, 3);
        require(opened == std::vector<std::string>{paths[0]}, "ordinary double-click opens once, triple-click does not repeat");

        reset();
        click(0, 1);
        click(1, 2);
        click(2, 3);
        require(opened.empty(), "nearby clicks on different files never form a double-click");
        click(2, 4);
        require(opened == std::vector<std::string>{paths[2]}, "a consecutive plain pair on the same file still opens");

        reset();
        click(0, 1);
        click(0, 2, Modifiers::Shift);
        click(0, 3);
        require(opened.empty(), "releasing Shift and clicking once does not open");
        click(0, 4);
        require(opened.size() == 1, "a fresh plain double-click after a modified click opens");

        reset();
        click(0, 1);
        host.dispatcher.pointerDown({point(0), MouseButton::Left, 2});
        host.dispatcher.pointerUp({point(0), MouseButton::Left, 2, Modifiers::Shift});
        require(opened.empty(), "a modifier pressed before release cancels opening");

        reset();
        click(0, 1);
        host.dispatcher.pointerDown({point(0), MouseButton::Left, 2});
        host.dispatcher.pointerMove({point(0) + Vec2{8, 0}});
        host.dispatcher.pointerUp({point(0), MouseButton::Left, 2});
        require(opened.empty(), "a double-click that becomes a drag never opens");

        reset();
        click(0, 1);
        host.dispatcher.pointerDown({point(0), MouseButton::Left, 2});
        host.dispatcher.pointerUp({{790, 580}, MouseButton::Left, 2});
        require(opened.empty(), "release outside the item cancels opening");

        reset();
        click(0, 1);
        view->reload();
        click(0, 2);
        require(opened.empty(), "reloading invalidates a previous click");

        reset();
        click(0, 1);
        host.dispatcher.keyDown({Key::Down, Modifiers::Shift});
        click(0, 2);
        require(opened.empty(), "keyboard selection interrupts a click sequence");

        if (mode == 2) {
            reset();
            browser->path = root.string();
            click(0, 2, Modifiers::Shift);
            require(navigations == 1 && opened.empty(), "an inactive column activates without opening a file");
        }
        browser->selectionChanged = {};
    }
}
