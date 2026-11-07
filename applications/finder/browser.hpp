#pragma once

// State shared by the Finder's views, and the behaviour they have in common: Finder-style selection
// (click, Ctrl-click, Shift-click, rubber band, type-to-select), inline renaming and drag and drop.

#include <mayonnaise/mayonnaise.hpp>

#include <functional>
#include <memory>
#include <set>
#include <string>
#include <vector>

#include "file_model.hpp"
#include "preview_cache.hpp"

namespace finder {

enum class ViewMode : std::uint8_t { Icons, List, Columns };

struct Browser {
    std::string path;                  ///< current folder (or kRecents)
    std::vector<FileEntry> entries;    ///< current folder or search results, filtered and sorted
    std::set<std::string> selection;   ///< absolute paths
    std::string lead;                  ///< keyboard cursor
    std::string anchor;                ///< Shift-click range start
    SortOrder sort;
    bool showHidden = false;
    bool searching = false;            ///< entries are search results, not the folder
    std::set<std::string> cut;         ///< drawn dimmed until pasted
    float iconSize = 64;
    std::string dropHover;             ///< folder highlighted under a drag
    PreviewCache* previews = nullptr;

    // Hooks implemented by the window controller.
    std::function<void(const FileEntry&)> open;
    std::function<void(const std::string& folder, const std::string& select)> navigate;
    std::function<void()> selectionChanged;
    std::function<void(mayo::Widget& from, mayo::Point at, bool onItem)> contextMenu;
    std::function<void(const std::string& path, const std::string& newName)> rename;
    std::function<void(const std::vector<std::string>& paths, const std::string& folder, bool copy)> drop;
    std::function<void()> quickLook;
    std::function<void()> sortChanged;
    std::function<void()> renameSelection;
    std::function<void(float size)> resizeIcons;  ///< Ctrl+scroll / pinch in the icon view
    /// Sorted, filtered listing of another folder (list view disclosure, columns). Cached by the owner.
    std::function<const std::vector<FileEntry>&(const std::string& folder)> listing;

    [[nodiscard]] bool isSelected(const std::string& p) const { return selection.count(p) > 0; }
    [[nodiscard]] const FileEntry* find(const std::string& p) const;
    [[nodiscard]] std::vector<std::string> selectedPaths() const;
    void select(const std::string& p, bool notify = true);  ///< only this one
    void notifySelection() const {
        if (selectionChanged) selectionChanged();
    }
};

/// Implemented by widgets that accept dropped items; returns the destination folder at `p` ("" = none).
class DropTarget {
public:
    virtual ~DropTarget() = default;
    virtual std::string dropFolderAt(mayo::Point p) = 0;
};

/// Single-line editor placed over an item's name.
class RenameField : public mayo::TextFieldView {
public:
    RenameField() : TextFieldView({}) {}
    std::function<void(bool commit)> onEnd;
    bool onKeyDown(const mayo::KeyEvent& e) override;
    void onFocusChanged(bool focused) override;
    void finish(bool commit);

private:
    bool m_done = false;
};

/// Base class of the icon grid, list rows and browser columns.
class FileView : public mayo::Widget, public DropTarget {
public:
    explicit FileView(std::shared_ptr<Browser> browser) : m_browser(std::move(browser)) {}

    [[nodiscard]] bool acceptsFocus() const override { return true; }
    [[nodiscard]] bool isInteractive() const override { return true; }
    bool onPointerDown(const mayo::PointerEvent& e) override;
    void onPointerMove(const mayo::PointerEvent& e) override;
    void onPointerUp(const mayo::PointerEvent& e) override;
    void onPointerLeave() override;
    bool onKeyDown(const mayo::KeyEvent& e) override;
    bool onTextInput(const mayo::TextInputEvent& e) override;
    void onFocusChanged(bool focused) override {
        if (!focused) resetClickSequence();
        invalidate();
    }
    void drawOverlay(mayo::Painter& p) override;
    void arrangeContent(const mayo::Rect& content) override;
    std::string dropFolderAt(mayo::Point p) override;

