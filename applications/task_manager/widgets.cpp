#include "mayonnaise/ui_translation.hpp"
#include "widgets.hpp"

#include <algorithm>
#include <cmath>

using namespace mayo;

namespace {

/// Shortens `text` with a trailing ellipsis so it fits in `width` points.
std::string fitText(Painter& p, const std::string& text, const Font& font, float width) {
    if (p.measureText(text, font) <= width) return text;
    const std::string ellipsis = "…";
    size_t lo = 0;
    size_t hi = text.size();
    while (lo < hi) {  // longest prefix that fits, on UTF-8 boundaries
        size_t mid = (lo + hi + 1) / 2;
        while (mid > 0 && mid < text.size() && (static_cast<unsigned char>(text[mid]) & 0xC0) == 0x80) --mid;
        if (mid <= lo) break;
        if (p.measureText(text.substr(0, mid) + ellipsis, font) <= width) lo = mid;
        else hi = mid - 1;
    }
    return text.substr(0, lo) + ellipsis;
}

double niceCeiling(double v) {
    if (v <= 0) return 1;
    double p = std::pow(10.0, std::floor(std::log10(v)));
    for (double m : {1.0, 2.0, 2.5, 5.0, 10.0}) {
        if (v <= m * p) return m * p;
    }
    return 10 * p;
}

/// Opaque mix so adjacent pixel columns never show seams or double-blended edges.
Color solid(const Color& over, const Color& base, float amount) {
    Color o = over;
    Color b = base;
    return {b.r + (o.r - b.r) * amount, b.g + (o.g - b.g) * amount, b.b + (o.b - b.b) * amount, 1.0f};
}

}  // namespace

// --- HistoryGraph ------------------------------------------------------------------------------

HistoryGraph::HistoryGraph(Mode mode, int capacity) : m_mode(mode), m_capacity(capacity) {}

void HistoryGraph::push(std::vector<double> values) {
    m_samples.push_back(std::move(values));
    while (static_cast<int>(m_samples.size()) > m_capacity) m_samples.pop_front();
    invalidate();
}

Size HistoryGraph::measureContent(Size available) {
    return {std::isfinite(available.width) ? available.width : 240.0f,
            std::isfinite(available.height) ? std::max(70.0f, available.height) : 110.0f};
}

double HistoryGraph::scaleMax() const {
    if (m_fixedMax > 0) return m_fixedMax;
    double peak = 0;
    for (const auto& s : m_samples) {
        if (m_mode == Mode::Stacked) {
            double sum = 0;
            for (double v : s) sum += v;
            peak = std::max(peak, sum);
        } else {
            for (double v : s) peak = std::max(peak, v);
        }
    }
    return niceCeiling(std::max(peak * 1.1, m_minMax));
}

