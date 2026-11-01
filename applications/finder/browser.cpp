#include "browser.hpp"

#include <algorithm>
#include <cmath>

#include "file_icons.hpp"

using namespace mayo;

namespace finder {

// --- Browser -----------------------------------------------------------------------------------------

const FileEntry* Browser::find(const std::string& p) const {
    for (const auto& e : entries) {
        if (e.path == p) return &e;
    }
    return nullptr;
}

std::vector<std::string> Browser::selectedPaths() const {
    // Visual order where possible, then anything selected elsewhere (expanded list rows, other columns).
    std::vector<std::string> out;
    for (const auto& e : entries) {
        if (selection.count(e.path)) out.push_back(e.path);
    }
    for (const auto& p : selection) {
        if (std::find(out.begin(), out.end(), p) == out.end()) out.push_back(p);
    }
    return out;
}

void Browser::select(const std::string& p, bool notify) {
    selection.clear();
    if (!p.empty()) selection.insert(p);
    lead = anchor = p;
    if (notify) notifySelection();
}

// --- text helpers ------------------------------------------------------------------------------------

namespace {

std::vector<size_t> charBoundaries(const std::string& s) {
    std::vector<size_t> b;
    for (size_t i = 0; i < s.size(); ++i) {
        if ((static_cast<unsigned char>(s[i]) & 0xC0) != 0x80) b.push_back(i);
    }
    b.push_back(s.size());
    return b;
}

}  // namespace

std::string truncateMiddle(Painter& p, const std::string& text, const Font& font, float width) {
    if (p.measureText(text, font) <= width) return text;
    const auto b = charBoundaries(text);
    const size_t n = b.size() - 1;
    // Largest number of kept characters whose head + "…" + tail fits.
    size_t lo = 0;
    size_t hi = n;
    auto make = [&](size_t keep) {
        size_t head = (keep + 1) / 2;
        size_t tail = keep / 2;
        return text.substr(0, b[head]) + "…" + text.substr(b[n - tail]);
    };
    while (lo < hi) {
        size_t mid = (lo + hi + 1) / 2;
        if (p.measureText(make(mid), font) <= width) lo = mid;
        else hi = mid - 1;
    }
    return make(lo);
}

std::string truncateEnd(Painter& p, const std::string& text, const Font& font, float width) {
    if (p.measureText(text, font) <= width) return text;
    const auto b = charBoundaries(text);
    size_t lo = 0;
    size_t hi = b.size() - 1;
    while (lo < hi) {
        size_t mid = (lo + hi + 1) / 2;
        if (p.measureText(text.substr(0, b[mid]) + "…", font) <= width) lo = mid;
        else hi = mid - 1;
    }
    return text.substr(0, b[lo]) + "…";
}

// --- drag ghost ------------------------------------------------------------------------------------

namespace {

/// Translucent icons following the pointer during a drag, with a count badge.
class DragGhost final : public Widget, public PopupHost {
public:
    DragGhost(std::vector<FileEntry> items, size_t count, PreviewCache* previews)
        : m_items(std::move(items)), m_count(count), m_previews(previews) {}

    Point position;
    bool copy = false;

    Size measureContent(Size /*available*/) override { return {kSize + 24, kSize + 24}; }
    Rect placePopup(Size /*window*/, Size measured) override {
        return {position.x - measured.width * 0.5f, position.y - measured.height * 0.5f, measured.width, measured.height};
    }
    Widget* hitTest(Point /*p*/) override { return nullptr; }

