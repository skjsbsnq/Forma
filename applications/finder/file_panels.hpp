#pragma once

// The panels that answer "with what?" and "with what rights?": "打开方式" (the application for an item, optionally
// for every item of its kind) and "属性" (the Get Info sheet, with editable permissions).

#include <mayonnaise/mayonnaise.hpp>

#include <functional>
#include <memory>
#include <string>
#include <vector>

#include "file_model.hpp"
#include "file_ops.hpp"
#include "panels.hpp"

namespace finder {

struct AppChoice {
    std::string id;
    std::string name;
    std::string icon;
    bool current = false;  ///< the application items of this kind currently open with
};

/// Applications that can open the item, the current default first.
std::vector<AppChoice> applicationChoices(const FileEntry& entry);
/// The themed icon of an application, decoded and cached (menus and panels share it).
std::shared_ptr<mayo::ImageData> applicationIconImage(const std::string& iconName);
/// Starts `id` on the item. Returns "" on success, or the error to report.
std::string openEntryWith(const FileEntry& entry, const std::string& id);
/// Why `id` can't become the default for items like `entry`, or "".
std::string rememberProblem(const FileEntry& entry, const std::string& id);
/// Makes `id` the default in $XDG_CONFIG_HOME/mimeapps.list; returns the error, or "".
std::string rememberApplication(const FileEntry& entry, const std::string& id);

/// macOS's "Open With" panel: the applications, an "always" box and a footer with OK / Cancel.
class OpenWithPanel : public CenteredPanel {
public:
    /// `entry` may be empty for a plain chooser (no "always" box then). `onDone` runs once the panel has closed,
    /// whether or not an application was started.
    OpenWithPanel(FileEntry entry, std::function<void()> onDone);
    void setOnOpen(std::function<std::string(const std::string& id, bool remember)> fn) { m_onOpen = std::move(fn); }
    /// The application the panel preselects (or nothing).
    void select(const std::string& id);
    /// Ticks the "always" box (`on` may be ignored: folders and trashed items can't be remembered).
    void setRemember(bool on);

    [[nodiscard]] mayo::Size measureContent(mayo::Size available) override;
    [[nodiscard]] bool acceptsFocus() const override { return true; }
    [[nodiscard]] bool isInteractive() const override { return true; }
    bool onPointerDown(const mayo::PointerEvent& e) override;
    void onPointerMove(const mayo::PointerEvent& e) override;
    void onPointerUp(const mayo::PointerEvent& e) override;
    void onPointerLeave() override;
    bool onScroll(const mayo::ScrollEvent& e) override;
    bool onKeyDown(const mayo::KeyEvent& e) override;

protected:
    void drawPanel(mayo::Painter& p) override;

private:
    [[nodiscard]] bool canRemember() const;
    [[nodiscard]] float listHeight() const;
    [[nodiscard]] int visibleRows() const;
    [[nodiscard]] float footerY() const;
    [[nodiscard]] mayo::Rect rowRect(int index) const;
    [[nodiscard]] int rowAt(mayo::Point p) const;
    [[nodiscard]] mayo::Rect rememberBox() const;
    [[nodiscard]] mayo::Rect okRect() const;
    [[nodiscard]] mayo::Rect cancelRect() const;
    void open(int index);
    void close();

    FileEntry m_entry;
    std::vector<AppChoice> m_choices;
    std::vector<std::shared_ptr<mayo::ImageData>> m_icons;
    std::function<std::string(const std::string& id, bool remember)> m_onOpen;
    std::function<void()> m_onDone;
    bool m_remember = false;
    std::string m_selected;
    float m_top = 0;  // first visible row (fractional while scrolling)
    int m_hover = -1;
    bool m_hoverRemember = false;
    bool m_hoverOk = false;
    bool m_hoverCancel = false;
    bool m_opening = false;
};

/// "属性" (Get Info): the icon, size, dates, where it is, the original for links and trashed items, the
/// application they open with (with "更改…"), and the permissions, editable like macOS's Get Info (read & write /
/// read only / no access for owner, group and others). `appButton` opens the "打开方式" panel; `changed` runs after
/// a permission change went through.
void presentProperties(mayo::WidgetHost& host, const FileEntry& entry, const mayo::Element& appButton,
                       std::function<void()> changed);
/// Closes the properties sheet (before another centered panel opens, or two would stack).
void closeProperties(mayo::WidgetHost& host);
/// Fills "大小" of an open properties sheet once the folder walk is done.
void setLatestFolderSize(std::uintmax_t bytes, size_t items);
/// Why a permission change can't be applied (read-only volume), or "".
std::string permissionsProblem(const FileEntry& entry);

}  // namespace finder