void HistoryGraph::draw(Painter& p) {
    const Theme& t = theme();
    const auto& c = t.colors;
    const Rect r = frame();
    const float radius = 10;
    const Color bg = t.isDark() ? Color::hex(0x1E1E20) : Color::white();
    p.fillRoundedRect(r, radius, bg, c.separator, 0.5f);
    p.pushClip(r.inset(0.5f), radius);
    const Rect in = r.inset(1);

    // Grid.
    const float px = 1.0f / p.scale();
    for (int i = 1; i < 4; ++i) {
        float y = p.snap(in.y + in.height * static_cast<float>(i) / 4.0f);
        Color line = (m_mode == Mode::Mirrored && i == 2) ? c.separator : c.separator.multipliedAlpha(0.45f);
        p.fillRect({in.x, y, in.width, px}, line);
    }

    const std::vector<Color> colors = m_colorProvider ? m_colorProvider() : m_colors;
    const int n = static_cast<int>(m_samples.size());
    const double max = scaleMax();
    if (n >= 2 && !colors.empty()) {
        const int series = static_cast<int>(m_samples.back().size());
        const float step = in.width / static_cast<float>(m_capacity - 1);
        const float x0 = in.right() - step * static_cast<float>(n - 1);  // newest sample at the right edge
        auto valueAt = [&](float x, int s) {
            float f = (x - x0) / step;
            int i = std::clamp(static_cast<int>(std::floor(f)), 0, n - 2);
            float frac = std::clamp(f - static_cast<float>(i), 0.0f, 1.0f);
            double a = s < static_cast<int>(m_samples[static_cast<size_t>(i)].size()) ? m_samples[static_cast<size_t>(i)][static_cast<size_t>(s)] : 0;
            double b = s < static_cast<int>(m_samples[static_cast<size_t>(i + 1)].size()) ? m_samples[static_cast<size_t>(i + 1)][static_cast<size_t>(s)] : 0;
            return a + (b - a) * frac;
        };
        const float mid = in.center().y;
        // Filled areas, one device pixel wide column at a time.
        for (float x = std::max(in.x, x0); x < in.right(); x += px) {
            if (m_mode == Mode::Stacked) {
                double base = 0;
                for (int s = 0; s < series; ++s) {
                    double v = valueAt(x + px * 0.5f, s);
                    float y0 = in.bottom() - static_cast<float>(base / max) * in.height;
                    float y1 = in.bottom() - static_cast<float>(std::min(max, base + v) / max) * in.height;
                    base += v;
                    if (y0 - y1 > 0.01f) p.fillRect({x, y1, px, y0 - y1}, solid(colors[static_cast<size_t>(s) % colors.size()], bg, 0.45f));
                }
            } else {
                for (int s = 0; s < std::min(series, 2); ++s) {
                    float h = static_cast<float>(std::min(1.0, valueAt(x + px * 0.5f, s) / max)) * in.height * 0.5f;
                    if (h < 0.01f) continue;
                    Rect col = s == 0 ? Rect{x, mid - h, px, h} : Rect{x, mid, px, h};
                    p.fillRect(col, solid(colors[static_cast<size_t>(s) % colors.size()], bg, 0.45f));
                }
            }
        }
        // Crisp line along the top of each series.
        for (int s = 0; s < series; ++s) {
            const Color col = colors[static_cast<size_t>(s) % colors.size()];
            Point prev{};
            for (int i = 0; i < n; ++i) {
                double v = 0;
                if (m_mode == Mode::Stacked) {
                    for (int k = 0; k <= s; ++k) v += m_samples[static_cast<size_t>(i)][static_cast<size_t>(k)];
                } else {
                    v = m_samples[static_cast<size_t>(i)][static_cast<size_t>(s)];
                }
                float x = x0 + step * static_cast<float>(i);
                float y = m_mode == Mode::Stacked ? in.bottom() - static_cast<float>(std::min(1.0, v / max)) * in.height
                          : s == 0             ? mid - static_cast<float>(std::min(1.0, v / max)) * in.height * 0.5f
                                               : mid + static_cast<float>(std::min(1.0, v / max)) * in.height * 0.5f;
                Point pt{x, y};
                if (i > 0) p.drawLine(prev, pt, col, 1.3f);
                prev = pt;
            }
            if (m_mode == Mode::Mirrored && s >= 1) break;
        }
    }
    if (m_format) {
        Font f = t.font(TextStyle::Caption);
        p.drawText(m_format(max), {in.x + 7, in.y + 3, in.width, 14}, f, c.tertiaryLabel);
    }
    p.popClip();
}

// --- CoreBars / UsageBar -----------------------------------------------------------------------------

Size CoreBars::measureContent(Size available) {
    const float n = static_cast<float>(std::max<size_t>(1, m_cores.size()));
    return {std::max(n * 9.0f, 60.0f), std::isfinite(available.height) ? available.height : 90.0f};
}