    void draw(Painter& p) override {
        const Theme& t = theme();
        const Rect r = frame();
        p.pushOpacity(0.8f);
        const int shown = static_cast<int>(std::min<size_t>(m_items.size(), 3));
        for (int i = shown - 1; i >= 0; --i) {
            const float off = static_cast<float>(i) * 5;
            Rect icon{r.center().x - kSize * 0.5f + off, r.center().y - kSize * 0.5f - off, kSize, kSize};
            const FileEntry& e = m_items[static_cast<size_t>(i)];
            IconStyle style;
            style.preview = m_previews ? m_previews->get(e, kSize * p.scale()) : nullptr;
            style.folderGlyph = e.isDir ? folderGlyphFor(e.path) : Icon::None;
            drawFileIcon(p, icon, e, style);
        }
        p.popOpacity();
        if (m_count > 1 || copy) {
            // macOS: red count badge; a green "+" when the drop will copy.
            const Font f = t.font(TextStyle::Caption).withWeight(FontWeight::Semibold);
            std::string label = m_count > 1 ? std::to_string(m_count) : "";
            float w = std::max(20.0f, p.measureText(label, f) + 12);
            Rect badge{r.right() - w - 2, r.y + 2, w, 20};
            p.drawShadow(badge, 10, Color::black(0.3f), 4, {0, 1});
            p.fillRoundedRect(badge, 10, copy ? t.colors.green : t.colors.red);
            if (copy && m_count <= 1) {
                p.drawIcon(Icon::Plus, badge.inset(4), Color::white(), 1.3f);
            } else {
                p.drawText(label, badge, f, Color::white(), TextAlign::Center);
            }
        }
    }

private:
    static constexpr float kSize = 56;
    std::vector<FileEntry> m_items;
    size_t m_count;
    PreviewCache* m_previews;
};

}  // namespace

// --- RenameField -----------------------------------------------------------------------------------

bool RenameField::onKeyDown(const KeyEvent& e) {
    if (e.key == Key::Escape) {
        finish(false);
        return true;
    }
    TextFieldView::onKeyDown(e);
    return true;  // keep Finder shortcuts (Space, Delete, arrows) away from the views while editing
}

void RenameField::onFocusChanged(bool focused) {
    TextFieldView::onFocusChanged(focused);
    if (!focused) finish(true);  // clicking elsewhere commits, like the Finder
}

void RenameField::finish(bool commit) {
    if (m_done) return;
    m_done = true;
    auto keep = shared_from_this();
    if (onEnd) onEnd(commit);
}

// --- FileView ------------------------------------------------------------------------------------

bool FileView::emphasized() const { return isFocused() && host() && host()->isKeyWindow(); }

Color FileView::selectionColor() const {
    const auto& c = theme().colors;
    return emphasized() ? c.accent : c.unemphasizedSelection.mix(c.fill, 0.4f);
}

Icon FileView::glyphFor(const FileEntry& e) const { return e.isDir ? folderGlyphFor(e.path) : Icon::None; }

std::string FileView::neighbour(const std::string& from, Key key) const {
    auto items = order();
    if (items.empty()) return {};
    auto it = std::find_if(items.begin(), items.end(), [&](const FileEntry* e) { return e->path == from; });
    if (it == items.end()) return (key == Key::Up || key == Key::Left) ? items.back()->path : items.front()->path;
    if (key == Key::Up || key == Key::Left) return it == items.begin() ? (*it)->path : (*(it - 1))->path;
    if (key == Key::Down || key == Key::Right) return it + 1 == items.end() ? (*it)->path : (*(it + 1))->path;
    return {};
}

bool FileView::onPointerDown(const PointerEvent& e) {
    if (auto* h = host()) h->setFocus(this, false);
    const std::string item = itemAt(e.position);
    m_leftPress = e.button == MouseButton::Left;
    m_downAt = e.position;
    m_downItem = item;
    m_collapseOnUp = false;
    m_banding = false;
    m_dragging = false;
    m_external = false;
    if (!ownsSelection()) {
        activate(item);
        if (auto* h = host()) h->setFocus(this, false);
        if (e.button == MouseButton::Right && m_browser->contextMenu) m_browser->contextMenu(*this, e.position, !item.empty());
        return true;
    }
    Browser& b = *m_browser;
    const bool ctrl = hasModifier(e.modifiers, Modifiers::Ctrl);
    const bool shift = hasModifier(e.modifiers, Modifiers::Shift);
    if (e.button == MouseButton::Right) {
        if (!item.empty() && !b.isSelected(item)) b.select(item);
        if (item.empty() && !b.selection.empty()) b.select({});
        invalidate();
        if (b.contextMenu) b.contextMenu(*this, e.position, !item.empty());
        return true;
    }
    if (e.button != MouseButton::Left) return true;
    if (!item.empty() && e.clickCount >= 2) {
        if (const FileEntry* entry = this->entry(item); entry && b.open) {
            FileEntry copy = *entry;  // open() may reload the listing under us
            b.open(copy);
        }
        return true;
    }
    if (item.empty()) {
        if (!ctrl && !shift && !b.selection.empty()) b.select({});
        m_banding = true;
        m_band = {e.position.x, e.position.y, 0, 0};
        m_bandBase = ctrl || shift ? b.selection : std::set<std::string>{};
        invalidate();
        return true;
    }
    clickItem(item, e);
    return true;
}

void FileView::clickItem(const std::string& item, const PointerEvent& e) {
    Browser& b = *m_browser;
    if (hasModifier(e.modifiers, Modifiers::Shift) && !b.anchor.empty()) {
        selectRange(b.anchor, item, hasModifier(e.modifiers, Modifiers::Ctrl));
        b.lead = item;
    } else if (hasModifier(e.modifiers, Modifiers::Ctrl)) {
        if (b.isSelected(item)) b.selection.erase(item);
        else b.selection.insert(item);
        b.lead = b.anchor = item;
    } else if (b.isSelected(item)) {
        m_collapseOnUp = b.selection.size() > 1;  // keep the group for a drag
        b.lead = b.anchor = item;
    } else {
        b.select(item, false);
    }
    b.notifySelection();
    invalidate();
}

void FileView::selectRange(const std::string& from, const std::string& to, bool additive) {
    auto items = order();
    auto ia = std::find_if(items.begin(), items.end(), [&](const FileEntry* e) { return e->path == from; });
    auto ib = std::find_if(items.begin(), items.end(), [&](const FileEntry* e) { return e->path == to; });
    if (ia == items.end() || ib == items.end()) {
        m_browser->select(to, false);
        return;
    }
    if (ia > ib) std::swap(ia, ib);
    if (!additive) m_browser->selection.clear();
    for (auto it = ia; it <= ib; ++it) m_browser->selection.insert((*it)->path);
}

void FileView::onPointerMove(const PointerEvent& e) {
    if (!m_leftPress && !m_dragging && !m_banding) {
        std::string h = itemAt(e.position);
        if (h != m_hover) {
            m_hover = std::move(h);
            invalidate();
        }
        return;
    }
    if (m_dragging) {
        if (m_external) return;
        // Leaving the window hands the files to the system, so they can be dropped on other apps (a browser
        // upload, an email, another file manager). Inside the window the drag stays ours.
        const Size win = host() ? host()->windowSize() : Size{};
        const bool outside = e.position.x < 0 || e.position.y < 0 || e.position.x >= win.width || e.position.y >= win.height;
        if (outside && host() && host()->startFileDrag(m_dragPaths)) {
            m_external = true;
            m_browser->dropHover.clear();
            if (m_ghost) {
                host()->dismissOverlay(m_ghost.get());
                m_ghost.reset();
            }
            invalidate();
            return;
        }
        if (auto* ghost = dynamic_cast<DragGhost*>(m_ghost.get())) ghost->copy = hasModifier(e.modifiers, Modifiers::Ctrl);
        updateDrag(e.position);
        return;
    }
    const Vec2 d = e.position - m_downAt;
    if (m_leftPress && !m_downItem.empty() && ownsSelection() && m_browser->isSelected(m_downItem) && std::hypot(d.x, d.y) > 4) {
        beginDrag(e.position);
        return;
    }
    if (m_banding) updateBand(e.position);
}

void FileView::onPointerUp(const PointerEvent& e) {
    m_leftPress = false;
    if (m_external) {
        // The drop happened in another app (or was cancelled); nothing to do here.
        m_external = false;
        m_dragging = false;
        m_collapseOnUp = false;
        return;
    }
    if (m_dragging) {
        endDrag(e.position, hasModifier(e.modifiers, Modifiers::Ctrl));
    } else if (m_collapseOnUp && !m_downItem.empty()) {
        m_browser->select(m_downItem);
    }
    m_collapseOnUp = false;
    if (m_banding) {
        m_banding = false;
        invalidate();
    }
}

void FileView::onPointerLeave() {
    if (!m_hover.empty()) {
        m_hover.clear();
        invalidate();
    }
}

void FileView::updateBand(Point p) {
    m_band = Rect::fromLTRB(std::min(m_downAt.x, p.x), std::min(m_downAt.y, p.y), std::max(m_downAt.x, p.x),
                            std::max(m_downAt.y, p.y));
    std::set<std::string> sel = m_bandBase;
    for (const FileEntry* e : order()) {
        if (itemRect(e->path).intersects(m_band)) sel.insert(e->path);
    }
    if (sel != m_browser->selection) {
        m_browser->selection = std::move(sel);
        m_browser->notifySelection();
    }
    invalidate();
}

void FileView::drawOverlay(Painter& p) {
    if (m_banding && m_band.width > 1 && m_band.height > 1) {
        const auto& c = theme().colors;
        p.fillRoundedRect(m_band, 2, c.accent.withAlpha(0.12f), c.accent.withAlpha(0.6f), 1);
    }
    Widget::drawOverlay(p);
}

Widget* FileView::windowRoot() {
    Widget* w = this;
    while (w->parent()) w = w->parent();
    return w;
}

void FileView::beginDrag(Point p) {
    m_dragging = true;
    m_collapseOnUp = false;
    m_dragPaths = m_browser->selectedPaths();
    std::vector<FileEntry> items;
    for (const auto& path : m_dragPaths) {
        if (items.size() >= 3) break;
        if (const FileEntry* e = entry(path)) items.push_back(*e);
        else if (auto st = statEntry(path)) items.push_back(*st);
    }
    auto ghost = std::make_shared<DragGhost>(std::move(items), m_dragPaths.size(), m_browser->previews);
    ghost->position = p;
    m_ghost = ghost;
    if (auto* h = host()) h->showOverlay(ghost);
    updateDrag(p);
}

void FileView::updateDrag(Point p) {
    if (auto* ghost = dynamic_cast<DragGhost*>(m_ghost.get())) {
        ghost->position = p;
        ghost->invalidateLayout();
    }
    std::string folder;
    for (Widget* w = windowRoot()->hitTest(p); w; w = w->parent()) {
        if (auto* target = dynamic_cast<DropTarget*>(w)) {
            folder = target->dropFolderAt(p);
            break;
        }
    }
    // Never into one of the dragged folders (or below it).
    for (const auto& src : m_dragPaths) {
        if (!folder.empty() && isInside(folder, src)) folder.clear();
    }
    if (folder != m_browser->dropHover) {
        m_browser->dropHover = folder;
        if (auto* h = host()) h->requestRedraw();
    }
}

void FileView::endDrag(Point p, bool copy) {
    updateDrag(p);
    const std::string folder = m_browser->dropHover;
    m_browser->dropHover.clear();
    m_dragging = false;
    if (m_ghost) {
        if (auto* h = host()) h->dismissOverlay(m_ghost.get());
        m_ghost.reset();
    }
    invalidate();
    if (folder.empty() || !m_browser->drop) return;
    const bool alreadyThere = std::all_of(m_dragPaths.begin(), m_dragPaths.end(),
                                          [&](const std::string& s) { return parentPath(s) == folder; });
    if (alreadyThere && !copy) return;
    auto paths = m_dragPaths;
    m_browser->drop(paths, folder, copy);
}

std::string FileView::dropFolderAt(Point p) {
    std::string item = itemAt(p);
    if (!item.empty()) {
        const FileEntry* e = entry(item);
        if (e && e->isDir) return item;
    }
    return folder();
}

bool FileView::onKeyDown(const KeyEvent& e) {
    if (handleKey(e)) return true;
    Browser& b = *m_browser;
    auto items = order();
    std::string from = !b.lead.empty() && b.isSelected(b.lead) ? b.lead
                       : b.selection.empty()                    ? std::string()
                                                                : *b.selection.begin();
    std::string next;
    switch (e.key) {
        case Key::Up:
        case Key::Down:
        case Key::Left:
        case Key::Right:
            if (e.primary() || e.alt()) return false;  // Ctrl+Up/Down are window shortcuts
            next = neighbour(from, e.key);
            break;
        case Key::Home:
            if (!items.empty()) next = items.front()->path;
            break;
        case Key::End:
            if (!items.empty()) next = items.back()->path;
            break;
        case Key::Enter:
            if (b.renameSelection) b.renameSelection();
            return true;
        case Key::Space:
            if (b.quickLook) b.quickLook();
            return true;
        default: break;
    }
    if (!next.empty()) {
        if (e.shift() && !b.anchor.empty()) {
            selectRange(b.anchor, next, false);
            b.lead = next;
            b.notifySelection();
        } else {
            b.select(next);
        }
        reveal(next);
        invalidate();
        return true;
    }
    // Type to select: letters and digits jump to the first name starting with what was typed.
    const bool letter = e.key >= Key::A && e.key <= Key::Z;
    const bool digit = e.key >= Key::Num0 && e.key <= Key::Num9;
    if ((letter || digit) && !e.primary() && !e.alt()) {
        char c = letter ? static_cast<char>('a' + (static_cast<int>(e.key) - static_cast<int>(Key::A)))
                        : static_cast<char>('0' + (static_cast<int>(e.key) - static_cast<int>(Key::Num0)));
        const double t = now();
        if (t - m_typedAt > 1.0) m_typed.clear();
        m_typedAt = t;
        m_typed.push_back(c);
        const FileEntry* best = nullptr;
        for (const FileEntry* it : items) {
            if (startsWithNoCase(it->name, m_typed)) {
                best = it;
                break;
            }
        }
        if (!best) {  // nearest following name, like the Finder
            for (const FileEntry* it : items) {
                if (compareNames(it->name, m_typed) >= 0 && (!best || compareNames(it->name, best->name) < 0)) best = it;
            }
        }
        if (best) {
            b.select(best->path);
            reveal(best->path);
            invalidate();
        }
        return true;
    }
    return false;
}

bool FileView::onTextInput(const TextInputEvent& /*e*/) { return false; }

void FileView::reveal(const std::string& path, bool animated) {
    if (!m_scroller || path.empty()) return;
    const Rect r = itemRect(path);
    if (r.width <= 0) return;
    const Rect view = m_scroller->frame();
    const float top = view.y + m_revealInset;
    const float bottom = view.bottom();
    float offset = m_scroller->offset().y;
    if (r.y < top + 4) offset -= top + 4 - r.y;
    else if (r.bottom() > bottom - 4) offset += r.bottom() - (bottom - 4);
    if (offset != m_scroller->offset().y) m_scroller->scrollTo({m_scroller->offset().x, offset}, animated);
}

// --- inline rename ---------------------------------------------------------------------------------

void FileView::beginRename(const std::string& path) {
    if (m_rename) m_rename->finish(true);
    const FileEntry* e = entry(path);
    if (!e) return;
    reveal(path, false);
    m_renamePath = path;
    auto field = std::make_shared<RenameField>();
    field->setText(e->name);
    std::weak_ptr<Widget> weakSelf = weak_from_this();
    std::weak_ptr<RenameField> weakField = field;
    const std::string original = e->name;
    field->setOnSubmit([weakField](const std::string&) {
        if (auto f = weakField.lock()) f->finish(true);
    });
    field->onEnd = [this, weakSelf, weakField, path, original](bool commit) {
        auto self = weakSelf.lock();
        auto f = weakField.lock();
        if (!self || !f) return;
        const std::string text = f->text();
        // Detach on the next loop iteration: we are inside the field's own focus / key handler.
        App::current().post([this, weakSelf, f] {
            auto keep = weakSelf.lock();
            if (!keep) return;
            if (m_rename == f) m_rename.reset();
            removeChild(f.get());
            if (auto* h = host(); h && !h->focusedWidget()) h->setFocus(this, false);
            invalidateLayout();
        });
        if (auto* h = host(); h && h->focusedWidget() == f.get()) h->setFocus(this, false);
        if (commit && !text.empty() && text != original && m_browser->rename) m_browser->rename(path, text);
    };
    m_rename = field;
    addChild(field);
    invalidateLayout();
    // Select the name without its extension, like the Finder (applied after layout, see arrangeContent).
    m_renameStem = e->isDir ? e->name.size() : e->name.rfind('.');
    if (m_renameStem == std::string::npos || m_renameStem == 0) m_renameStem = e->name.size();
    if (auto* h = host()) h->setFocus(field.get(), true);
}

void FileView::arrangeContent(const Rect& content) {
    Widget::arrangeContent(content);
    if (m_rename) {
        Rect nr = nameRect(m_renamePath);
        const float h = 22;
        Size s = m_rename->measure({std::max(nr.width + 12, 90.0f), h});
        (void)s;
        m_rename->arrange({nr.x - 6, nr.center().y - h * 0.5f, std::max(nr.width + 12, 90.0f), h});
        if (m_renameStem != std::string::npos) {
            m_rename->moveCaret(0, false);
            m_rename->moveCaret(m_renameStem, true);
            m_renameStem = std::string::npos;
        }
    }
}

}  // namespace finder
