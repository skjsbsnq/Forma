#pragma once

#include <cstdint>
#include <span>

#include "color.hpp"
#include "geometry.hpp"

namespace mayo {

enum class Appearance : std::uint8_t { Light, Dark };

/// The macOS type scale.
enum class TextStyle : std::uint8_t {
    LargeTitle, Title1, Title2, Title3, Headline, Body, Callout, Subheadline, Footnote, Caption,
};

enum class FontWeight : std::uint16_t {
    Thin = 200,
    Light = 300,
    Regular = 400,
    Medium = 500,
    Semibold = 600,
    Bold = 700,
};

/// Font family id: 0 is the built-in Inter; others come from registerFontFamily() / systemFontFamily()
/// (<mayonnaise/font.hpp>).
using FontFamily = std::uint16_t;

struct Font {
    float size = 13;                       ///< points
    FontWeight weight = FontWeight::Regular;
    float lineHeight = 16;                 ///< points, baseline to baseline
    FontFamily family = 0;
    bool italic = false;                   ///< synthesized (slanted) when the family has no italic face

    [[nodiscard]] Font withWeight(FontWeight w) const { return {size, w, lineHeight, family, italic}; }
    [[nodiscard]] Font withSize(float s) const { return {s, weight, lineHeight * s / size, family, italic}; }
    [[nodiscard]] Font withFamily(FontFamily f) const { return {size, weight, lineHeight, f, italic}; }
    [[nodiscard]] Font withItalic(bool i = true) const { return {size, weight, lineHeight, family, i}; }
    constexpr bool operator==(const Font&) const = default;
};

/// Translucent backdrop materials (blurred, tinted "glass").
enum class Material : std::uint8_t {
    Sidebar,     ///< floating sidebar glass
    Titlebar,    ///< toolbar / title bar over scrolled content
    Popover,     ///< popovers and tooltips
    Menu,        ///< menus
    Glass,       ///< generic Liquid Glass (cards, floating controls)
    GlassClear,  ///< lightly frosted glass over photos and video
};

enum class ScrollEdgeStyle : std::uint8_t { None, Soft, Hard };

struct MaterialStyle {
    float blurRadius = 24;    ///< points
    Color tint;               ///< laid over the blurred backdrop
    float saturation = 1.6f;  ///< vibrancy: boosts color of what shows through
    float noise = 0.015f;     ///< subtle grain so large glass areas don't band
    Color highlight;          ///< specular rim along the top edge
    Color border;             ///< hairline around the shape
    float fadeBottom = 0;     ///< fraction of the height over which the material fades out at the bottom
    float bezelWidth = 0;     ///< lens edge width, in points; zero keeps the flat material
    float refraction = 0;     ///< maximum backdrop displacement, in points
    float rimStrength = 0;    ///< directional highlight along the lens edge
    float toneLow = 0;        ///< black/white mapping before tint (identity by default)
    float toneHigh = 1;
    float toneCurve = 1;
};

struct ThemeColors {
    Color accent;
    Color accentHover;
    Color accentPressed;
    Color onAccent;                ///< text and glyphs drawn on top of accent fills

    Color label;
    Color secondaryLabel;
    Color tertiaryLabel;
    Color quaternaryLabel;
    Color placeholder;

    Color windowBackground;
    Color contentBackground;       ///< lists, scrolling content
    Color controlBackground;       ///< text fields
    Color control;                 ///< push button face
    Color controlHover;
    Color controlPressed;
    Color controlBorder;
    Color controlShadow;

    Color fill;                    ///< neutral fills: tracks, wells
    Color secondaryFill;
    Color tertiaryFill;

    Color separator;
    Color focusRing;
    Color textSelection;
    Color selection;               ///< selected rows (focused)
    Color unemphasizedSelection;   ///< selected rows (unfocused / sidebar)
    Color hover;                   ///< row/borderless-button hover wash

    Color destructive;
    Color toggleOff;
    Color knob;                    ///< toggle and slider knobs
    Color knobBorder;

    Color shadow;
    Color windowBorder;
    Color windowHighlight;

    // Named system palette (for icons, badges, charts).
    Color red, orange, yellow, green, mint, teal, cyan, blue, indigo, purple, pink, brown, gray;
};

struct ThemeMetrics {
    float windowRadius = 16;
    float titlebarHeight = 52;     ///< unified title bar / toolbar height
    float trafficLightSize = 12;
    float trafficLightSpacing = 8;
    float trafficLightInset = 24;  ///< from the left window edge (clear of the sidebar panel's corner)

    float controlHeight = 24;
    float controlRadius = 7;
    float largeControlHeight = 30;
    float controlPaddingH = 12;
    float smallRadius = 5;
    float focusRingWidth = 3;
    float borderWidth = 0.5f;

    float sidebarWidth = 220;
    float sidebarInset = 8;        ///< gap between the floating sidebar and the window edge
    float sidebarRadius = 12;
    float rowHeight = 28;
    float rowRadius = 8;

    float menuRadius = 10;
    float menuItemHeight = 24;
    float popoverRadius = 14;

    float spacingSmall = 6;
    float spacing = 12;
    float spacingLarge = 20;

    float toggleWidth = 40;
    float toggleHeight = 22;
    float checkboxSize = 15;
    float sliderTrackHeight = 4;
    float sliderKnobWidth = 22;
    float sliderKnobHeight = 16;
    float scrollbarWidth = 7;

    float animationFast = 0.15f;   ///< seconds
    float animation = 0.25f;
};

struct Theme {
    Appearance appearance = Appearance::Light;
    ThemeColors colors;
    ThemeMetrics metrics;

    [[nodiscard]] Font font(TextStyle style) const;
    [[nodiscard]] MaterialStyle material(Material material) const;
    [[nodiscard]] bool isDark() const { return appearance == Appearance::Dark; }

    static Theme make(Appearance appearance, Color accent = defaultAccent());
    static constexpr Color defaultAccent() { return Color::hex(0x0088FF); }
};

/// A named accent colour from the macOS palette.
struct AccentChoice {
    const char* name;
    Color color;
};
/// Blue (the default), Purple, Pink, Red, Orange, Yellow, Green, Graphite.
std::span<const AccentChoice> accentChoices();
/// The palette entry closest to `c` in hue (greys map to Graphite). Desktop accent colours are snapped to
/// it so they keep the macOS tones.
const AccentChoice& nearestAccent(Color c);

}  // namespace mayo