void CoreBars::draw(Painter& p) {
    const Theme& t = theme();
    const Rect r = frame();
    const auto n = static_cast<float>(m_cores.size());
    if (n <= 0) return;
    const float gap = 3;
    const float w = (r.width - gap * (n - 1)) / n;
    for (size_t i = 0; i < m_cores.size(); ++i) {
        Rect bar{r.x + static_cast<float>(i) * (w + gap), r.y, w, r.height};
        p.fillRoundedRect(bar, 2.5f, t.colors.fill);
        float hu = static_cast<float>(std::clamp(m_cores[i].user, 0.0, 1.0)) * bar.height;
        float hs = static_cast<float>(std::clamp(m_cores[i].system, 0.0, 1.0)) * bar.height;
        p.pushClip(bar, 2.5f);
        p.fillRect({bar.x, bar.bottom() - hu, bar.width, hu}, t.colors.blue);
        p.fillRect({bar.x, bar.bottom() - hu - hs, bar.width, hs}, t.colors.red);
        p.popClip();
    }
}

Size UsageBar::measureContent(Size available) {
    return {std::isfinite(available.width) ? available.width : 160.0f, 8};
}

void UsageBar::draw(Painter& p) {
    const Theme& t = theme();
    const Rect r = frame();
    const float radius = r.height * 0.5f;
    p.fillRoundedRect(r, radius, t.colors.fill);
    p.pushClip(r, radius);
    float x = r.x;
    for (const auto& s : m_segments) {
        float w = static_cast<float>(std::clamp(s.fraction, 0.0, 1.0)) * r.width;
        p.fillRect({x, r.y, w, r.height}, s.color);
        x += w;
    }
    p.popClip();
}

// --- table model -------------------------------------------------------------------------------------

std::vector<std::pair<float, float>> TableModel::layout(float width) const {
    std::vector<std::pair<float, float>> out;
    float fixed = 0;
    for (size_t i = 1; i < columns.size(); ++i) fixed += columns[i].width;
    float x = 0;
    for (size_t i = 0; i < columns.size(); ++i) {
        float w = i == 0 ? std::max(150.0f, width - fixed) : columns[i].width;
        out.emplace_back(x, w);
        x += w;
    }
    return out;
}

void TableModel::sort() {
    if (sortColumn < 0 || sortColumn >= static_cast<int>(columns.size())) return;
    const Column& col = columns[static_cast<size_t>(sortColumn)];
    const bool desc = descending;
    std::stable_sort(rows.begin(), rows.end(), [&](const ProcessInfo* a, const ProcessInfo* b) {
        if (col.key) {
            double ka = col.key(*a);
            double kb = col.key(*b);
            if (ka != kb) return desc ? ka > kb : ka < kb;
        } else {
            std::string ta = col.text(*a);
            std::string tb = col.text(*b);
            std::transform(ta.begin(), ta.end(), ta.begin(), [](unsigned char ch) { return std::tolower(ch); });
            std::transform(tb.begin(), tb.end(), tb.begin(), [](unsigned char ch) { return std::tolower(ch); });
            if (ta != tb) return desc ? ta > tb : ta < tb;
        }
        return a->pid < b->pid;
    });
}

// --- header ------------------------------------------------------------------------------------------

int TableHeader::columnAt(float x) const {
    auto cols = m_model->layout(frame().width);
    for (size_t i = 0; i < cols.size(); ++i) {
        float cx = frame().x + cols[i].first;
        if (x >= cx && x < cx + cols[i].second) return static_cast<int>(i);
    }
    return -1;
}

int TableHeader::resizeHandleAt(float x) const {
    auto cols = m_model->layout(frame().width);
    // The right edge of every fixed column can be dragged (the name column absorbs the change).
    for (size_t i = 1; i + 1 < cols.size(); ++i) {
        float edge = frame().x + cols[i].first + cols[i].second;
        if (std::abs(x - edge) <= 4) return static_cast<int>(i);
    }
    return -1;
}

