// Store pages combine Forma controls (Sidebar, Label, Button) with custom editorial
// artwork, image shelves and responsive catalogue layouts, backed by StoreModel.
#pragma once

#include <mayonnaise/mayonnaise.hpp>

#include <functional>
#include <map>
#include <memory>
#include <set>
#include <string>
#include <vector>

#include "catalog.hpp"
#include "images.hpp"

namespace forma::store {

struct StoreModel;

struct Callbacks {
    std::function<void(std::string)> openApp;
    std::function<void(std::string)> openSection;
    std::function<void(std::string)> openCategory;
    std::function<void(std::string)> search;
    std::function<void(std::string)> install;
    std::function<void(std::string)> update;
    std::function<void(std::string)> launch;
    std::function<void(std::string)> uninstall;
    std::function<void()> updateAll;
    std::function<void(std::string)> copyText;  ///< clipboard (app id, home page)
    std::function<void(std::string, mayo::Point)> contextMenu;
};

enum class InstallState : unsigned char { Get, Busy, Open, Update, Broken, Unavailable };

struct StoreModel {
    Catalog catalog;
    ImageCache images;
    Callbacks callbacks;
    std::set<std::string> busy;                 ///< install/update in progress
    std::map<std::string, float> progress;      ///< 0..1, or -1 for indeterminate
    std::map<std::string, std::string> stage;   ///< "下载中 42%", "正在安装…"
    std::set<std::string> broken;               ///< last attempt failed
    std::string installing;                     ///< the app currently being changed
    std::string query;
    int updateCount = 0;
    int heroIndex = 0;
    bool offline = false;

    [[nodiscard]] const AppEntry* find(const std::string& id) const { return catalog.find(id); }
    [[nodiscard]] AppEntry* findMutable(const std::string& id) { return const_cast<AppEntry*>(catalog.find(id)); }
    [[nodiscard]] InstallState stateOf(const AppEntry& app) const;
    /// 0..1 progress of the app being installed, -1 when indeterminate.
    [[nodiscard]] float progressOf(const std::string& id) const;
};

/// A full-width page displayed inside the detail scroll view.
std::shared_ptr<mayo::Widget> discoverPage(StoreModel& model);
std::shared_ptr<mayo::Widget> sectionPage(StoreModel& model, const std::string& sectionId);
std::shared_ptr<mayo::Widget> categoryPage(StoreModel& model, const std::string& category);
std::shared_ptr<mayo::Widget> searchPage(StoreModel& model, const std::string& query);
std::shared_ptr<mayo::Widget> updatesPage(StoreModel& model);
std::shared_ptr<mayo::Widget> installedPage(StoreModel& model);
std::shared_ptr<mayo::Widget> appPage(StoreModel& model, const std::string& id);
std::shared_ptr<mayo::Widget> categoriesPage(StoreModel& model);
std::shared_ptr<mayo::Widget> settingsPage(StoreModel& model);

/// The store's sidebar body: search field, section list and the account footer. Returned widget
/// starts below the traffic lights; the app places it on the SplitView's glass panel.
std::shared_ptr<mayo::Widget> sidebarBody(StoreModel& model, const std::string& selected,
                                          std::function<void(const std::string&)> onSelect,
                                          const mayo::Element& searchField);

/// Updates the sidebar's selection highlight (state changes from app pages / search).
void sidebarSelect(mayo::Widget& sidebar, const std::string& id);

/// Sidebar section ids in display order (includes "updates" and "installed").
const std::vector<std::pair<std::string, std::string>>& sidebarSections();

}  // namespace forma::store
