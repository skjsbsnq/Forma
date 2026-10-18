#include <fnmatch.h>
#include <unistd.h>
#include <QJsonArray>
#include <QJsonDocument>
#include <QJsonObject>
#include <QMimeDatabase>
#include <QUrl>
#include <filesystem>
#include <iostream>
#include <mayonnaise/mayonnaise.hpp>
#include "browser.hpp"
#include "preview_cache.hpp"
#include "views.hpp"

using namespace mayo;
namespace fs = std::filesystem;

// Finder uses Return for renaming; a chooser uses it to confirm selection.
template <class Base>
class PickerFileView : public Base {
public:
    using Base::Base;
    std::function<void()> confirm;
    bool onKeyDown(const KeyEvent& event) override {
        if (event.key == Key::Enter && confirm) {
            confirm();
            return true;
        }
        return Base::onKeyDown(event);
    }
};

int main(int argc, char** argv) {
    // One private pipe request per process. No shell commands or temporary files.
    const bool preview = argc > 1 && std::string(argv[1]) == "--preview";
    QJsonObject request;
    if (!preview) {
        std::string line;
        if (!std::getline(std::cin, line) || line.size() > 1024 * 1024) return 2;
        QJsonParseError error;
        const auto doc = QJsonDocument::fromJson(QByteArray::fromStdString(line), &error);
        if (error.error != QJsonParseError::NoError || !doc.isObject()) return 2;
        request = doc.object();
        if (request["version"].toInt() != 1) return 2;
    }
    const bool save = request["save"].toBool();
    const bool directory = request["directory"].toBool();
    const bool multiple = request["multiple"].toBool();
    const auto filters = request["filters"].toArray();
    int filterIndex = request["filterIndex"].toInt(0);
    if (filterIndex < 0 || filterIndex >= filters.size()) filterIndex = 0;
    App app("forma-file-picker", "io.github.Forma.FilePicker");
    auto& window = app.createWindow({.title = request["title"].toString(save ? "存储" : "打开").toStdString(),
                                     .width = 960,
                                     .height = 640,
                                     .minWidth = 720,
                                     .minHeight = 460});
    auto browser = std::make_shared<finder::Browser>();
    auto grid = std::make_shared<PickerFileView<finder::IconGridView>>(browser);
    auto scroll = ScrollView(Element(grid));
    grid->setScroller(scroll.node().get());
    auto listColumns = std::make_shared<finder::ListColumns>();
    auto rows = std::make_shared<PickerFileView<finder::ListRowsView>>(browser, listColumns);
    rows->setScroller(scroll.node().get());
    auto headerElement = Custom<finder::ListHeaderView>(browser, listColumns);
    headerElement.setHidden(true);
    finder::PreviewCache previews(app, [&] {
        grid->invalidate();
        rows->invalidate();
    });
    browser->previews = &previews;
    auto location = TextField("文件夹路径");
    auto name = TextField("文件名").text(request["name"].toString().toStdString());
    auto search = SearchField("搜索当前文件夹").width(180);
    auto status = Label("").secondary();
    auto accept =
        Button(request["acceptLabel"].toString(save ? "存储" : "打开").toStdString()).style(ButtonStyle::Prominent);
    auto filterLabel = Label("").secondary();
    std::vector<std::string> filterNames;
    for (const auto& filter : filters) filterNames.push_back(filter.toObject()["name"].toString().toStdString());
    if (filterNames.empty()) filterNames.push_back("所有文件");
    auto filterPicker = Picker(filterNames, filterIndex);
    filterPicker.setDisabled(filters.isEmpty());
    bool confirmedOverwrite = false;
    std::function<void()> reload;
    std::function<void(const std::string&)> navigate;
    QMimeDatabase mime;
    reload = [&] {
        std::string error;
        browser->entries = finder::listDirectory(browser->path, browser->showHidden, &error);
        const auto terms = filters.isEmpty() ? QJsonArray{} : filters[filterIndex].toObject()["terms"].toArray();
        std::erase_if(browser->entries, [&](const auto& e) {
            if (!finder::containsNoCase(e.name, search.value())) return true;
            if (e.isDir) return false;
            if (directory) return true;
            if (terms.isEmpty()) return false;
            for (const auto& term : terms) {
                const auto t = term.toObject();
                const auto value = t["value"].toString();
                if (t["type"].toInt() == 0 && fnmatch(value.toUtf8().constData(), e.name.c_str(), 0) == 0) return false;
                if (t["type"].toInt() == 1) {
                    const auto mt = mime.mimeTypeForFile(QString::fromStdString(e.path));
                    if (mt.inherits(value) || (value.endsWith("/*") && mt.name().startsWith(value.chopped(1))))
                        return false;
                }
            }
            return true;
        });
        finder::sortEntries(browser->entries, browser->sort);
        grid->reload();
        rows->reload();
        status.setText(error.empty() ? std::to_string(browser->entries.size()) + " 个项目" : error);
        filterLabel.setText(filters.isEmpty() ? "所有文件"
                                              : filters[filterIndex].toObject()["name"].toString().toStdString());
    };
    navigate = [&](const std::string& path) {
        std::error_code ec;
        if (!fs::is_directory(path, ec)) {
            status.setText("无法访问此文件夹");
            return;
        }
        browser->path = fs::absolute(path, ec).string();
        browser->selection.clear();
        browser->lead.clear();
        location.setText(browser->path);
        confirmedOverwrite = false;
        reload();
    };
    browser->navigate = [&](const std::string& path, const std::string&) { navigate(path); };
    auto finish = [&](const std::vector<std::string>& paths) {
        QJsonArray uris;
        for (const auto& path : paths)
            uris.append(QUrl::fromLocalFile(QString::fromStdString(path)).toString(QUrl::FullyEncoded));
        if (!preview)
            std::cout << QJsonDocument(
                             QJsonObject{{"version", 1}, {"response", 0}, {"uris", uris}, {"filterIndex", filterIndex}})
                             .toJson(QJsonDocument::Compact)
                             .constData()
                      << std::endl;
        app.quit();
    };
    std::function<void()> submit = [&] {
        auto paths = browser->selectedPaths();
        if (save) {
            const auto value = name.value();
            if (value.empty() || value == "." || value == ".." || value.find('/') != std::string::npos) {
                status.setText("请输入有效的文件名");
                return;
            }
            const auto target = fs::path(browser->path) / value;
            std::error_code ec;
            if (fs::is_directory(target, ec)) {
                navigate(target.string());
                return;
            }
            if (access(browser->path.c_str(), W_OK) != 0) {
                status.setText("此文件夹不可写入");
                return;
            }
            if (fs::exists(target, ec) && !confirmedOverwrite) {
                confirmedOverwrite = true;
                status.setText("文件已存在，再次点击“替换”以确认");
                accept.setTitle("替换");
                return;
            }
            finish({target.string()});
            return;
        }
        if (directory && paths.empty()) paths = {browser->path};
        if (paths.empty()) {
            status.setText("请先选择文件");
            return;
        }
        if (!multiple && paths.size() > 1) {
            status.setText("只能选择一个项目");
            return;
        }
        for (const auto& path : paths) {
            std::error_code ec;
            if (!fs::exists(path, ec)) {
                status.setText("所选项目已不存在");
                reload();
                return;
            }
            if (fs::is_directory(path, ec) != directory) {
                if (paths.size() == 1 && !directory)
                    navigate(path);
                else
                    status.setText(directory ? "请选择文件夹" : "请选择文件");
                return;
            }
        }
        finish(paths);
    };
    grid->confirm = submit;
    rows->confirm = submit;
    browser->open = [&](const finder::FileEntry& e) {
        if (e.isDir)
            navigate(e.path);
        else {
            browser->select(e.path);
            if (save)
                name.setText(e.name);
            else
                submit();
        }
    };
    browser->selectionChanged = [&] {
        confirmedOverwrite = false;
        accept.setTitle(request["acceptLabel"].toString(save ? "存储" : "打开").toStdString());
        const auto paths = browser->selectedPaths();
        if (save && paths.size() == 1) name.setText(fs::path(paths.front()).filename().string());
    };
    name.onChange([&](const auto&) {
        confirmedOverwrite = false;
        accept.setTitle("存储");
    });
    location.onSubmit([&](const auto& path) { navigate(path); });
    search.onChange([&](const auto&) { reload(); });
    filterPicker.onChange([&](int index) {
        filterIndex = index;
        browser->selection.clear();
        reload();
    });
    auto viewMode = SegmentedControl({Icon::Grid, Icon::List}, 0).onChange([&](int index) {
        headerElement.setHidden(index == 0);
        scroll->setContent(index == 0 ? std::static_pointer_cast<Widget>(grid)
                                      : std::static_pointer_cast<Widget>(rows));
    });
    headerElement->onColumnsResized = [&] { rows->invalidate(); };
    accept.onClick(submit);
    name.onSubmit([&](const auto&) { submit(); });
    auto cancel = [&] {
        if (!preview) std::cout << "{\"version\":1,\"response\":1}" << std::endl;
        app.quit();
    };
    window.onCloseRequested([&] {
        cancel();
        return true;
    });
    window.onKey([&](const KeyEvent& e) {
        if (e.key == Key::Enter) {
            submit();
            return true;
        }
        if (e.key == Key::Escape) {
            cancel();
            return true;
        }
        return false;
    });
    std::vector<SidebarItem> places{SidebarItem::header("常用位置"), {finder::homeDirectory(), Icon::Folder, "主目录"}};
    for (const auto& item : {std::pair{"DESKTOP", "Desktop"},
                             {"DOCUMENTS", "Documents"},
                             {"DOWNLOAD", "Downloads"},
                             {"PICTURES", "Pictures"}}) {
        const auto path = finder::userDirectory(item.first, item.second);
        const std::string title = std::string(item.first) == "DESKTOP"     ? "桌面"
                                  : std::string(item.first) == "DOCUMENTS" ? "文稿"
                                  : std::string(item.first) == "DOWNLOAD"  ? "下载"
                                                                           : "图片";
        places.emplace_back(path, Icon::Folder, title);
    }
    places.emplace_back("/", Icon::Folder, "此电脑");
    auto sidebar = Sidebar(places).onSelect(navigate);
    auto up = Button(Icon::ChevronLeft).style(ButtonStyle::Borderless).onClick([&] {
        navigate(finder::parentPath(browser->path));
    });
    auto hidden = Button("隐藏文件").style(ButtonStyle::Borderless).onClick([&] {
        browser->showHidden = !browser->showHidden;
        reload();
    });
    auto footer = HStack({filterPicker, Spacer(), Button("取消").onClick(cancel), accept}).spacing(12).padding(12, 20);
    auto filename = HStack({Label("存储为："), name}).spacing(12).padding(8, 20);
    filename.setHidden(!save);
    auto content = VStack({HStack({up, location, viewMode, search}).spacing(10).padding(12, 16), headerElement,
                           scroll.fillHeight(), HStack({status, Spacer(), hidden}).padding(6, 20), filename, footer})
                       .spacing(0);
    window.setContent(SplitView(sidebar, content).sidebarWidth(190));
    std::string initial = request["folder"].toString().toStdString();
    if (initial.empty()) initial = finder::homeDirectory();
    navigate(initial);
    if (browser->path.empty()) navigate(finder::homeDirectory());
    app.after(0, [&] {
        auto* target = save ? static_cast<Widget*>(accept.node().get()) : static_cast<Widget*>(grid.get());
        if (target->host()) target->host()->setFocus(target, true);
    });
    return app.run();
}