void TableHeader::draw(Painter& p) {
    const Theme& t = theme();
    const auto& c = t.colors;
    const Rect r = frame();
    p.fillRect(r, t.isDark() ? Color::hex(0x1E1E20) : Color::white());
    const float px = 1.0f / p.scale();
    p.fillRect({r.x, p.snap(r.bottom() - px), r.width, px}, c.separator);
    const Font f = t.font(TextStyle::Subheadline).withWeight(FontWeight::Semibold);
    auto cols = m_model->layout(r.width);
    for (size_t i = 0; i < cols.size(); ++i) {
        const Column& col = m_model->columns[i];
        Rect cell{r.x + cols[i].first, r.y, cols[i].second, r.height - px};
        if (static_cast<int>(i) == m_hover && m_resizing < 0) p.fillRect(cell, c.hover);
        if (i > 0) p.fillRect({p.snap(cell.x), cell.y + 7, px, cell.height - 14}, c.separator);
        const bool active = static_cast<int>(i) == m_model->sortColumn;
        Rect text = cell.inset(Insets(0, 10, 0, active ? 22 : 10));
        p.pushClip(cell);
        p.drawText(mayo::uiText(col.title), text, f, active ? c.label : c.secondaryLabel,
                   col.numeric ? TextAlign::Trailing : TextAlign::Leading);
        if (active) {
            Icon arrow = m_model->descending ? Icon::ChevronDown : Icon::ChevronUp;
            p.drawIcon(arrow, {cell.right() - 18, cell.center().y - 4.5f, 9, 9}, c.secondaryLabel, 1.4f);
        }
        p.popClip();
    }
}

bool TableHeader::onPointerDown(const PointerEvent& e) {
    if (e.button != MouseButton::Left) return false;
    m_resizing = resizeHandleAt(e.position.x);
    if (m_resizing >= 0) {
        m_resizeStartX = e.position.x;
        m_resizeStartWidth = m_model->columns[static_cast<size_t>(m_resizing)].width;
        return true;
    }
    m_pressed = columnAt(e.position.x);
    return m_pressed >= 0;
}

void TableHeader::onPointerMove(const PointerEvent& e) {
    if (m_resizing >= 0) {
        auto& col = m_model->columns[static_cast<size_t>(m_resizing)];
        col.width = std::max(44.0f, m_resizeStartWidth + (e.position.x - m_resizeStartX));
        invalidate();
        if (onColumnsResized) onColumnsResized();
        return;
    }
    setCursor(resizeHandleAt(e.position.x) >= 0 ? std::optional(CursorShape::ResizeHorizontal) : std::nullopt);
    int h = columnAt(e.position.x);
    if (h != m_hover) {
        m_hover = h;
        invalidate();
    }
}

void TableHeader::onPointerUp(const PointerEvent& e) {
    if (m_resizing >= 0) {
        m_resizing = -1;
        return;
    }
    int col = columnAt(e.position.x);
    if (col >= 0 && col == m_pressed) {
        if (m_model->sortColumn == col) {
            m_model->descending = !m_model->descending;
        } else {
            m_model->sortColumn = col;
            m_model->descending = m_model->columns[static_cast<size_t>(col)].numeric;  // numbers: biggest first
        }
        if (onSortChanged) onSortChanged();
        invalidate();
    }
    m_pressed = -1;
}

void TableHeader::onPointerLeave() {
    m_hover = -1;
    setCursor(std::nullopt);
    invalidate();
}

// --- rows --------------------------------------------------------------------------------------------

TableRows::TableRows(std::shared_ptr<TableModel> model) : m_model(std::move(model)) {
    m_menuAnchor = std::make_shared<Widget>();
    addChild(m_menuAnchor);
}

void TableRows::rowsChanged() { invalidateLayout(); }

Size TableRows::measureContent(Size available) {
    return {std::isfinite(available.width) ? available.width : 600.0f,
            static_cast<float>(m_model->rows.size()) * kRowHeight + 4};
}

