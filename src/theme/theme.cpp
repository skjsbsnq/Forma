#include "mayonnaise/theme.hpp"

#include <algorithm>
#include <cmath>
#include <iterator>

namespace mayo {

namespace {

// Light/dark pairs follow the macOS 26 system palette.
struct Pair {
    Color light;
    Color dark;
    [[nodiscard]] Color pick(bool isDark) const { return isDark ? dark : light; }
};

constexpr Pair kRed{Color::hex(0xFF383C), Color::hex(0xFF4245)};
constexpr Pair kOrange{Color::hex(0xFF8D28), Color::hex(0xFF9230)};
constexpr Pair kYellow{Color::hex(0xFFCC00), Color::hex(0xFFD600)};
constexpr Pair kGreen{Color::hex(0x34C759), Color::hex(0x30D158)};
constexpr Pair kMint{Color::hex(0x00C8B3), Color::hex(0x00DAC3)};
constexpr Pair kTeal{Color::hex(0x00C3D0), Color::hex(0x00D2E0)};
constexpr Pair kCyan{Color::hex(0x00C0E8), Color::hex(0x3CD3FE)};
constexpr Pair kBlue{Color::hex(0x0088FF), Color::hex(0x0091FF)};
constexpr Pair kIndigo{Color::hex(0x6155F5), Color::hex(0x6D7CFF)};
constexpr Pair kPurple{Color::hex(0xCB30E0), Color::hex(0xDB34F2)};
constexpr Pair kPink{Color::hex(0xFF2D55), Color::hex(0xFF375F)};
constexpr Pair kBrown{Color::hex(0xAC7F5E), Color::hex(0xB78A66)};
constexpr Pair kGray{Color::hex(0x8E8E93), Color::hex(0x8E8E93)};

}  // namespace

Font Theme::font(TextStyle style) const {
    switch (style) {
        case TextStyle::LargeTitle: return {26, FontWeight::Bold, 32};
        case TextStyle::Title1: return {22, FontWeight::Bold, 28};
        case TextStyle::Title2: return {17, FontWeight::Semibold, 22};
        case TextStyle::Title3: return {15, FontWeight::Semibold, 20};
        case TextStyle::Headline: return {13, FontWeight::Semibold, 16};
        case TextStyle::Body: return {13, FontWeight::Regular, 16};
        case TextStyle::Callout: return {12, FontWeight::Regular, 15};
        case TextStyle::Subheadline: return {11, FontWeight::Regular, 14};
        case TextStyle::Footnote: return {10, FontWeight::Regular, 13};
        case TextStyle::Caption: return {10, FontWeight::Medium, 13};
    }
    return {};
}

MaterialStyle Theme::material(Material m) const {
    const bool dark = isDark();
    // One optical surface for every glass component. Roles vary only the frost
    // footprint, so menus and submenus cannot silently fall back to a flat fill.
    MaterialStyle s;
    s.blurRadius = 8;
    s.saturation = 1.3f;
    s.noise = 0.003f;
    s.tint = Color::white(0.04f);
    s.highlight = Color::transparent();
    s.border = dark ? Color::white(0.16f) : Color::black(0.09f);
    s.bezelWidth = 12;
    s.refraction = 8;
    s.rimStrength = dark ? 0.22f : 0.32f;
    // Keep backdrop contrast: lifting black all the way to 0.5 erased the
    // frost on pale file lists, even though the blur pass was running.
    s.toneLow = dark ? 0.07f : 0.22f;
    s.toneHigh = dark ? 0.42f : 0.97f;
    switch (m) {
        case Material::Sidebar:
            s.blurRadius = 24;
            break;
        case Material::Menu:
        case Material::Popover:
            s.blurRadius = 12;
            break;
        case Material::Titlebar:
            s.blurRadius = 20;
            // A continuous scroll surface has no enclosing panel outline.
            s.border = Color::transparent();
            break;
        case Material::Glass:
            break;
        case Material::GlassClear:
            s.blurRadius = 1.5f;
            s.toneLow = dark ? 0.02f : 0.08f;
            s.toneHigh = dark ? 0.92f : 1.0f;
            break;
    }
    return s;
}

Theme Theme::make(Appearance appearance, Color accent) {
    Theme t;
    t.appearance = appearance;
    const bool dark = appearance == Appearance::Dark;
    ThemeColors& c = t.colors;

    // The default accent has a slightly brighter dark-mode variant, like systemBlue.
    if (accent == kBlue.light && dark) accent = kBlue.dark;
    c.accent = accent;
    c.accentHover = accent.lighter(0.06f);
    c.accentPressed = accent.lighter(-0.14f);
    c.onAccent = Color::white();

    const Color ink = dark ? Color::white() : Color::black();
    c.label = ink.withAlpha(0.85f);
    c.secondaryLabel = ink.withAlpha(dark ? 0.55f : 0.50f);
    c.tertiaryLabel = ink.withAlpha(0.26f);
    c.quaternaryLabel = ink.withAlpha(0.10f);
    c.placeholder = ink.withAlpha(dark ? 0.30f : 0.28f);

    if (dark) {
        c.windowBackground = Color::hex(0x232326);
        c.contentBackground = Color::hex(0x1C1C1E);
        c.controlBackground = Color::white(0.06f);
        c.control = Color::white(0.14f);
        c.controlHover = Color::white(0.18f);
        c.controlPressed = Color::white(0.24f);
        c.controlBorder = Color::white(0.07f);
        c.controlShadow = Color::black(0.35f);
        c.toggleOff = Color::white(0.16f);
        c.knob = Color::hex(0xE8E8EA);
        c.knobBorder = Color::black(0.25f);
        c.shadow = Color::black(0.50f);
        c.windowBorder = Color::black(0.70f);
        c.windowHighlight = Color::white(0.13f);
    } else {
        c.windowBackground = Color::hex(0xF5F5F7);
        c.contentBackground = Color::white();
        c.controlBackground = Color::white();
        c.control = Color::white();
        c.controlHover = Color::white();
        c.controlPressed = Color::hex(0xE4E4E7);
        c.controlBorder = Color::black(0.06f);
        c.controlShadow = Color::black(0.16f);
        c.toggleOff = Color::black(0.09f);
        c.knob = Color::white();
        c.knobBorder = Color::black(0.06f);
        c.shadow = Color::black(0.22f);
        c.windowBorder = Color::black(0.16f);
        c.windowHighlight = Color::white(0.55f);
    }

    c.fill = ink.withAlpha(dark ? 0.12f : 0.09f);
    c.secondaryFill = ink.withAlpha(dark ? 0.08f : 0.06f);
    c.tertiaryFill = ink.withAlpha(0.04f);
    c.separator = ink.withAlpha(dark ? 0.12f : 0.10f);
    c.focusRing = accent.withAlpha(0.5f);
    c.textSelection = accent.withAlpha(dark ? 0.40f : 0.28f);
    c.selection = accent;
    c.unemphasizedSelection = ink.withAlpha(dark ? 0.12f : 0.08f);
    c.hover = ink.withAlpha(dark ? 0.07f : 0.05f);
    c.destructive = kRed.pick(dark);

    c.red = kRed.pick(dark);
    c.orange = kOrange.pick(dark);
    c.yellow = kYellow.pick(dark);
    c.green = kGreen.pick(dark);
    c.mint = kMint.pick(dark);
    c.teal = kTeal.pick(dark);
    c.cyan = kCyan.pick(dark);
    c.blue = kBlue.pick(dark);
    c.indigo = kIndigo.pick(dark);
    c.purple = kPurple.pick(dark);
    c.pink = kPink.pick(dark);
    c.brown = kBrown.pick(dark);
    c.gray = kGray.pick(dark);
    return t;
}

namespace {

constexpr AccentChoice kAccents[] = {
    {"Blue", Theme::defaultAccent()}, {"Purple", Color::hex(0xA550A7)},   {"Pink", Color::hex(0xF74F9E)},
    {"Red", Color::hex(0xFF5257)},    {"Orange", Color::hex(0xF7821B)},   {"Yellow", Color::hex(0xFFC600)},
    {"Green", Color::hex(0x62BA46)},  {"Graphite", Color::hex(0x8C8C8C)},
};

/// Hue in degrees and saturation (HSV).
void hueSat(Color c, float& hue, float& sat) {
    const float mx = std::max({c.r, c.g, c.b});
    const float mn = std::min({c.r, c.g, c.b});
    const float d = mx - mn;
    sat = mx > 0 ? d / mx : 0;
    if (d <= 0) {
        hue = 0;
    } else if (mx == c.r) {
        hue = 60 * std::fmod((c.g - c.b) / d + 6, 6.0f);
    } else if (mx == c.g) {
        hue = 60 * ((c.b - c.r) / d + 2);
    } else {
        hue = 60 * ((c.r - c.g) / d + 4);
    }
}

}  // namespace

std::span<const AccentChoice> accentChoices() { return kAccents; }

const AccentChoice& nearestAccent(Color c) {
    float hue = 0;
    float sat = 0;
    hueSat(c, hue, sat);
    const AccentChoice& graphite = kAccents[std::size(kAccents) - 1];
    if (sat < 0.22f) return graphite;
    const AccentChoice* best = &kAccents[0];
    float bestDist = 1e9f;
    for (const auto& a : kAccents) {
        if (&a == &graphite) continue;
        float h = 0;
        float s = 0;
        hueSat(a.color, h, s);
        float d = std::abs(h - hue);
        d = std::min(d, 360 - d);
        if (d < bestDist) {
            bestDist = d;
            best = &a;
        }
    }
    return *best;
}

}  // namespace mayo
