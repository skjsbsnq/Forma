#include "views.hpp"

#include <algorithm>
#include <cmath>
#include <filesystem>

#include "file_icons.hpp"

using namespace mayo;

namespace finder {

namespace {

constexpr float kGridPadX = 14;
constexpr float kGridPadTop = 10;
constexpr float kLabelLine = 15;

Font labelFont() { return theme().font(TextStyle::Callout); }

/// Fill for the rows of a contiguous selection: inner corners are square so the run reads as one shape.
CornerRadii runRadii(bool joinAbove, bool joinBelow, float r) {
    return {joinAbove ? 0.0f : r, joinAbove ? 0.0f : r, joinBelow ? 0.0f : r, joinBelow ? 0.0f : r};
}

Color unfocusedSelection() {
    const auto& c = theme().colors;
    return c.unemphasizedSelection.mix(c.fill, 0.4f);
}

IconStyle styleFor(const Browser& b, const FileEntry& e, float pixels) {
    IconStyle s;
    s.preview = b.previews ? b.previews->get(e, pixels) : nullptr;
    s.folderGlyph = e.isDir ? folderGlyphFor(e.path) : Icon::None;
    s.dimmed = b.cut.count(e.path) > 0 || e.isHidden;
    return s;
}

/// Small row icon with its preview (list and column views).
void drawRowIcon(Painter& p, const Browser& b, const Rect& r, const FileEntry& e) {
    drawFileIcon(p, r, e, styleFor(b, e, r.width * p.scale()));
}

}  // namespace

// --- IconGridView ----------------------------------------------------------------------------------

void IconGridView::reload() {
    m_index.clear();
    for (size_t i = 0; i < m_browser->entries.size(); ++i) m_index[m_browser->entries[i].path] = static_cast<int>(i);
    m_labels.clear();
    m_labelExtents.clear();
    invalidateLayout();
}

bool IconGridView::onScroll(const ScrollEvent& e) {
    // Plain scrolling belongs to the ScrollView; Ctrl+scroll resizes the icons.
    if (!hasModifier(e.modifiers, Modifiers::Ctrl) || !m_browser->resizeIcons) return false;
    m_browser->resizeIcons(m_browser->iconSize * std::pow(1.002f, -e.delta.y));
    return true;
}

bool IconGridView::onMagnify(const MagnifyEvent& e) {
    if (m_browser->resizeIcons) m_browser->resizeIcons(m_browser->iconSize * e.scale);
    return true;
}

void IconGridView::computeLayout(float width) {
    const float icon = m_browser->iconSize;
    const float minCell = std::max(icon + 38.0f, 88.0f);
    const float inner = std::max(1.0f, width - kGridPadX * 2);
    m_cols = std::max(1, static_cast<int>(inner / minCell));
    m_cellW = inner / static_cast<float>(m_cols);
    // Spare width spreads the cells apart; labels keep a comfortable measure.
    const float lw = std::min(m_cellW - 6, std::max(icon + 40, 100.0f));
    if (std::abs(lw - m_labelWidth) > 0.5f) {
        m_labelWidth = lw;
        m_labels.clear();
    }
    m_cellH = icon + 12 + kLabelLine * 2 + 14;
}

Size IconGridView::measureContent(Size available) {
    const float w = std::isfinite(available.width) ? available.width : 800.0f;
    computeLayout(w);
    const auto n = m_browser->entries.size();
    const auto rows = static_cast<float>((n + static_cast<size_t>(m_cols) - 1) / static_cast<size_t>(m_cols));
    return {w, kGridPadTop + rows * m_cellH + 16};
}

int IconGridView::indexOf(const std::string& path) const {
    auto it = m_index.find(path);
    return it == m_index.end() ? -1 : it->second;
}

Rect IconGridView::cellRect(int i) const {
    const int col = i % m_cols;
    const int row = i / m_cols;
    return {frame().x + kGridPadX + static_cast<float>(col) * m_cellW, frame().y + kGridPadTop + static_cast<float>(row) * m_cellH,
            m_cellW, m_cellH};
}

Rect IconGridView::iconRect(int i) const {
    const Rect c = cellRect(i);
    const float s = m_browser->iconSize;
    return {std::round(c.center().x - s * 0.5f), c.y + 6, s, s};
}

Rect IconGridView::nameRect(const std::string& path) const {
    const int i = indexOf(path);
    if (i < 0) return {};
    const Rect ic = iconRect(i);
    const Rect c = cellRect(i);
    return {c.center().x - m_labelWidth * 0.5f + 6, ic.bottom() + 6, m_labelWidth - 12, kLabelLine};
}

Rect IconGridView::itemRect(const std::string& path) const {
    const int i = indexOf(path);
    if (i < 0) return {};
    const Rect ic = iconRect(i).outset(5);
    auto it = m_labelExtents.find(path);
    const float w = it != m_labelExtents.end() ? it->second : m_labelWidth;
    const Rect name{cellRect(i).center().x - w * 0.5f, ic.bottom(), w, kLabelLine * 2 + 2};
    return ic.united(name);
}

std::string IconGridView::itemAt(Point p) const {
    if (!frame().contains(p) || m_browser->entries.empty()) return {};
    const int col = static_cast<int>((p.x - frame().x - kGridPadX) / m_cellW);
    const int row = static_cast<int>(std::floor((p.y - frame().y - kGridPadTop) / m_cellH));
    if (col < 0 || col >= m_cols || row < 0) return {};
    const int i = row * m_cols + col;
    if (i >= static_cast<int>(m_browser->entries.size())) return {};
    const std::string& path = m_browser->entries[static_cast<size_t>(i)].path;
    // Only the icon and the label are hit areas, as in the Finder; the rest of the cell starts a rubber band.
    return itemRect(path).contains(p) ? path : std::string();
}

std::vector<const FileEntry*> IconGridView::order() const {
    std::vector<const FileEntry*> out;
    out.reserve(m_browser->entries.size());
    for (const auto& e : m_browser->entries) out.push_back(&e);
    return out;
}

std::string IconGridView::neighbour(const std::string& from, Key key) const {
    const auto& entries = m_browser->entries;
    const int n = static_cast<int>(entries.size());
    if (n == 0) return {};
    const int i = indexOf(from);
    if (i < 0) return entries.front().path;
    int j = i;
    switch (key) {
        case Key::Left: j = std::max(0, i - 1); break;
        case Key::Right: j = std::min(n - 1, i + 1); break;
        case Key::Up: j = i - m_cols >= 0 ? i - m_cols : i; break;
        case Key::Down: j = i + m_cols < n ? i + m_cols : (i / m_cols < (n - 1) / m_cols ? n - 1 : i); break;
        default: return {};
    }
    return entries[static_cast<size_t>(j)].path;
}

const IconGridView::Label& IconGridView::label(Painter& p, const FileEntry& e) {
    auto it = m_labels.find(e.path);
    if (it != m_labels.end()) return it->second;
    const Font f = labelFont();
    const float w = m_labelWidth - 10;
    Label l;
    const std::string& name = e.name;
    if (p.measureText(name, f) <= w) {
        l.lines[0] = name;
        l.count = 1;
    } else {
        // Longest prefix that fits, broken after a separator when there is one in its second half.
        std::vector<size_t> b;
        for (size_t i = 0; i < name.size(); ++i) {
            if ((static_cast<unsigned char>(name[i]) & 0xC0) != 0x80) b.push_back(i);
        }
        b.push_back(name.size());
        size_t lo = 1;
        size_t hi = b.size() - 1;
        while (lo < hi) {
            size_t mid = (lo + hi + 1) / 2;
            if (p.measureText(name.substr(0, b[mid]), f) <= w) lo = mid;
            else hi = mid - 1;
        }
        size_t cut = b[lo];
        for (size_t k = lo; k > lo / 2 && k > 0; --k) {
            const char ch = name[b[k] - 1];
            if (ch == ' ' || ch == '-' || ch == '_' || ch == '.') {
                cut = b[k];
                break;
            }
        }
        std::string first = name.substr(0, cut);
        std::string rest = name.substr(cut);
        while (!first.empty() && first.back() == ' ') first.pop_back();
        while (!rest.empty() && rest.front() == ' ') rest.erase(0, 1);
        l.lines[0] = first;
        l.lines[1] = truncateMiddle(p, rest, f, w);
        l.count = 2;
    }
    for (int i = 0; i < l.count; ++i) l.widths[i] = p.measureText(l.lines[i], f);
    m_labelExtents[e.path] = std::max(l.widths[0], l.widths[1]) + 10;
    return m_labels.emplace(e.path, std::move(l)).first->second;
}

void IconGridView::draw(Painter& p) {
    const Theme& t = theme();
    const auto& c = t.colors;
    computeLayout(frame().width);
    const Rect clip = p.clipBounds();
    const auto& entries = m_browser->entries;
    const int n = static_cast<int>(entries.size());
    const int firstRow = std::max(0, static_cast<int>(std::floor((clip.y - frame().y - kGridPadTop) / m_cellH)));
    const int lastRow = static_cast<int>(std::ceil((clip.bottom() - frame().y - kGridPadTop) / m_cellH));
    const Font f = labelFont();
    const bool emph = emphasized();
    const float scale = p.scale();
    for (int i = firstRow * m_cols; i < std::min(n, (lastRow + 1) * m_cols); ++i) {
        const FileEntry& e = entries[static_cast<size_t>(i)];
        const Rect ic = iconRect(i);
        const bool sel = m_browser->isSelected(e.path);
        const bool drop = m_browser->dropHover == e.path;
        if (sel || drop) {
            Color back = drop ? c.accent.withAlpha(0.22f) : (t.isDark() ? Color::white(0.1f) : Color::black(0.07f));
            p.fillRoundedRect(ic.outset(5), 7, back);
        }
        drawFileIcon(p, ic, e, styleFor(*m_browser, e, m_browser->iconSize * scale));

        const Label& l = label(p, e);
        float y = ic.bottom() + 6;
        const float cx = cellRect(i).center().x;
        for (int k = 0; k < l.count; ++k) {
            Rect pill{std::round(cx - l.widths[k] * 0.5f - 5), y, l.widths[k] + 10, kLabelLine + 1};
            if (sel || drop) {
                const bool joinBelow = k + 1 < l.count && l.widths[k + 1] + 10 >= pill.width - 1;
                const bool joinAbove = k > 0 && l.widths[k - 1] + 10 >= pill.width - 1;
                p.fillRoundedRect(pill, runRadii(joinAbove, joinBelow, 4), drop ? c.accent : selectionColor());
            }
            const Color text = (sel && emph) || drop ? c.onAccent : (e.isHidden ? c.secondaryLabel : c.label);
            p.drawText(l.lines[k], pill, f, text, TextAlign::Center);
            y += kLabelLine;
        }
    }
    paintChildren(p);
}

// --- List ------------------------------------------------------------------------------------------

std::array<ListColumns::Span, 4> ListColumns::layout(float width) const {
    const float fixed = modified + size + kind;
    const float name = std::max(180.0f, width - fixed - 12);
    return {Span{0, name}, Span{name, modified}, Span{name + modified, size}, Span{name + modified + size, kind}};
}

namespace {
constexpr const char* kColumnTitles[4] = {"名称", "修改时间", "大小", "类型"};
constexpr SortKey kColumnKeys[4] = {SortKey::Name, SortKey::Modified, SortKey::Size, SortKey::Kind};
}  // namespace

int ListHeaderView::columnAt(float x) const {
    auto cols = m_columns->layout(frame().width);
    for (int i = 0; i < 4; ++i) {
        float cx = frame().x + cols[static_cast<size_t>(i)].x;
        if (x >= cx && x < cx + cols[static_cast<size_t>(i)].width) return i;
    }
    return -1;
}

int ListHeaderView::edgeAt(float x) const {
    auto cols = m_columns->layout(frame().width);
    for (int i = 1; i < 4; ++i) {
        if (std::abs(x - (frame().x + cols[static_cast<size_t>(i)].x)) <= 4) return i;
    }
    return -1;
}

void ListHeaderView::draw(Painter& p) {
    const Theme& t = theme();
    const auto& c = t.colors;
    const Rect r = frame();
    const float px = 1.0f / p.scale();
    p.fillRect({r.x, p.snap(r.bottom() - px), r.width, px}, c.separator);
    const Font f = t.font(TextStyle::Subheadline).withWeight(FontWeight::Medium);
    auto cols = m_columns->layout(r.width);
    for (int i = 0; i < 4; ++i) {
        const auto& span = cols[static_cast<size_t>(i)];
        Rect cell{r.x + span.x, r.y, span.width, r.height - px};
        if (i == m_hover && m_resizing < 0) p.fillRoundedRect(cell.inset(2, 3), 5, c.hover);
        if (i > 0) p.fillRect({p.snap(cell.x), cell.y + 7, px, cell.height - 14}, c.separator);
        const bool active = m_browser->sort.key == kColumnKeys[i];
        const bool numeric = i == 2;
        Rect text = cell.inset(Insets(0, i == 0 ? 40 : 10, 0, active ? 24 : 10));
        p.pushClip(cell);
        const char* title = i == 3 && m_browser->searching ? "位置" : kColumnTitles[i];
        p.drawText(title, text, f, active ? c.label : c.secondaryLabel,
                   numeric ? TextAlign::Trailing : TextAlign::Leading);
        if (active) {
            Icon arrow = m_browser->sort.ascending ? Icon::ChevronUp : Icon::ChevronDown;
            p.drawIcon(arrow, {cell.right() - 19, cell.center().y - 4.5f, 9, 9}, c.secondaryLabel, 1.4f);
        }
        p.popClip();
    }
}

bool ListHeaderView::onPointerDown(const PointerEvent& e) {
    if (e.button != MouseButton::Left) return false;
    m_resizing = edgeAt(e.position.x);
    if (m_resizing >= 0) {
        m_resizeX = e.position.x;
        float* w = m_resizing == 1 ? &m_columns->modified : m_resizing == 2 ? &m_columns->size : &m_columns->kind;
        m_resizeWidth = *w;
        return true;
    }
    m_pressed = columnAt(e.position.x);
    return m_pressed >= 0;
}

void ListHeaderView::onPointerMove(const PointerEvent& e) {
    if (m_resizing >= 0) {
        // Each edge resizes the column to its right; the name column absorbs the difference.
        float* w = m_resizing == 1 ? &m_columns->modified : m_resizing == 2 ? &m_columns->size : &m_columns->kind;
        *w = std::clamp(m_resizeWidth - (e.position.x - m_resizeX), 50.0f, 400.0f);
        invalidate();
        if (onColumnsResized) onColumnsResized();
        return;
    }
    setCursor(edgeAt(e.position.x) >= 0 ? std::optional(CursorShape::ResizeHorizontal) : std::nullopt);
    int h = columnAt(e.position.x);
    if (h != m_hover) {
        m_hover = h;
        invalidate();
    }
}

void ListHeaderView::onPointerUp(const PointerEvent& e) {
    if (m_resizing >= 0) {
        m_resizing = -1;
        return;
    }
    const int col = columnAt(e.position.x);
    if (col >= 0 && col == m_pressed) {
        SortOrder& s = m_browser->sort;
        if (s.key == kColumnKeys[col]) {
            s.ascending = !s.ascending;
        } else {
            s.key = kColumnKeys[col];
            s.ascending = col == 0 || col == 3;  // dates and sizes start with the newest / largest
        }
        if (m_browser->sortChanged) m_browser->sortChanged();
        invalidate();
    }
    m_pressed = -1;
}

void ListHeaderView::onPointerLeave() {
    m_hover = -1;
    setCursor(std::nullopt);
    invalidate();
}

void ListRowsView::reload() {
    // Forget expanded folders that are gone.
    std::erase_if(m_expanded, [](const std::string& p) {
        std::error_code ec;
        return !std::filesystem::is_directory(p, ec);
    });
    rebuild();
}

void ListRowsView::rebuild() {
    m_rows.clear();
    m_index.clear();
    std::function<void(const std::vector<FileEntry>&, int)> add = [&](const std::vector<FileEntry>& list, int depth) {
        for (const auto& e : list) {
            m_index[e.path] = static_cast<int>(m_rows.size());
            m_rows.push_back({e, depth});
            if (e.isDir && depth < 12 && m_expanded.count(e.path) && m_browser->listing) {
                add(m_browser->listing(e.path), depth + 1);
            }
        }
    };
    add(m_browser->entries, 0);
    invalidateLayout();
}

void ListRowsView::toggle(const std::string& path) {
    if (m_expanded.count(path)) {
        std::erase_if(m_expanded, [&](const std::string& p) { return isInside(p, path); });
        // Selected descendants disappear with the rows.
        bool changed = false;
        for (auto it = m_browser->selection.begin(); it != m_browser->selection.end();) {
            if (*it != path && isInside(*it, path)) {
                it = m_browser->selection.erase(it);
                changed = true;
            } else {
                ++it;
            }
        }
        if (changed) m_browser->notifySelection();
    } else {
        m_expanded.insert(path);
    }
    rebuild();
}

Size ListRowsView::measureContent(Size available) {
    const float w = std::isfinite(available.width) ? available.width : 700.0f;
    return {w, static_cast<float>(m_rows.size()) * kRowHeight + 12};
}

int ListRowsView::rowIndex(const std::string& path) const {
    auto it = m_index.find(path);
    return it == m_index.end() ? -1 : it->second;
}

Rect ListRowsView::rowRect(int i) const {
    return {frame().x, frame().y + 4 + static_cast<float>(i) * kRowHeight, frame().width, kRowHeight};
}

Rect ListRowsView::disclosureRect(int i) const {
    const Rect row = rowRect(i);
    const float x = row.x + 12 + static_cast<float>(m_rows[static_cast<size_t>(i)].depth) * 16;
    return {x - 3, row.center().y - 8, 16, 16};
}

Rect ListRowsView::itemRect(const std::string& path) const {
    const int i = rowIndex(path);
    return i < 0 ? Rect{} : rowRect(i).inset(8, 0);
}

Rect ListRowsView::nameRect(const std::string& path) const {
    const int i = rowIndex(path);
    if (i < 0) return {};
    const Rect row = rowRect(i);
    const auto cols = m_columns->layout(row.width);
    const float x = row.x + 12 + static_cast<float>(m_rows[static_cast<size_t>(i)].depth) * 16 + 36;
    return {x, row.y, std::max(60.0f, row.x + cols[0].width - x - 10), row.height};
}

std::string ListRowsView::itemAt(Point p) const {
    if (!frame().contains(p)) return {};
    const int i = static_cast<int>(std::floor((p.y - frame().y - 4) / kRowHeight));
    if (i < 0 || i >= static_cast<int>(m_rows.size())) return {};
    return m_rows[static_cast<size_t>(i)].entry.path;
}

const FileEntry* ListRowsView::entry(const std::string& path) const {
    const int i = rowIndex(path);
    return i < 0 ? nullptr : &m_rows[static_cast<size_t>(i)].entry;
}

std::vector<const FileEntry*> ListRowsView::order() const {
    std::vector<const FileEntry*> out;
    out.reserve(m_rows.size());
    for (const auto& r : m_rows) out.push_back(&r.entry);
    return out;
}

bool ListRowsView::onPointerDown(const PointerEvent& e) {
    if (e.button == MouseButton::Left) {
        std::string item = itemAt(e.position);
        const int i = rowIndex(item);
        if (i >= 0 && m_rows[static_cast<size_t>(i)].entry.isDir && disclosureRect(i).contains(e.position)) {
            if (auto* h = host()) h->setFocus(this, false);
            toggle(item);
            return true;
        }
    }
    return FileView::onPointerDown(e);
}

bool ListRowsView::handleKey(const KeyEvent& e) {
    if (e.primary() || e.alt() || e.shift()) return false;
    const std::string& lead = m_browser->lead;
    const int i = rowIndex(lead);
    if (i < 0 || !m_browser->isSelected(lead)) return false;
    const Row& row = m_rows[static_cast<size_t>(i)];
    if (e.key == Key::Right) {
        if (row.entry.isDir && !m_expanded.count(lead)) toggle(lead);
        return true;
    }
    if (e.key == Key::Left) {
        if (row.entry.isDir && m_expanded.count(lead)) {
            toggle(lead);
        } else if (row.depth > 0) {
            std::string parent = parentPath(lead);
            m_browser->select(parent);
            reveal(parent);
        }
        invalidate();
        return true;
    }
    return false;
}

void ListRowsView::draw(Painter& p) {
    const Theme& t = theme();
    const auto& c = t.colors;
    const Rect r = frame();
    const Rect clip = p.clipBounds();
    const bool emph = emphasized();
    const Font font = t.font(TextStyle::Body);
    const auto cols = m_columns->layout(r.width);
    const int n = static_cast<int>(m_rows.size());
    const int first = std::max(0, static_cast<int>((clip.y - r.y - 4) / kRowHeight) - 1);
    const int last = static_cast<int>((clip.bottom() - r.y) / kRowHeight) + 1;

    // Alternating stripes continue past the last row, like macOS tables.
    for (int i = first; i <= last; ++i) {
        if (i % 2 == 1) p.fillRoundedRect(rowRect(i).inset(8, 0), 6, c.tertiaryFill.multipliedAlpha(0.8f));
    }
    for (int i = first; i < std::min(n, last + 1); ++i) {
        const Row& row = m_rows[static_cast<size_t>(i)];
        const FileEntry& e = row.entry;
        const Rect rr = rowRect(i);
        const bool sel = m_browser->isSelected(e.path);
        const bool drop = m_browser->dropHover == e.path;
        if (sel) {
            const bool above = i > 0 && m_browser->isSelected(m_rows[static_cast<size_t>(i - 1)].entry.path);
            const bool below = i + 1 < n && m_browser->isSelected(m_rows[static_cast<size_t>(i + 1)].entry.path);
            p.fillRoundedRect(rr.inset(8, 0), runRadii(above, below, 6), selectionColor());
        }
        if (drop) p.fillRoundedRect(rr.inset(8, 0), 6, c.accent.withAlpha(0.18f), c.accent, 2);
        const bool onAccent = sel && emph;
        const Color primary = onAccent ? c.onAccent : (e.isHidden ? c.secondaryLabel : c.label);
        const Color secondary = onAccent ? c.onAccent.withAlpha(0.85f) : c.secondaryLabel;

        float x = rr.x + 12 + static_cast<float>(row.depth) * 16;
        if (e.isDir) {
            const bool open = m_expanded.count(e.path) > 0;
            p.drawIcon(open ? Icon::ChevronDown : Icon::ChevronRight, {x, rr.center().y - 5, 10, 10},
                       onAccent ? c.onAccent : c.secondaryLabel, 1.5f);
        }
        x += 16;
        drawRowIcon(p, *m_browser, {x, rr.center().y - 8, 16, 16}, e);
        x += 22;
        const float nameRight = rr.x + cols[0].width - 8;
        p.drawText(truncateMiddle(p, e.name, font, nameRight - x), {x, rr.y, nameRight - x, rr.height}, font, primary);

        auto cell = [&](int k) {
            const auto& s = cols[static_cast<size_t>(k)];
            return Rect{rr.x + s.x + 10, rr.y, s.width - 20, rr.height};
        };
        Rect mod = cell(1);
        p.drawText(truncateEnd(p, formatDate(e.modified), font, mod.width), mod, font, secondary);
        Rect size = cell(2);
        p.drawText(e.isDir ? "--" : formatSize(e.size), size, font, secondary, TextAlign::Trailing);
        Rect kind = cell(3);
        // Search results come from many folders: show where each one is instead of its kind.
        const std::string trailing = m_browser->searching && row.depth == 0 ? displayName(parentPath(e.path)) : kindDescription(e);
        p.drawText(truncateEnd(p, trailing, font, kind.width), kind, font, secondary);
    }
    paintChildren(p);
}

// --- Columns ---------------------------------------------------------------------------------------

void ColumnListView::setEntries(std::vector<FileEntry> entries, std::string chainChild) {
    m_entries = std::move(entries);
    m_chainChild = std::move(chainChild);
    invalidateLayout();
}

bool ColumnListView::isActive() const { return m_folder == m_browser->path; }

Size ColumnListView::measureContent(Size available) {
    const float w = std::isfinite(available.width) ? available.width : ColumnBrowserView::kColumnWidth;
    return {w, static_cast<float>(m_entries.size()) * kRowHeight + 10};
}

int ColumnListView::indexOf(const std::string& path) const {
    for (size_t i = 0; i < m_entries.size(); ++i) {
        if (m_entries[i].path == path) return static_cast<int>(i);
    }
    return -1;
}

Rect ColumnListView::itemRect(const std::string& path) const {
    const int i = indexOf(path);
    if (i < 0) return {};
    return {frame().x + 6, frame().y + 5 + static_cast<float>(i) * kRowHeight, frame().width - 12, kRowHeight};
}

Rect ColumnListView::nameRect(const std::string& path) const {
    Rect r = itemRect(path);
    return {r.x + 30, r.y, r.width - 50, r.height};
}

std::string ColumnListView::itemAt(Point p) const {
    if (!frame().contains(p)) return {};
    const int i = static_cast<int>(std::floor((p.y - frame().y - 5) / kRowHeight));
    if (i < 0 || i >= static_cast<int>(m_entries.size())) return {};
    return m_entries[static_cast<size_t>(i)].path;
}

const FileEntry* ColumnListView::entry(const std::string& path) const {
    const int i = indexOf(path);
    return i < 0 ? nullptr : &m_entries[static_cast<size_t>(i)];
}

std::vector<const FileEntry*> ColumnListView::order() const {
    std::vector<const FileEntry*> out;
    for (const auto& e : m_entries) out.push_back(&e);
    return out;
}

void ColumnListView::activate(const std::string& path) {
    if (m_browser->navigate) m_browser->navigate(m_folder, path);
}

bool ColumnListView::handleKey(const KeyEvent& e) {
    if (!isActive() || e.primary() || e.alt()) return false;
    Browser& b = *m_browser;
    if (e.key == Key::Right) {
        const FileEntry* lead = entry(b.lead);
        if (lead && lead->isDir && b.selection.size() == 1 && b.navigate) {
            const auto& children = b.listing(lead->path);
            b.navigate(lead->path, children.empty() ? std::string() : children.front().path);
        }
        return true;
    }
    if (e.key == Key::Left) {
        if (parentPath(m_folder) != m_folder && b.navigate) b.navigate(parentPath(m_folder), m_folder);
        return true;
    }
    return false;
}

void ColumnListView::draw(Painter& p) {
    const Theme& t = theme();
    const auto& c = t.colors;
    const Rect clip = p.clipBounds();
    const bool active = isActive();
    const bool emph = active && emphasized();
    const Font font = t.font(TextStyle::Body);
    const int n = static_cast<int>(m_entries.size());
    auto selected = [&](int i) {
        const std::string& path = m_entries[static_cast<size_t>(i)].path;
        return active ? m_browser->isSelected(path) : path == m_chainChild;
    };
    for (int i = 0; i < n; ++i) {
        const FileEntry& e = m_entries[static_cast<size_t>(i)];
        const Rect row = itemRect(e.path);
        if (!row.intersects(clip)) continue;
        const bool sel = selected(i);
        if (sel) {
            const bool above = i > 0 && selected(i - 1);
            const bool below = i + 1 < n && selected(i + 1);
            p.fillRoundedRect(row, runRadii(above, below, 6), active ? selectionColor() : unfocusedSelection());
        }
        if (m_browser->dropHover == e.path) p.fillRoundedRect(row, 6, c.accent.withAlpha(0.18f), c.accent, 2);
        const bool onAccent = sel && emph;
        const Color text = onAccent ? c.onAccent : (e.isHidden ? c.secondaryLabel : c.label);
        drawRowIcon(p, *m_browser, {row.x + 8, row.center().y - 8, 16, 16}, e);
        const float right = row.right() - (e.isDir ? 24 : 8);
        p.drawText(truncateMiddle(p, e.name, font, right - row.x - 32), {row.x + 32, row.y, right - row.x - 32, row.height},
                   font, text);
        if (e.isDir) {
            p.drawIcon(Icon::ChevronRight, {row.right() - 18, row.center().y - 5, 10, 10},
                       onAccent ? c.onAccent : c.secondaryLabel, 1.5f);
        }
    }
    if (m_entries.empty()) {
        p.drawText("清空", {frame().x, frame().y + 10, frame().width, 20}, t.font(TextStyle::Callout), c.tertiaryLabel,
                   TextAlign::Center);
    }
    paintChildren(p);
}

void PreviewPaneView::setEntry(const FileEntry& e) {
    const bool changed = e.path != m_entry.path || e.modified != m_entry.modified;
    m_entry = e;
    if (changed) {
        m_large.reset();
        m_largeFor.clear();
    }
    if (changed && isImageExtension(e.ext) && m_browser->previews) {
        m_largeFor = e.path;
        std::weak_ptr<Widget> weak = weak_from_this();
        m_browser->previews->loadLarge(e.path, 1024, [this, weak, path = e.path](std::shared_ptr<ImageData> img) {
            auto keep = weak.lock();
            if (!keep || m_largeFor != path) return;
            m_large = std::move(img);
            invalidate();
        });
    }
    invalidate();
}

void PreviewPaneView::draw(Painter& p) {
    if (m_entry.path.empty()) return;
    const Theme& t = theme();
    const auto& c = t.colors;
    const Rect r = frame().inset(Insets(18, 22, 18, 22));
    const float box = std::min(r.width, 200.0f);
    const Rect art{r.center().x - box * 0.5f, r.y + 8, box, box};
    if (m_large && m_large->width() > 0) {
        const float iw = static_cast<float>(m_large->width());
        const float ih = static_cast<float>(m_large->height());
        const float k = std::min(art.width / iw, art.height / ih);
        Rect pic{art.center().x - iw * k * 0.5f, art.center().y - ih * k * 0.5f, iw * k, ih * k};
        p.drawShadow(pic, 6, Color::black(t.isDark() ? 0.5f : 0.2f), 14, {0, 4});
        p.drawImage(pic, *m_large, 6);
    } else {
        drawFileIcon(p, art.inset(box * 0.12f), m_entry, styleFor(*m_browser, m_entry, box * p.scale()));
    }
    float y = art.bottom() + 16;
    const Font title = t.font(TextStyle::Headline);
    p.drawText(truncateMiddle(p, m_entry.name, title, r.width), {r.x, y, r.width, 18}, title, c.label, TextAlign::Center);
    y += 20;
    const Font small = t.font(TextStyle::Subheadline);
    std::string sub = kindDescription(m_entry) + (m_entry.isDir ? "" : " – " + formatSize(m_entry.size));
    p.drawText(truncateEnd(p, sub, small, r.width), {r.x, y, r.width, 16}, small, c.secondaryLabel, TextAlign::Center);
    y += 34;

    p.drawText("信息", {r.x, y, r.width, 16}, t.font(TextStyle::Subheadline).withWeight(FontWeight::Semibold),
               c.label);
    y += 22;
    std::vector<std::pair<std::string, std::string>> rows;
    if (m_entry.created > 0) rows.emplace_back("创建时间", formatDate(m_entry.created));
    rows.emplace_back("修改时间", formatDate(m_entry.modified));
    if (m_large) {
        rows.emplace_back("尺寸",
                          std::to_string(m_large->originalWidth()) + " × " + std::to_string(m_large->originalHeight()));
    }
    rows.emplace_back("权限", formatPermissions(m_entry.mode, m_entry.isDir));
    const float px = 1.0f / p.scale();
    for (const auto& [label, value] : rows) {
        Rect line{r.x, y, r.width, 22};
        p.drawText(label, line, small, c.secondaryLabel);
        p.drawText(truncateEnd(p, value, small, r.width * 0.65f), line, small, c.label, TextAlign::Trailing);
        y += 24;
        p.fillRect({r.x, p.snap(y - 2), r.width, px}, c.separator.multipliedAlpha(0.6f));
    }
}

ColumnBrowserView::Column ColumnBrowserView::columnFor(const std::string& folder) {
    auto it = m_cache.find(folder);
    if (it != m_cache.end()) return it->second;
    Column col;
    col.list = std::make_shared<ColumnListView>(m_browser, folder);
    col.scroll = std::make_shared<ScrollViewView>(col.list);
    col.scroll->setScrollEdge(ScrollEdgeStyle::Soft, theme().metrics.spacingLarge);
    col.list->setScroller(col.scroll.get());
    m_cache[folder] = col;
    return col;
}

void ColumnBrowserView::rebuild(bool focusActive) {
    const Browser& b = *m_browser;
    if (b.path.empty()) return;  // nothing to show before the first navigation
    if (!m_preview) m_preview = std::make_shared<PreviewPaneView>(m_browser);
    std::vector<std::string> chain;
    if (b.searching || b.path == kRecents) {
        chain = {b.path};
    } else {
        std::string root = "/";
        for (const auto& candidate : roots) {
            if (isInside(b.path, candidate) && candidate.size() > root.size()) root = candidate;
        }
        for (std::string cur = b.path;; cur = parentPath(cur)) {
            chain.push_back(cur);
            if (cur == root || parentPath(cur) == cur) break;
        }
        std::reverse(chain.begin(), chain.end());
    }

    std::vector<Column> columns;
    for (size_t i = 0; i < chain.size(); ++i) {
        Column col = columnFor(chain[i]);
        if (chain[i] == b.path) col.list->setEntries(b.entries, {});
        else col.list->setEntries(b.listing(chain[i]), i + 1 < chain.size() ? chain[i + 1] : std::string());
        columns.push_back(col);
    }
    m_showPreview = false;
    if (b.selection.size() == 1) {
        const FileEntry* sel = b.find(*b.selection.begin());
        if (sel && sel->isDir) {
            Column col = columnFor(sel->path);
            col.list->setEntries(b.listing(sel->path), {});
            columns.push_back(col);
        } else if (sel) {
            m_preview->setEntry(*sel);
            m_showPreview = true;
        }
    }

    // Swap children only when the set of columns changed, so focus and pointer capture stay put.
    std::vector<std::shared_ptr<Widget>> wanted;
    for (const auto& col : columns) wanted.push_back(col.scroll);
    if (m_showPreview) wanted.push_back(m_preview);
    if (wanted != children()) {
        clearChildren();
        for (const auto& w : wanted) addChild(w);
    }
    m_columns = std::move(columns);
    std::erase_if(m_cache, [&](const auto& kv) {
        return std::none_of(m_columns.begin(), m_columns.end(), [&](const Column& c) { return c.list == kv.second.list; });
    });
    invalidateLayout();
    if (focusActive) {
        if (auto* a = activeColumn(); a && host()) host()->setFocus(a, false);
    }
    // Bring the newest column into view once laid out.
    if (m_scroller) {
        App::current().post([weak = weak_from_this(), this] {
            if (!weak.lock() || !m_scroller) return;
            m_scroller->scrollTo({1e6f, 0}, true);
        });
    }
}

ColumnListView* ColumnBrowserView::activeColumn() const {
    for (const auto& col : m_columns) {
        if (col.list->isActive()) return col.list.get();
    }
    return nullptr;
}

void ColumnBrowserView::beginRename(const std::string& path) {
    if (auto* a = activeColumn()) a->beginRename(path);
}

Size ColumnBrowserView::measureContent(Size available) {
    const float w = static_cast<float>(m_columns.size()) * kColumnWidth + (m_showPreview ? kPreviewWidth : 0.0f);
    return {w, std::isfinite(available.height) ? available.height : 400.0f};
}

void ColumnBrowserView::arrangeContent(const Rect& content) {
    float x = content.x;
    for (const auto& col : m_columns) {
        col.scroll->measure({kColumnWidth, content.height});
        col.scroll->arrange({x, content.y, kColumnWidth, content.height});
        x += kColumnWidth;
    }
    if (m_showPreview) {
        const float w = std::max(kPreviewWidth, content.right() - x);
        m_preview->measure({w, content.height});
        m_preview->arrange({x, content.y, w, content.height});
    }
}

void ColumnBrowserView::draw(Painter& p) {
    paintChildren(p);
    const auto& c = theme().colors;
    const float px = 1.0f / p.scale();
    const Rect r = frame();
    for (size_t i = 1; i <= m_columns.size(); ++i) {
        if (i == m_columns.size() && !m_showPreview) break;
        const float x = p.snap(r.x + static_cast<float>(i) * kColumnWidth);
        p.fillRect({x - px, r.y, px, r.height}, c.separator);
    }
}

// --- Path bar ----------------------------------------------------------------------------------------

std::vector<PathBarView::Segment> PathBarView::segments(Painter* p) const {
    std::vector<Segment> out;
    const Browser& b = *m_browser;
    if (b.path == kRecents) {
        out.push_back({kRecents, "最近使用", {}, true});
    } else {
        const std::string home = homeDirectory();
        const std::string root = isInside(b.path, home) ? home : "/";
        std::vector<std::string> chain;
        for (std::string cur = b.path;; cur = parentPath(cur)) {
            chain.push_back(cur);
            if (cur == root || parentPath(cur) == cur) break;
        }
        std::reverse(chain.begin(), chain.end());
        for (const auto& c : chain) out.push_back({c, displayName(c), {}, true});
    }
    // Like the Finder, the path continues to the selected item.
    if (b.selection.size() == 1) {
        if (const FileEntry* e = b.find(*b.selection.begin()); e && !b.searching) out.push_back({e->path, e->name, {}, e->isDir});
    }
    if (!p) return out;

    const Font f = theme().font(TextStyle::Subheadline);
    const float icon = 14;
    const float chevron = 16;
    const Rect r = frame();
    std::vector<float> widths;
    float total = 0;
    for (const auto& s : out) {
        widths.push_back(p->measureText(s.title, f));
        total += icon + 4 + widths.back() + chevron + 8;
    }
    // Squeeze the middle segments first, then everything but the last.
    const float avail = r.width - 24;
    for (float cap : {90.0f, 50.0f, 0.0f}) {
        if (total <= avail) break;
        for (size_t i = 1; i + 1 < widths.size() && total > avail; ++i) {
            if (widths[i] > cap) {
                total -= widths[i] - cap;
                widths[i] = cap;
            }
        }
    }
    float x = r.x + 12;
    for (size_t i = 0; i < out.size(); ++i) {
        out[i].rect = {x - 4, r.y + 4, icon + 4 + widths[i] + 8, r.height - 8};
        if (widths[i] <= 0) out[i].title.clear();
        else out[i].title = truncateEnd(*p, out[i].title, f, widths[i] + 0.5f);
        x += icon + 4 + widths[i] + chevron + 8;
    }
    return out;
}

void PathBarView::draw(Painter& p) {
    const Theme& t = theme();
    const auto& c = t.colors;
    m_laidOut = segments(&p);
    const Font f = t.font(TextStyle::Subheadline);
    for (size_t i = 0; i < m_laidOut.size(); ++i) {
        const Segment& s = m_laidOut[i];
        const bool last = i + 1 == m_laidOut.size();
        if (static_cast<int>(i) == m_hover) p.fillRoundedRect(s.rect, 5, c.hover);
        if (!s.path.empty() && m_browser->dropHover == s.path) p.fillRoundedRect(s.rect, 5, c.accent.withAlpha(0.25f));
        Rect icon{s.rect.x + 4, s.rect.center().y - 7, 14, 14};
        if (s.path == kRecents || s.path == "/" || s.path == homeDirectory() || s.path == trashFilesDirectory()) {
            p.drawIcon(placeIcon(s.path), icon, c.secondaryLabel);
        } else if (s.isDir) {
            drawFolderIcon(p, icon);
        } else if (const FileEntry* e = m_browser->find(s.path)) {
            drawFileIcon(p, icon, *e);
        }
        p.drawText(s.title, {icon.right() + 4, s.rect.y, s.rect.right() - icon.right() - 4, s.rect.height}, f,
                   last ? c.label : c.secondaryLabel);
        if (!last) {
            p.drawIcon(Icon::ChevronRight, {s.rect.right() + 4, s.rect.center().y - 4, 8, 8}, c.tertiaryLabel, 1.6f);
        }
    }
}

int PathBarView::segmentAt(Point p) const {
    for (size_t i = 0; i < m_laidOut.size(); ++i) {
        if (m_laidOut[i].rect.contains(p)) return static_cast<int>(i);
    }
    return -1;
}

bool PathBarView::onPointerDown(const PointerEvent& e) {
    const int i = segmentAt(e.position);
    if (i < 0 || e.button != MouseButton::Left) return false;
    const Segment s = m_laidOut[static_cast<size_t>(i)];
    if (!m_browser->navigate) return true;
    if (s.isDir) m_browser->navigate(s.path, {});
    else m_browser->navigate(parentPath(s.path), s.path);
    return true;
}

void PathBarView::onPointerMove(const PointerEvent& e) {
    const int i = segmentAt(e.position);
    if (i != m_hover) {
        m_hover = i;
        invalidate();
    }
}

void PathBarView::onPointerLeave() {
    m_hover = -1;
    invalidate();
}

std::string PathBarView::dropFolderAt(Point p) {
    const int i = segmentAt(p);
    if (i < 0) return {};
    const Segment& s = m_laidOut[static_cast<size_t>(i)];
    return s.isDir && s.path != kRecents ? s.path : std::string();
}

}  // namespace finder