    /// Starts editing the name of `path` in place.
    void beginRename(const std::string& path);
    [[nodiscard]] bool isRenaming() const { return m_rename != nullptr; }
    void setScroller(mayo::ScrollViewView* s, float revealInset = 0) {
        m_scroller = s;
        m_revealInset = revealInset;
    }
    /// Scrolls so the item is visible.
    void reveal(const std::string& path, bool animated = true);
    /// Entries changed; drop cached layout.
    virtual void reload() {
        resetClickSequence();
        invalidateLayout();
    }

protected:
    /// Items in visual order (for ranges, arrows and type-select).
    [[nodiscard]] virtual std::vector<const FileEntry*> order() const = 0;
    [[nodiscard]] virtual std::string itemAt(mayo::Point p) const = 0;
    [[nodiscard]] virtual const FileEntry* entry(const std::string& path) const { return m_browser->find(path); }
    /// Folder this view lists (drop destination for its background); "" for search results and Recents.
    [[nodiscard]] virtual std::string folder() const {
        return m_browser->searching || m_browser->path == kRecents ? std::string() : m_browser->path;
    }
    /// Bounds of the item (hit area) and of its name label, in window points.
    [[nodiscard]] virtual mayo::Rect itemRect(const std::string& path) const = 0;
    [[nodiscard]] virtual mayo::Rect nameRect(const std::string& path) const = 0;
    /// Arrow key navigation; return the path the lead moves to ("" = not handled).
    [[nodiscard]] virtual std::string neighbour(const std::string& from, mayo::Key key) const;
    /// Whether this view shows the browser's current selection (columns other than the active one don't).
    [[nodiscard]] virtual bool ownsSelection() const { return true; }
    /// Click in a view that doesn't own the selection.
    virtual void activate(const std::string& /*path*/) {}
    virtual bool handleKey(const mayo::KeyEvent& /*e*/) { return false; }
    [[nodiscard]] bool emphasized() const;  ///< focused in the key window: accent selection
    [[nodiscard]] mayo::Color selectionColor() const;
    [[nodiscard]] mayo::Icon glyphFor(const FileEntry& e) const;
    void resetClickSequence();

    std::shared_ptr<Browser> m_browser;
    mayo::ScrollViewView* m_scroller = nullptr;
    float m_revealInset = 0;  // part of the scroller hidden under the toolbar
    std::string m_hover;

private:
    void clickItem(const std::string& path, const mayo::PointerEvent& e);
    void selectRange(const std::string& from, const std::string& to, bool additive);
    void updateBand(mayo::Point p);
    void beginDrag(mayo::Point p);
    void updateDrag(mayo::Point p);
    void endDrag(mayo::Point p, bool copy);
    mayo::Widget* windowRoot();

    // Pointer gesture state.
    mayo::Point m_downAt;
    std::string m_downItem;
    std::string m_lastClickItem;
    int m_lastClickCount = 0;
    bool m_plainPress = false;
    bool m_openOnUp = false;
    bool m_collapseOnUp = false;  // click on an already selected item: select only it, unless a drag starts
    bool m_leftPress = false;
    bool m_banding = false;
    mayo::Rect m_band;
    std::set<std::string> m_bandBase;
    bool m_dragging = false;
    bool m_external = false;  // the drag left the window and was handed to the system (other apps)
    std::vector<std::string> m_dragPaths;
    std::shared_ptr<mayo::Widget> m_ghost;
    // Type-to-select.
    std::string m_typed;
    double m_typedAt = 0;
    // Inline rename.
    std::shared_ptr<RenameField> m_rename;
    std::string m_renamePath;
    size_t m_renameStem = std::string::npos;  // selection applied once the field has a size
};

/// Middle-truncates `text` ("Long fi…name.txt") to fit `width`.
std::string truncateMiddle(mayo::Painter& p, const std::string& text, const mayo::Font& font, float width);
/// End-truncates `text` with an ellipsis.
std::string truncateEnd(mayo::Painter& p, const std::string& text, const mayo::Font& font, float width);

}  // namespace finder