int TableRows::rowAt(float y) const {
    int i = static_cast<int>(std::floor((y - frame().y) / kRowHeight));
    return i >= 0 && i < static_cast<int>(m_model->rows.size()) ? i : -1;
}

int TableRows::selectedIndex() const {
    for (size_t i = 0; i < m_model->rows.size(); ++i) {
        if (m_model->rows[i]->pid == m_model->selected) return static_cast<int>(i);
    }
    return -1;
}

void TableRows::select(pid_t pid, bool scroll) {
    m_model->selected = pid;
    if (scroll) ensureVisible(selectedIndex());
    invalidate();
    if (onSelect) onSelect(pid);
}

void TableRows::ensureVisible(int index) {
    if (!m_scroller || index < 0) return;
    const float top = static_cast<float>(index) * kRowHeight;
    const float visible = m_scroller->frame().height;
    float offset = m_scroller->offset().y;
    if (top < offset) offset = top;
    if (top + kRowHeight > offset + visible) offset = top + kRowHeight - visible;
    if (offset != m_scroller->offset().y) m_scroller->scrollTo({0, offset}, false);
}

void drawProcessIcon(Painter& p, const Rect& r, const ProcessInfo& info) {
    const Theme& t = theme();
    if (info.isKernelThread) {
        p.strokeCircle(r.center(), r.width * 0.5f, t.colors.tertiaryLabel, 1);
        p.drawIcon(Icon::Gear, r.inset(3), t.colors.tertiaryLabel, 0.9f);
        return;
    }
    const Color palette[] = {t.colors.blue, t.colors.indigo, t.colors.purple, t.colors.pink, t.colors.orange,
                             t.colors.teal, t.colors.green, t.colors.red, t.colors.brown, t.colors.cyan};
    std::size_t h = std::hash<std::string>{}(info.name);
    Color c = palette[h % std::size(palette)];
    p.fillVerticalGradient(r, r.width * 0.5f, c.lighter(0.15f), c);
    std::string initial = info.name.empty() ? "?" : std::string(1, static_cast<char>(std::toupper(static_cast<unsigned char>(info.name[0]))));
    Font f{r.height * 0.58f, FontWeight::Semibold, r.height};
    p.drawText(initial, r, f, Color::white(), TextAlign::Center);
}

void TableRows::draw(Painter& p) {
    const Theme& t = theme();
    const auto& c = t.colors;
    const Rect r = frame();
    const Rect clip = p.clipBounds();
    const bool focused = isFocused() && host() && host()->isKeyWindow();
    const Font font = t.font(TextStyle::Body);
    auto cols = m_model->layout(r.width);
    const int first = std::max(0, static_cast<int>((clip.y - r.y) / kRowHeight) - 1);
    const int last = std::min(static_cast<int>(m_model->rows.size()), static_cast<int>((clip.bottom() - r.y) / kRowHeight) + 2);
    for (int i = first; i < last; ++i) {
        const ProcessInfo& info = *m_model->rows[static_cast<size_t>(i)];
        Rect row{r.x, r.y + static_cast<float>(i) * kRowHeight, r.width, kRowHeight};
        const bool selected = info.pid == m_model->selected;
        if (selected) {
            p.fillRoundedRect(row.inset(4, 0), 6, focused ? c.accent : c.unemphasizedSelection.mix(c.fill, 0.5f));
        } else {
            if (i % 2 == 1) p.fillRoundedRect(row.inset(4, 0), 6, c.tertiaryFill);
            if (i == m_hover) p.fillRoundedRect(row.inset(4, 0), 6, c.hover);
        }
        const Color text = selected && focused ? c.onAccent : info.isKernelThread ? c.secondaryLabel : c.label;
        for (size_t k = 0; k < cols.size(); ++k) {
            const Column& col = m_model->columns[k];
            Rect cell{r.x + cols[k].first, row.y, cols[k].second, row.height};
            Rect inner = cell.inset(10, 0);
            p.pushClip(cell);
            if (k == 0) {
                drawProcessIcon(p, {inner.x, row.center().y - 8, 16, 16}, info);
                inner = Rect::fromLTRB(inner.x + 24, inner.y, inner.right(), inner.bottom());
            }
            p.drawText(fitText(p, col.text(info), font, inner.width), inner, font, text,
                       col.numeric ? TextAlign::Trailing : TextAlign::Leading);
            p.popClip();
        }
    }
    paintChildren(p);
}

