#include "terminal_view.hpp"

#include <algorithm>
#include <cctype>
#include <chrono>
#include <cmath>
#include <cstring>

using namespace mayo;

namespace term {

namespace {

constexpr float kPadX = 6;
constexpr float kPadY = 3;
constexpr double kBlinkPeriod = 0.53;
constexpr double kBlinkTimeout = 30;  // stop blinking (and waking the CPU) after this much idle time
constexpr size_t kOutputChunk = 64 * 1024;
constexpr double kOutputBudget = 0.008;  // seconds of parsing per event-loop pass; then draw and take input

size_t encodeUtf8(char32_t cp, char* out) {
    if (cp < 0x80) {
        out[0] = static_cast<char>(cp);
        return 1;
    }
    if (cp < 0x800) {
        out[0] = static_cast<char>(0xC0 | (cp >> 6));
        out[1] = static_cast<char>(0x80 | (cp & 0x3F));
        return 2;
    }
    if (cp < 0x10000) {
        out[0] = static_cast<char>(0xE0 | (cp >> 12));
        out[1] = static_cast<char>(0x80 | ((cp >> 6) & 0x3F));
        out[2] = static_cast<char>(0x80 | (cp & 0x3F));
        return 3;
    }
    out[0] = static_cast<char>(0xF0 | (cp >> 18));
    out[1] = static_cast<char>(0x80 | ((cp >> 12) & 0x3F));
    out[2] = static_cast<char>(0x80 | ((cp >> 6) & 0x3F));
    out[3] = static_cast<char>(0x80 | (cp & 0x3F));
    return 4;
}

bool isWordChar(char32_t c) {
    if (c > 0x7F) return c != 0xA0;  // letters of other scripts count as word characters
    return std::isalnum(static_cast<int>(c)) || std::strchr("-_.~/:@%+=#?&,$", static_cast<int>(c)) != nullptr;
}

/// Box-drawing line weights (up, right, down, left): 1 light, 2 heavy, 3 double.
struct BoxLines {
    std::uint8_t up, right, down, left;
};

bool boxLines(char32_t cp, BoxLines& b) {
    switch (cp) {
        case 0x2500: case 0x2504: case 0x2508: case 0x254C: b = {0, 1, 0, 1}; return true;
        case 0x2501: case 0x2505: case 0x2509: case 0x254D: b = {0, 2, 0, 2}; return true;
        case 0x2502: case 0x2506: case 0x250A: case 0x254E: b = {1, 0, 1, 0}; return true;
        case 0x2503: case 0x2507: case 0x250B: case 0x254F: b = {2, 0, 2, 0}; return true;
        case 0x250C: b = {0, 1, 1, 0}; return true;
        case 0x250F: b = {0, 2, 2, 0}; return true;
        case 0x2510: b = {0, 0, 1, 1}; return true;
        case 0x2513: b = {0, 0, 2, 2}; return true;
        case 0x2514: b = {1, 1, 0, 0}; return true;
        case 0x2517: b = {2, 2, 0, 0}; return true;
        case 0x2518: b = {1, 0, 0, 1}; return true;
        case 0x251B: b = {2, 0, 0, 2}; return true;
        case 0x251C: b = {1, 1, 1, 0}; return true;
        case 0x2523: b = {2, 2, 2, 0}; return true;
        case 0x2524: b = {1, 0, 1, 1}; return true;
        case 0x252B: b = {2, 0, 2, 2}; return true;
        case 0x252C: b = {0, 1, 1, 1}; return true;
        case 0x2533: b = {0, 2, 2, 2}; return true;
        case 0x2534: b = {1, 1, 0, 1}; return true;
        case 0x253B: b = {2, 2, 0, 2}; return true;
        case 0x253C: b = {1, 1, 1, 1}; return true;
        case 0x254B: b = {2, 2, 2, 2}; return true;
        case 0x2550: b = {0, 3, 0, 3}; return true;
        case 0x2551: b = {3, 0, 3, 0}; return true;
        case 0x2554: b = {0, 3, 3, 0}; return true;
        case 0x2557: b = {0, 0, 3, 3}; return true;
        case 0x255A: b = {3, 3, 0, 0}; return true;
        case 0x255D: b = {3, 0, 0, 3}; return true;
        case 0x2560: b = {3, 3, 3, 0}; return true;
        case 0x2563: b = {3, 0, 3, 3}; return true;
        case 0x2566: b = {0, 3, 3, 3}; return true;
        case 0x2569: b = {3, 3, 0, 3}; return true;
        case 0x256C: b = {3, 3, 3, 3}; return true;
        case 0x2574: b = {0, 0, 0, 1}; return true;
        case 0x2575: b = {1, 0, 0, 0}; return true;
        case 0x2576: b = {0, 1, 0, 0}; return true;
        case 0x2577: b = {0, 0, 1, 0}; return true;
        case 0x2578: b = {0, 0, 0, 2}; return true;
        case 0x2579: b = {2, 0, 0, 0}; return true;
        case 0x257A: b = {0, 2, 0, 0}; return true;
        case 0x257B: b = {0, 0, 2, 0}; return true;
        default: return false;
    }
}

}  // namespace

TerminalView::TerminalView(App& app, std::shared_ptr<Config> config)
    : m_app(app), m_config(std::move(config)), m_screen(80, 24, m_config->scrollback), m_pty(app) {
    setCursor(mayo::CursorShape::IBeam);
    m_physics.setAxes(false, true);
    m_physics.setBouncesWhenFitting(false, true);
    m_screen.respond = [this](const std::string& reply) { m_pty.write(reply); };
    m_screen.onBell = [this] {
        if (!m_config->visualBell) return;
        m_bell.set(1.0f);
        m_bell.animateTo(0.0f, now(), Transition{0.35f, Curve::EaseOut});
        invalidate();
    };
    m_screen.onTitle = [this](const std::string&) {
        if (onTitleChanged) onTitleChanged();
    };
    m_screen.onClipboard = [this](const std::string& text) {
        if (auto* h = host()) h->setClipboardText(text);
    };
    m_screen.queryColor = [this](int index) -> std::uint32_t {
        const Color c = index == -1 ? m_profile.foreground : index == -2 ? m_profile.background : index == -3 ? m_profile.cursor
                                                                                                 : m_palette[static_cast<size_t>(index & 0xFF)];
        return (c.toRGBA8() >> 8) & 0xFFFFFF;
    };
    m_screen.onSetPaletteColor = [this](int index, std::uint32_t rgb) {
        if (index < 0) {
            m_paletteOverridden.fill(false);
        } else if (index < 256) {
            m_paletteOverridden[static_cast<size_t>(index)] = true;
            m_overrides[static_cast<size_t>(index)] = Color::hex(rgb);
        }
        applyConfig();
    };
    m_pty.onOutput = [this] { output(); };
    m_pty.onExit = [this](int status) {
        m_exited = true;
        invalidate();
        if (onExit) onExit(status);
    };
    applyConfig();
}

TerminalView::~TerminalView() = default;

bool TerminalView::start(const std::string& cwd, std::string& error) {
    Pty::Options options;
    options.cwd = cwd;
    options.cols = m_screen.cols();
    options.rows = m_screen.rows();
    m_started = m_pty.start(options, error);
    if (m_started) {
        m_pty.resize(m_screen.cols(), m_screen.rows(), static_cast<int>(std::lround(m_cellW * m_scale * static_cast<float>(m_screen.cols()))),
                     static_cast<int>(std::lround(m_cellH * m_scale * static_cast<float>(m_screen.rows()))));
    }
    return m_started;
}

void TerminalView::applyConfig() {
    m_profile = profileNamed(m_config->profile, theme().isDark());
    m_palette = palette256(m_profile);
    for (size_t i = 0; i < 256; ++i) {
        if (m_paletteOverridden[i]) m_palette[i] = m_overrides[i];
    }
    FontFamily family = 0;
    if (auto f = systemFontFamily(m_config->fontFamily)) family = *f;
    else if (auto mono = systemFontFamily("monospace")) family = *mono;
    m_font = Font{m_config->fontSize, FontWeight::Regular, m_config->fontSize * 1.2f, family};
    m_screen.setScrollbackLimit(m_config->scrollback);
    m_scale = 0;  // recompute cell metrics at the next draw
    invalidateLayout();
}

void TerminalView::updateMetrics(float scale) {
    m_scale = scale;
    const float adv = glyphAdvance('M', m_font);
    m_cellW = std::max(1.0f, std::round(adv * scale)) / scale;
    const LineMetrics lm = fontMetrics(m_font);
    const float natural = lm.ascender + lm.descender;
    m_cellH = std::max(1.0f, std::round(natural * m_config->lineSpacing * scale)) / scale;
    const float extra = m_cellH - natural;
    m_baseline = std::round((lm.ascender + extra * 0.5f) * scale) / scale;
    scrollToLines(m_scrollOffset, false);  // the physics works in points; keep the same lines in view
}

void TerminalView::arrangeContent(const Rect& content) {
    if (m_scale <= 0) updateMetrics(m_drawScale);
    const float s = m_scale;
    m_origin = {std::round((content.x + kPadX) * s) / s, std::round((content.y + kPadY) * s) / s};
    updateGrid();
}

void TerminalView::updateGrid() {
    const Rect r = frame();
    const int cols = std::max(2, static_cast<int>((r.width - kPadX * 2) / m_cellW));
    const int rows = std::max(1, static_cast<int>((r.height - kPadY * 2) / m_cellH));
    if (cols == m_screen.cols() && rows == m_screen.rows()) return;
    m_screen.resize(cols, rows);
    m_selection.active = false;
    scrollToLines(std::min(m_scrollOffset, m_screen.scrollbackSize()), false);
    if (m_started) {
        m_pty.resize(cols, rows, static_cast<int>(std::lround(m_cellW * m_scale * static_cast<float>(cols))),
                     static_cast<int>(std::lround(m_cellH * m_scale * static_cast<float>(rows))));
    }
    if (onTitleChanged) onTitleChanged();  // the title shows the size
}

// --- output & input ----------------------------------------------------------------------------------

void TerminalView::output() {
    const int before = m_screen.scrollbackSize();
    const auto droppedBefore = m_screen.droppedLines();
    // Parse in slices: a flood (`cat huge.log`, a noisy build) must not freeze drawing and typing. Whatever
    // doesn't fit in the budget continues on the next pass of the event loop, after a frame.
    const auto start = std::chrono::steady_clock::now();
    const auto budget = std::chrono::duration<double>(kOutputBudget);
    bool more = false;
    bool any = false;
    do {
        const std::string data = m_pty.takeOutput(kOutputChunk, more);
        if (data.empty()) break;
        m_screen.feed(data);
        any = true;
    } while (more && std::chrono::steady_clock::now() - start < budget);
    if (more) {
        std::weak_ptr<Widget> weak = weak_from_this();
        m_app.post([this, weak] {
            if (weak.lock()) output();
        });
    }
    if (!any) return;

    const int grew = m_screen.scrollbackSize() - before;
    const auto dropped = static_cast<float>(m_screen.droppedLines() - droppedBefore);
    if (grew != 0 || dropped != 0) {
        const float backPx = m_physics.maxOffset().y - m_physics.offset().y;
        updateScrollLimits();
        // At the live end the view follows the output; while reading history, the history stays put.
        if (backPx <= 0.5f) m_physics.shift({0, static_cast<float>(grew) * m_cellH});
        else m_physics.shift({0, -dropped * m_cellH});
        if (!m_physics.isAnimating()) m_physics.setOffset(m_physics.offset());
        syncScroll();
    }
    m_blinkStart = now();
    invalidate();
}

void TerminalView::updateScrollLimits() {
    const float rowsH = static_cast<float>(m_screen.rows()) * m_cellH;
    m_physics.setLimits({0, static_cast<float>(m_screen.scrollbackSize()) * m_cellH}, {frame().width, rowsH});
}

void TerminalView::syncScroll() {
    const int history = m_screen.scrollbackSize();
    const float back = m_physics.maxOffset().y - m_physics.offset().y;
    const int lines = std::clamp(static_cast<int>(std::ceil(back / m_cellH - 0.001f)), 0, history);
    const float scale = m_scale > 0 ? m_scale : 1.0f;
    m_scrollOffset = lines;
    m_scrollShift = std::round((back - static_cast<float>(lines) * m_cellH) * scale) / scale;
}

void TerminalView::scrollToLines(int linesBack, bool animated) {
    updateScrollLimits();
    const float y = m_physics.maxOffset().y - static_cast<float>(std::max(0, linesBack)) * m_cellH;
    m_physics.scrollTo({0, y}, animated, now());
    syncScroll();
    if (animated) {
        m_lastScroll = now();
        requestAnimationFrame();
    }
    invalidate();
}

void TerminalView::stepScroll(double t) {
    if (!m_physics.isAnimating()) return;
    if (m_physics.step(t)) {
        m_lastScroll = t;
        requestAnimationFrame();
    } else if (auto* h = host(); h && m_physics.deadline() >= 0) {
        h->scheduleFrame(m_physics.deadline());
    }
    syncScroll();
}

void TerminalView::send(std::string_view bytes) { m_pty.write(bytes); }

void TerminalView::userInput() {
    if (m_scrollOffset != 0 || m_scrollShift != 0) scrollToLines(0, false);
    m_blinkStart = now();
    invalidate();
}

bool TerminalView::onKeyDown(const KeyEvent& e) {
    const bool ctrl = e.primary();
    const bool alt = e.alt();
    const bool shift = e.shift();
    // App shortcuts (tabs, copy/paste, zoom, settings) are Ctrl+Shift+… or Ctrl with punctuation.
    if (ctrl && (shift || e.key == Key::Comma || e.key == Key::Equals || e.key == Key::Minus || e.key == Key::Num0 ||
                 e.key == Key::PageUp || e.key == Key::PageDown || e.key == Key::Tab))
        return false;
    if (shift && !ctrl && !alt && !m_screen.altScreen()) {
        // Shift+PageUp/PageDown/Home/End browse the scrollback.
        const int page = std::max(1, m_screen.rows() - 1);
        int offset = m_scrollOffset;
        if (e.key == Key::PageUp) offset += page;
        else if (e.key == Key::PageDown) offset -= page;
        else if (e.key == Key::Home) offset = m_screen.scrollbackSize();
        else if (e.key == Key::End) offset = 0;
        else offset = -1;
        if (offset >= 0 || e.key == Key::PageDown) {
            scrollToLines(std::clamp(offset, 0, m_screen.scrollbackSize()), true);
            return true;
        }
    }

    const int mods = 1 + (shift ? 1 : 0) + (alt ? 2 : 0) + (ctrl ? 4 : 0);
    const std::string m = std::to_string(mods);
    std::string seq;
    auto cursorKey = [&](char c) {
        if (mods > 1) seq = "\x1b[1;" + m + c;
        else seq = std::string(m_screen.appCursorKeys() ? "\x1bO" : "\x1b[") + c;
    };
    auto tildeKey = [&](int n) { seq = "\x1b[" + std::to_string(n) + (mods > 1 ? ";" + m : "") + "~"; };
    switch (e.key) {
        case Key::Enter: seq = alt ? "\x1b\r" : "\r"; break;
        case Key::Backspace: seq = ctrl ? "\x08" : alt ? "\x1b\x7f" : "\x7f"; break;
        case Key::Tab: seq = shift ? "\x1b[Z" : "\t"; break;
        case Key::Escape: seq = "\x1b"; break;
        case Key::Up: cursorKey('A'); break;
        case Key::Down: cursorKey('B'); break;
        case Key::Right: cursorKey('C'); break;
        case Key::Left: cursorKey('D'); break;
        case Key::Home: cursorKey('H'); break;
        case Key::End: cursorKey('F'); break;
        case Key::Insert: tildeKey(2); break;
        case Key::Delete: tildeKey(3); break;
        case Key::PageUp: tildeKey(5); break;
        case Key::PageDown: tildeKey(6); break;
        case Key::F1:
        case Key::F2:
        case Key::F3:
        case Key::F4: {
            const char c = static_cast<char>('P' + (static_cast<int>(e.key) - static_cast<int>(Key::F1)));
            seq = mods > 1 ? "\x1b[1;" + m + c : std::string("\x1bO") + c;
            break;
        }
        case Key::F5: tildeKey(15); break;
        case Key::F6: tildeKey(17); break;
        case Key::F7: tildeKey(18); break;
        case Key::F8: tildeKey(19); break;
        case Key::F9: tildeKey(20); break;
        case Key::F10: tildeKey(21); break;
        case Key::F11: tildeKey(23); break;
        case Key::F12: tildeKey(24); break;
        case Key::Space:
            if (ctrl) seq = std::string(1, '\0');
            else if (alt) seq = "\x1b ";
            break;
        default: break;
    }
    const bool letter = e.key >= Key::A && e.key <= Key::Z;
    const bool digit = e.key >= Key::Num0 && e.key <= Key::Num9;
    if (seq.empty() && letter && ctrl) {
        seq = std::string(alt ? "\x1b" : "") + static_cast<char>(1 + static_cast<int>(e.key) - static_cast<int>(Key::A));
    } else if (seq.empty() && (letter || digit) && alt && !ctrl) {
        // Alt+key sends ESC + key, unless it is AltGr producing a character (Turkish @, € ...): the text
        // input that follows decides, see onTextInput.
        const char c = letter ? static_cast<char>((shift ? 'A' : 'a') + (static_cast<int>(e.key) - static_cast<int>(Key::A)))
                              : static_cast<char>('0' + (static_cast<int>(e.key) - static_cast<int>(Key::Num0)));
        m_pendingAlt = std::string("\x1b") + c;
        std::weak_ptr<Widget> weak = weak_from_this();
        m_app.post([this, weak] {
            if (!weak.lock() || m_pendingAlt.empty()) return;
            send(m_pendingAlt);
            m_pendingAlt.clear();
            userInput();
        });
        return true;
    }
    if (seq.empty()) return false;
    send(seq);
    userInput();
    return true;
}

bool TerminalView::onTextInput(const TextInputEvent& e) {
    if (e.text.empty()) return true;
    if (!m_pendingAlt.empty()) {
        const std::string pending = m_pendingAlt;
        m_pendingAlt.clear();
        // Left Alt still reports the plain character; AltGr reports a different one.
        const bool plain = e.text.size() == 1 && std::tolower(static_cast<unsigned char>(e.text[0])) ==
                                                     std::tolower(static_cast<unsigned char>(pending[1]));
        send(plain ? pending : e.text);
    } else {
        send(e.text);
    }
    userInput();
    return true;
}

void TerminalView::onFocusChanged(bool focused) {
    if (m_screen.focusReporting()) send(focused ? "\x1b[I" : "\x1b[O");
    if (auto* h = host()) h->setTextInput(focused, m_imeRect);
    m_blinkStart = now();
    invalidate();
}

// --- clipboard & selection ---------------------------------------------------------------------------

void TerminalView::copySelection() {
    if (!m_selection.active) return;
    if (auto* h = host()) h->setClipboardText(selectedText());
}

void TerminalView::paste() {
    if (auto* h = host()) pasteText(h->clipboardText());
}

void TerminalView::pasteText(const std::string& text) {
    if (text.empty()) return;
    std::string t;
    t.reserve(text.size());
    for (size_t i = 0; i < text.size(); ++i) {
        if (text[i] == '\r' && i + 1 < text.size() && text[i + 1] == '\n') continue;
        t.push_back(text[i] == '\n' ? '\r' : text[i]);
    }
    if (m_screen.bracketedPaste()) {
        // A pasted end marker could smuggle commands past the shell's paste handling.
        for (size_t pos; (pos = t.find("\x1b[201~")) != std::string::npos;) t.erase(pos, 6);
        send("\x1b[200~" + t + "\x1b[201~");
    } else {
        send(t);
    }
    userInput();
}

void TerminalView::selectAll() {
    const auto dropped = static_cast<std::int64_t>(m_screen.droppedLines());
    m_selection.active = true;
    m_selection.mode = SelectMode::Char;
    m_selection.anchor = {dropped, 0};
    m_selection.head = {dropped + m_screen.scrollbackSize() + m_screen.rows() - 1, m_screen.cols() - 1};
    invalidate();
}

void TerminalView::clearScrollback() {
    m_screen.clearScrollback();
    scrollToLines(0, false);
    m_selection.active = false;
    invalidate();
}

std::int64_t TerminalView::topLine() const {
    return static_cast<std::int64_t>(m_screen.droppedLines()) + m_screen.scrollbackSize() - m_scrollOffset;
}

const Line* TerminalView::lineGlobal(std::int64_t line) const {
    const std::int64_t abs = line - static_cast<std::int64_t>(m_screen.droppedLines());
    if (abs < 0 || abs >= m_screen.scrollbackSize() + m_screen.rows()) return nullptr;
    return &m_screen.lineAt(static_cast<int>(abs));
}

TerminalView::Pos TerminalView::cellAt(Point p, bool clampToGrid) const {
    int col = static_cast<int>(std::floor((p.x - m_origin.x) / m_cellW));
    int row = static_cast<int>(std::floor((p.y - m_origin.y - m_scrollShift) / m_cellH));
    if (clampToGrid) {
        col = std::clamp(col, 0, m_screen.cols() - 1);
        row = std::clamp(row, 0, m_screen.rows() - 1);
    }
    return {topLine() + row, col};
}

void TerminalView::extendWord(Pos& start, Pos& end) const {
    auto cp = [&](const Pos& p) -> char32_t {
        const Line* l = lineGlobal(p.line);
        if (!l || p.col < 0 || p.col >= static_cast<int>(l->cells.size())) return ' ';
        return l->cells[static_cast<size_t>(p.col)].codepoint();
    };
    if (!isWordChar(cp(start))) return;  // double-click on a space selects just it
    while (start.col > 0 && isWordChar(cp({start.line, start.col - 1}))) --start.col;
    while (end.col + 1 < m_screen.cols() && isWordChar(cp({end.line, end.col + 1}))) ++end.col;
}

void TerminalView::selectionBounds(Pos& start, Pos& end) const {
    start = std::min(m_selection.anchor, m_selection.head);
    end = std::max(m_selection.anchor, m_selection.head);
    if (m_selection.mode == SelectMode::Word) {
        Pos first = start;
        Pos firstEnd = start;
        extendWord(first, firstEnd);
        Pos lastStart = end;
        Pos last = end;
        extendWord(lastStart, last);
        start = first;
        end = last;
    } else if (m_selection.mode == SelectMode::Line) {
        // Whole logical lines, including their soft-wrapped continuations.
        start.col = 0;
        while (const Line* prev = lineGlobal(start.line - 1)) {
            if (!prev->wrapped) break;
            --start.line;
        }
        end.col = m_screen.cols() - 1;
        while (const Line* l = lineGlobal(end.line)) {
            if (!l->wrapped || !lineGlobal(end.line + 1)) break;
            ++end.line;
        }
    }
}

bool TerminalView::selected(std::int64_t line, int col) const {
    if (!m_selection.active) return false;
    Pos s;
    Pos e;
    selectionBounds(s, e);
    const Pos p{line, col};
    return !(p < s) && !(e < p);
}

std::string TerminalView::selectedText() const {
    Pos s;
    Pos e;
    selectionBounds(s, e);
    std::string out;
    for (std::int64_t line = s.line; line <= e.line; ++line) {
        const Line* l = lineGlobal(line);
        if (!l) continue;
        const int from = line == s.line ? s.col : 0;
        const int to = line == e.line ? e.col : m_screen.cols() - 1;
        std::string text;
        char buf[4];
        for (int c = from; c <= to && c < static_cast<int>(l->cells.size()); ++c) {
            const Cell& cell = l->cells[static_cast<size_t>(c)];
            if (cell.attrs() & kWideSpacer) continue;
            text.append(buf, encodeUtf8(cell.codepoint(), buf));
        }
        const bool continues = l->wrapped && line != e.line;
        if (!continues) {
            while (!text.empty() && text.back() == ' ') text.pop_back();
        }
        out += text;
        if (!continues && line != e.line) out += '\n';
    }
    return out;
}

std::string TerminalView::lineText(std::int64_t line, std::vector<int>* columns) const {
    std::string text;
    const Line* l = lineGlobal(line);
    if (!l) return text;
    char buf[4];
    for (int c = 0; c < static_cast<int>(l->cells.size()); ++c) {
        const Cell& cell = l->cells[static_cast<size_t>(c)];
        if (cell.attrs() & kWideSpacer) continue;
        const size_t n = encodeUtf8(cell.codepoint(), buf);
        text.append(buf, n);
        if (columns) columns->insert(columns->end(), n, c);
    }
    return text;
}

std::string TerminalView::urlAt(Point p) const {
    const Pos at = cellAt(p);
    // Join the line with its wrapped neighbours: long URLs span rows.
    std::int64_t first = at.line;
    while (const Line* prev = lineGlobal(first - 1)) {
        if (!prev->wrapped) break;
        --first;
    }
    std::string text;
    std::vector<std::pair<std::int64_t, int>> where;  // per byte: line, column
    for (std::int64_t line = first;; ++line) {
        const Line* l = lineGlobal(line);
        if (!l) break;
        std::vector<int> cols;
        text += lineText(line, &cols);
        for (int c : cols) where.emplace_back(line, c);
        if (!l->wrapped) break;
    }
    for (const char* scheme : {"https://", "http://", "file://", "ftp://"}) {
        for (size_t pos = text.find(scheme); pos != std::string::npos; pos = text.find(scheme, pos + 1)) {
            size_t end = pos;
            while (end < text.size() && !std::isspace(static_cast<unsigned char>(text[end])) && !std::strchr("\"'<>`|", text[end])) ++end;
            while (end > pos && std::strchr(".,;:!?)]}", text[end - 1])) --end;  // trailing punctuation
            for (size_t i = pos; i < end; ++i) {
                if (where[i].first == at.line && where[i].second == at.col) return text.substr(pos, end - pos);
            }
        }
    }
    return {};
}

// --- mouse -----------------------------------------------------------------------------------------------

bool TerminalView::reportsMouse(Modifiers mods) const {
    // Shift always selects, so text can be copied out of full-screen programs.
    return m_screen.mouseMode() != MouseMode::None && !hasModifier(mods, Modifiers::Shift);
}

void TerminalView::sendMouse(int button, bool press, bool motion, Point p, Modifiers mods) {
    if (m_scrollOffset != 0 || m_scrollShift != 0) scrollToLines(0, false);
    const Pos c = cellAt(p);
    const int row = static_cast<int>(c.line - topLine());
    int cb = button;
    if (hasModifier(mods, Modifiers::Alt)) cb |= 8;
    if (hasModifier(mods, Modifiers::Ctrl)) cb |= 16;
    if (motion) cb |= 32;
    if (m_screen.sgrMouse()) {
        send("\x1b[<" + std::to_string(cb) + ";" + std::to_string(c.col + 1) + ";" + std::to_string(row + 1) + (press ? "M" : "m"));
    } else {
        if (!press) cb = (cb & ~3) | 3;
        std::string s = "\x1b[M";
        s.push_back(static_cast<char>(32 + cb));
        s.push_back(static_cast<char>(32 + std::min(c.col + 1, 223)));
        s.push_back(static_cast<char>(32 + std::min(row + 1, 223)));
        send(s);
    }
}

bool TerminalView::onPointerDown(const PointerEvent& e) {
    if (auto* h = host()) h->setFocus(this, false);
    if (reportsMouse(e.modifiers) && e.button != MouseButton::None) {
        const int button = e.button == MouseButton::Left ? 0 : e.button == MouseButton::Middle ? 1 : 2;
        m_mouseButton = button;
        m_lastMouseCell = cellAt(e.position);
        sendMouse(button, true, false, e.position, e.modifiers);
        return true;
    }
    if (e.button == MouseButton::Right) {
        if (onContextMenu) onContextMenu(*this, e.position);
        return true;
    }
    if (e.button == MouseButton::Middle) {
        paste();
        return true;
    }
    if (e.button != MouseButton::Left) return true;
    if (hasModifier(e.modifiers, Modifiers::Ctrl)) {
        // Ctrl+click opens links, like Cmd+click in macOS Terminal.
        if (std::string url = urlAt(e.position); !url.empty()) {
            if (onOpenUrl) onOpenUrl(url);
            return true;
        }
    }
    const Pos c = cellAt(e.position);
    if (hasModifier(e.modifiers, Modifiers::Shift) && m_selection.active) {
        m_selection.head = c;
    } else {
        m_selection.anchor = m_selection.head = c;
        m_selection.mode = e.clickCount >= 3 ? SelectMode::Line : e.clickCount == 2 ? SelectMode::Word : SelectMode::Char;
        m_selection.active = e.clickCount >= 2;
    }
    m_selecting = true;
    invalidate();
    return true;
}

void TerminalView::onPointerMove(const PointerEvent& e) {
    if (reportsMouse(e.modifiers) || m_mouseButton >= 0) {
        const MouseMode mode = m_screen.mouseMode();
        const Pos c = cellAt(e.position);
        if (c == m_lastMouseCell) return;
        m_lastMouseCell = c;
        if (m_mouseButton >= 0 && (mode == MouseMode::Drag || mode == MouseMode::Motion))
            sendMouse(m_mouseButton, true, true, e.position, e.modifiers);
        else if (m_mouseButton < 0 && mode == MouseMode::Motion)
            sendMouse(3, true, true, e.position, e.modifiers);
        return;
    }
    if (!m_selecting || !isPressed()) return;
    // Dragging past the top or bottom edge scrolls the history.
    const Rect r = frame();
    if (e.position.y < r.y && m_scrollOffset < m_screen.scrollbackSize()) scrollToLines(m_scrollOffset + 1, false);
    else if (e.position.y > r.bottom() && m_scrollOffset > 0) scrollToLines(m_scrollOffset - 1, false);
    Pos c = cellAt(e.position);
    if (e.position.x > m_origin.x + m_cellW * static_cast<float>(m_screen.cols())) c.col = m_screen.cols() - 1;
    if (!(c == m_selection.head)) {
        m_selection.head = c;
        m_selection.active = true;
        invalidate();
    }
}

void TerminalView::onPointerUp(const PointerEvent& e) {
    if (m_mouseButton >= 0) {
        sendMouse(m_mouseButton, false, false, e.position, e.modifiers);
        m_mouseButton = -1;
        return;
    }
    m_selecting = false;
}

bool TerminalView::onScroll(const ScrollEvent& e) {
    if (!reportsMouse(e.modifiers) && !m_screen.altScreen()) {
        // The history scrolls smoothly, with the same physics as every other scroll view.
        updateScrollLimits();
        const double t = now();
        if (!m_physics.handle(e, t)) return true;
        syncScroll();
        m_lastScroll = t;
        if (m_physics.isAnimating()) requestAnimationFrame();
        if (auto* h = host(); h && m_physics.deadline() >= 0) h->scheduleFrame(m_physics.deadline());
        invalidate();
        return true;
    }
    if (e.phase == ScrollPhase::Ended) return true;
    m_scrollAccum += e.delta.y / m_cellH;
    const int lines = static_cast<int>(m_scrollAccum);
    m_scrollAccum -= static_cast<float>(lines);
    if (lines == 0) return true;
    if (reportsMouse(e.modifiers)) {
        for (int i = 0; i < std::abs(lines); ++i) sendMouse(lines < 0 ? 64 : 65, true, false, e.position, e.modifiers);
        return true;
    }
    if (m_screen.altScreen()) {
        // Full-screen programs without mouse support (less, man) scroll with the arrow keys.
        const std::string key = std::string(m_screen.appCursorKeys() ? "\x1bO" : "\x1b[") + (lines < 0 ? 'A' : 'B');
        std::string seq;
        for (int i = 0; i < std::abs(lines); ++i) seq += key;
        send(seq);
        return true;
    }
    return true;
}

// --- drawing ---------------------------------------------------------------------------------------------

Color TerminalView::resolve(ColorRef c, bool foreground, std::uint16_t attrs) const {
    const bool reverse = m_screen.reverseVideo();
    if (c == kDefaultColor) {
        return foreground != reverse ? m_profile.foreground : m_profile.background;
    }
    if (isPalette(c)) {
        int i = static_cast<int>(c & 0xFF);
        if (foreground && m_config->boldIsBright && (attrs & kBold) && i < 8) i += 8;
        return m_palette[static_cast<size_t>(i)];
    }
    return Color::rgb8(static_cast<int>((c >> 16) & 0xFF), static_cast<int>((c >> 8) & 0xFF), static_cast<int>(c & 0xFF));
}

bool TerminalView::drawSpecial(Painter& p, char32_t cp, const Rect& cell, const Color& color) const {
    const float s = m_scale;
    const float px = 1.0f / s;
    // Lines and blocks are drawn as geometry so they join seamlessly between cells, whatever the font.
    BoxLines b{};
    if (boxLines(cp, b)) {
        const float light = std::max(px, std::round(m_cellW * 0.09f * s) / s);
        const float cx = std::round((cell.x + m_cellW * 0.5f) * s) / s;
        const float cy = std::round((cell.y + cell.height * 0.5f) * s) / s;
        auto thick = [&](int w) { return w == 2 ? light * 2 : light; };
        auto vertical = [&](float y0, float y1, int w, float dx) {
            const float t = thick(w);
            p.fillRect({cx + dx - t * 0.5f, y0, t, y1 - y0}, color);
        };
        auto horizontal = [&](float x0, float x1, int w, float dy) {
            const float t = thick(w);
            p.fillRect({x0, cy + dy - t * 0.5f, x1 - x0, t}, color);
        };
        const float gap = light * 1.5f;  // double lines
        auto draw = [&](int w, auto&& fn) {
            if (w == 3) {
                fn(-gap);
                fn(gap);
            } else if (w) {
                fn(0.0f);
            }
        };
        const float maxV = std::max(thick(b.left), thick(b.right)) * 0.5f;
        const float maxH = std::max(thick(b.up), thick(b.down)) * 0.5f;
        draw(b.up, [&](float d) { vertical(cell.y, cy + maxV, b.up == 3 ? 1 : b.up, d); });
        draw(b.down, [&](float d) { vertical(cy - maxV, cell.bottom(), b.down == 3 ? 1 : b.down, d); });
        draw(b.left, [&](float d) { horizontal(cell.x, cx + maxH, b.left == 3 ? 1 : b.left, d); });
        draw(b.right, [&](float d) { horizontal(cx - maxH, cell.x + m_cellW, b.right == 3 ? 1 : b.right, d); });
        return true;
    }
    if (cp >= 0x256D && cp <= 0x2570) {  // rounded corners ╭╮╯╰
        const float t = std::max(px, std::round(m_cellW * 0.09f * s) / s);
        const float cx = std::round((cell.x + m_cellW * 0.5f) * s) / s;
        const float cy = std::round((cell.y + cell.height * 0.5f) * s) / s;
        const float r = std::min(m_cellW, cell.height) * 0.5f;
        constexpr float kPi = 3.14159265f;
        const bool rightSide = cp == 0x256D || cp == 0x2570;  // the line continues to the right
        const bool down = cp == 0x256D || cp == 0x256E;        // ... and downwards
        const Point center{cx + (rightSide ? r : -r), cy + (down ? r : -r)};
        const float start = cp == 0x256D ? kPi : cp == 0x256E ? 1.5f * kPi : cp == 0x256F ? 0.0f : 0.5f * kPi;
        p.drawArc(center, r, start, kPi * 0.5f, color, t);
        if (rightSide) p.fillRect({center.x, cy - t * 0.5f, cell.x + m_cellW - center.x, t}, color);
        else p.fillRect({cell.x, cy - t * 0.5f, center.x - cell.x, t}, color);
        if (down) p.fillRect({cx - t * 0.5f, center.y, t, cell.bottom() - center.y}, color);
        else p.fillRect({cx - t * 0.5f, cell.y, t, center.y - cell.y}, color);
        return true;
    }
    if (cp >= 0x2580 && cp <= 0x259F) {
        const float w = m_cellW;
        const float h = cell.height;
        auto rect = [&](float x0, float y0, float x1, float y1, float alpha = 1) {
            p.fillRect(Rect::fromLTRB(cell.x + w * x0, cell.y + h * y0, cell.x + w * x1, cell.y + h * y1), color.multipliedAlpha(alpha));
        };
        if (cp == 0x2580) rect(0, 0, 1, 0.5f);
        else if (cp >= 0x2581 && cp <= 0x2588) rect(0, 1 - static_cast<float>(cp - 0x2580) / 8.0f, 1, 1);
        else if (cp >= 0x2589 && cp <= 0x258F) rect(0, 0, static_cast<float>(0x2590 - cp) / 8.0f, 1);
        else if (cp == 0x2590) rect(0.5f, 0, 1, 1);
        else if (cp == 0x2591) rect(0, 0, 1, 1, 0.25f);
        else if (cp == 0x2592) rect(0, 0, 1, 1, 0.5f);
        else if (cp == 0x2593) rect(0, 0, 1, 1, 0.75f);
        else if (cp == 0x2594) rect(0, 0, 1, 0.125f);
        else if (cp == 0x2595) rect(0.875f, 0, 1, 1);
        else {
            // Quadrants: bit 0 upper left, 1 upper right, 2 lower left, 3 lower right.
            static constexpr std::uint8_t kQuads[] = {0b0100, 0b1000, 0b0001, 0b1101, 0b1001, 0b0111, 0b1011, 0b0010, 0b0110, 0b1110};
            const std::uint8_t q = kQuads[cp - 0x2596];
            if (q & 1) rect(0, 0, 0.5f, 0.5f);
            if (q & 2) rect(0.5f, 0, 1, 0.5f);
            if (q & 4) rect(0, 0.5f, 0.5f, 1);
            if (q & 8) rect(0.5f, 0.5f, 1, 1);
        }
        return true;
    }
    if (cp >= 0x2800 && cp <= 0x28FF) {  // braille (graphs in btop, gping...)
        const unsigned bits = cp - 0x2800;
        static constexpr int kDot[8][2] = {{0, 0}, {0, 1}, {0, 2}, {1, 0}, {1, 1}, {1, 2}, {0, 3}, {1, 3}};
        const float dw = m_cellW / 2;
        const float dh = cell.height / 4;
        const float r = std::max(px, std::min(dw, dh) * 0.3f);
        for (int i = 0; i < 8; ++i) {
            if (bits & (1u << i)) p.fillCircle({cell.x + dw * (static_cast<float>(kDot[i][0]) + 0.5f), cell.y + dh * (static_cast<float>(kDot[i][1]) + 0.5f)}, r, color);
        }
        return true;
    }
    if (cp >= 0xE0B0 && cp <= 0xE0B3) {  // powerline separators
        const float w = m_cellW;
        const float h = cell.height;
        const Point tl{cell.x, cell.y}, bl{cell.x, cell.y + h}, tr{cell.x + w, cell.y}, br{cell.x + w, cell.y + h};
        const Point mr{cell.x + w, cell.y + h * 0.5f}, ml{cell.x, cell.y + h * 0.5f};
        if (cp == 0xE0B0) p.fillTriangle(tl, mr, bl, color);
        else if (cp == 0xE0B2) p.fillTriangle(tr, br, ml, color);
        else if (cp == 0xE0B1) {
            p.drawLine(tl, mr, color, std::max(px, w * 0.08f));
            p.drawLine(mr, bl, color, std::max(px, w * 0.08f));
        } else {
            p.drawLine(tr, ml, color, std::max(px, w * 0.08f));
            p.drawLine(ml, br, color, std::max(px, w * 0.08f));
        }
        return true;
    }
    return false;
}

void TerminalView::draw(Painter& p) {
    if (std::abs(p.scale() - m_scale) > 0.001f) {
        m_drawScale = p.scale();
        updateMetrics(p.scale());
        m_origin = {std::round(m_origin.x * m_scale) / m_scale, std::round(m_origin.y * m_scale) / m_scale};
        updateGrid();
    }
    const Theme& t = theme();
    const Rect r = frame();
    stepScroll(now());
    const Color paper = m_screen.reverseVideo() ? m_profile.foreground : m_profile.background;
    p.fillRect(r, paper);
    p.pushClip(r);

    const double tnow = now();
    const bool keyFocus = isFocused() && host() && host()->isKeyWindow();
    // Cursor: the program's DECSCUSR choice wins over the preference.
    const bool appStyle = m_screen.cursorStyleOverridden();
    const CursorShape shape = appStyle ? m_screen.cursorShape() : static_cast<CursorShape>(m_config->cursorShape);
    const bool blinks = appStyle ? m_screen.cursorBlinks() : m_config->cursorBlink;
    bool cursorOn = m_screen.cursorVisible() && m_scrollOffset == 0 && !m_exited;
    if (cursorOn && keyFocus && blinks) {
        const double elapsed = tnow - m_blinkStart;
        if (elapsed < kBlinkTimeout) {
            const auto phase = static_cast<long>(elapsed / kBlinkPeriod);
            cursorOn = phase % 2 == 0;
            if (auto* h = host()) h->scheduleFrame(m_blinkStart + static_cast<double>(phase + 1) * kBlinkPeriod);
        }
    }
    const int cursorRow = m_screen.cursorY();
    const int cursorCol = m_screen.cursorX();
    const bool solidCursor = cursorOn && keyFocus && shape == CursorShape::Block;

    const std::int64_t top = topLine();
    const int rows = m_screen.rows();
    const int cols = m_screen.cols();
    Pos selStart;
    Pos selEnd;
    if (m_selection.active) selectionBounds(selStart, selEnd);
    auto isSel = [&](std::int64_t line, int col) {
        if (!m_selection.active) return false;
        const Pos q{line, col};
        return !(q < selStart) && !(selEnd < q);
    };
    const Cell blank{};
    const LineMetrics lm = fontMetrics(m_font);
    const float px = 1.0f / m_scale;
    const float stroke = std::max(px, std::round(m_font.size * 0.07f * m_scale) / m_scale);

    struct Colors {
        Color fg;
        Color bg;
    };
    auto cellColors = [&](const Cell& cell, std::int64_t line, int col, bool cursorHere) {
        const std::uint16_t a = cell.attrs();
        Color fg = resolve(cell.fg, true, a);
        Color bg = resolve(cell.bg, false, a);
        if (a & kInverse) std::swap(fg, bg);
        if (a & kDim) fg = fg.mix(bg, 0.45f);
        if (a & kHidden) fg = bg;
        if (isSel(line, col)) bg = keyFocus ? m_profile.selection : m_profile.selection.mix(paper, 0.4f);
        if (cursorHere && solidCursor) {
            fg = bg;  // the character shows through the block in the background colour
            bg = m_profile.cursor;
        }
        return Colors{fg, bg};
    };

    char buf[4];
    // While scrolling smoothly, a partial row shows above or below the grid.
    const int firstRow = m_scrollShift > 0 ? -1 : 0;
    const int lastRow = m_scrollShift < 0 ? rows : rows - 1;
    for (int row = firstRow; row <= lastRow; ++row) {
        const std::int64_t global = top + row;
        const Line* line = lineGlobal(global);
        if (!line) continue;
        const float y = m_origin.y + static_cast<float>(row) * m_cellH + m_scrollShift;
        const bool cursorLine = m_scrollOffset == 0 && row == cursorRow;
        // Backgrounds, merged into runs.
        int runStart = 0;
        Color runColor = paper;
        auto flush = [&](int end) {
            if (end > runStart && !(runColor == paper)) {
                p.fillRect({m_origin.x + static_cast<float>(runStart) * m_cellW, y, static_cast<float>(end - runStart) * m_cellW, m_cellH}, runColor);
            }
        };
        for (int col = 0; col < cols; ++col) {
            const Cell& cell = col < static_cast<int>(line->cells.size()) ? line->cells[static_cast<size_t>(col)] : blank;
            const Colors c = cellColors(cell, global, col, cursorLine && col == cursorCol);
            if (!(c.bg == runColor)) {
                flush(col);
                runStart = col;
                runColor = c.bg;
            }
        }
        flush(cols);
        // Text and decorations.
        const int count = std::min(cols, static_cast<int>(line->cells.size()));
        for (int col = 0; col < count; ++col) {
            const Cell& cell = line->cells[static_cast<size_t>(col)];
            const std::uint16_t a = cell.attrs();
            if (a & kWideSpacer) continue;
            const char32_t cp = cell.codepoint();
            const bool decorated = a & (kUnderline | kDoubleUnderline | kStrike);
            if ((cp == ' ' || cp == 0) && !decorated) continue;
            const Colors c = cellColors(cell, global, col, cursorLine && col == cursorCol);
            const float x = m_origin.x + static_cast<float>(col) * m_cellW;
            const float w = m_cellW * ((a & kWide) ? 2.0f : 1.0f);
            if (cp != ' ' && cp != 0 && !(a & kHidden)) {
                if (!drawSpecial(p, cp, {x, y, w, m_cellH}, c.fg)) {
                    Font f = m_font;
                    if (a & kBold) f = f.withWeight(FontWeight::Bold);
                    if (a & kItalic) f = f.withItalic();
                    p.drawText(std::string_view(buf, encodeUtf8(cp, buf)), {x, y + m_baseline}, f, c.fg);
                }
            }
            if (a & (kUnderline | kDoubleUnderline)) {
                const float uy = std::round((y + m_baseline + std::max(stroke, lm.descender * 0.35f)) * m_scale) / m_scale;
                p.fillRect({x, uy, w, stroke}, c.fg);
                if (a & kDoubleUnderline) p.fillRect({x, uy + stroke * 2, w, stroke}, c.fg);
            }
            if (a & kStrike) {
                const float sy = std::round((y + m_baseline - lm.capHeight * 0.4f) * m_scale) / m_scale;
                p.fillRect({x, sy, w, stroke}, c.fg);
            }
        }
    }

    // Cursor shapes other than the solid block, and the hollow block of an inactive terminal.
    if (cursorOn) {
        const Line* cl = lineGlobal(top + cursorRow);
        const bool wide = cl && cursorCol < static_cast<int>(cl->cells.size()) && (cl->cells[static_cast<size_t>(cursorCol)].attrs() & kWide);
        const Rect cell{m_origin.x + static_cast<float>(cursorCol) * m_cellW, m_origin.y + static_cast<float>(cursorRow) * m_cellH + m_scrollShift,
                        m_cellW * (wide ? 2.0f : 1.0f), m_cellH};
        const float thick = std::max(px * 2, std::round(m_cellW * 0.14f * m_scale) / m_scale);
        if (!keyFocus) {
            p.strokeRoundedRect(cell, 0, m_profile.cursor, std::max(px, 1.0f));
        } else if (shape == CursorShape::Underline) {
            p.fillRect({cell.x, cell.bottom() - thick, cell.width, thick}, m_profile.cursor);
        } else if (shape == CursorShape::Bar) {
            p.fillRect({cell.x, cell.y, thick, cell.height}, m_profile.cursor);
        }
        if (cell != m_imeRect) {
            m_imeRect = cell;
            if (keyFocus && host()) host()->setTextInput(true, cell);
        }
    }

    // Overlay scrollbar while browsing the history, fading out after scrolling stops.
    const int history = m_screen.scrollbackSize();
    const double sinceScroll = tnow - m_lastScroll;
    if (history > 0 && (m_scrollOffset > 0 || sinceScroll < 1.2)) {
        const float alpha = m_scrollOffset > 0 ? 1.0f : static_cast<float>(std::clamp((1.2 - sinceScroll) / 0.4, 0.0, 1.0));
        const Rect track{r.right() - 9, r.y + 4, 5, r.height - 8};
        const float total = static_cast<float>(history + rows);
        const float thumbH = std::max(24.0f, track.height * static_cast<float>(rows) / total);
        const float frac = std::clamp(m_physics.offset().y / std::max(1.0f, m_physics.maxOffset().y), 0.0f, 1.0f);
        const Rect thumb{track.x, track.y + (track.height - thumbH) * frac, track.width, thumbH};
        p.fillRoundedRect(thumb, 2.5f, m_profile.foreground.withAlpha(0.35f * alpha));
        if (m_scrollOffset == 0) requestAnimationFrame();
    }
    if (const float bell = sample(m_bell); bell > 0.001f) p.fillRect(r, m_profile.foreground.withAlpha(0.12f * bell));
    if (m_exited) {
        const Font f = t.font(TextStyle::Callout);
        const Rect band{r.x, r.bottom() - 26, r.width, 22};
        p.drawText("[进程已结束]", band, f, m_profile.foreground.withAlpha(0.55f), TextAlign::Center);
    }
    p.popClip();
}

std::string TerminalView::title() const {
    if (!m_screen.title().empty()) return m_screen.title();
    std::string fg = m_pty.foregroundProcess();
    return fg.empty() ? "Terminal" : fg;
}

}  // namespace term