bool TableRows::onPointerDown(const PointerEvent& e) {
    if (auto* h = host()) h->setFocus(this, false);
    int i = rowAt(e.position.y);
    if (i < 0) return true;
    pid_t pid = m_model->rows[static_cast<size_t>(i)]->pid;
    select(pid, false);
    if (e.button == MouseButton::Right) {
        showContextMenu(e.position, pid);
    } else if (e.clickCount == 2 && onOpen) {
        onOpen(pid);
    }
    return true;
}

void TableRows::onPointerMove(const PointerEvent& e) {
    int i = rowAt(e.position.y);
    if (i != m_hover) {
        m_hover = i;
        invalidate();
    }
}

void TableRows::onPointerLeave() {
    m_hover = -1;
    invalidate();
}

void TableRows::showContextMenu(Point at, pid_t pid) {
    const ProcessInfo* info = nullptr;
    for (const auto* row : m_model->rows) {
        if (row->pid == pid) info = row;
    }
    if (!info) return;
    const std::string command = info->command.empty() ? info->name : info->command;
    std::vector<MenuItem> items{
        MenuItem("Inspect…", [this, pid] {
            if (onOpen) onOpen(pid);
        }).withIcon(Icon::Info),
        MenuItem::separator(),
        MenuItem("Quit", [this, pid] {
            if (onQuit) onQuit(pid, false);
        }).withIcon(Icon::Xmark),
        MenuItem("Force Quit", [this, pid] {
            if (onQuit) onQuit(pid, true);
        }).asDestructive(),
        MenuItem::separator(),
        MenuItem("Copy PID", [this, pid] {
            if (auto* h = host()) h->setClipboardText(std::to_string(pid));
        }),
        MenuItem("Copy Command", [this, command] {
            if (auto* h = host()) h->setClipboardText(command);
        }),
    };
    if (!m_menu) m_menu = std::make_shared<MenuView>(std::vector<MenuItem>{});
    m_menu->setItems(std::move(items));
    m_menu->setGap(2);
    m_menuAnchor->arrange({at.x, at.y, 1, 1});
    m_menu->showFrom(*m_menuAnchor);
    if (auto* h = host()) h->setFocus(m_menu.get(), false);
}

bool TableRows::onKeyDown(const KeyEvent& e) {
    const int n = static_cast<int>(m_model->rows.size());
    if (n == 0) return false;
    int i = selectedIndex();
    const int page = m_scroller ? std::max(1, static_cast<int>(m_scroller->frame().height / kRowHeight) - 1) : 10;
    int target = i;
    switch (e.key) {
        case Key::Up: target = std::max(0, i - 1); break;
        case Key::Down: target = std::min(n - 1, i + 1); break;
        case Key::PageUp: target = std::max(0, i - page); break;
        case Key::PageDown: target = std::min(n - 1, i + page); break;
        case Key::Home: target = 0; break;
        case Key::End: target = n - 1; break;
        case Key::Enter:
            if (i >= 0 && onOpen) onOpen(m_model->selected);
            return true;
        case Key::Delete:
        case Key::Backspace:
            if (i >= 0 && onQuit) onQuit(m_model->selected, false);
            return true;
        default: return false;
    }
    if (target < 0) target = 0;
    select(m_model->rows[static_cast<size_t>(target)]->pid, true);
    return true;
}
