#pragma once

#include <cstdint>
#include <string>

#include "geometry.hpp"

namespace mayo {

enum class MouseButton : std::uint8_t { None, Left, Middle, Right };

/// Modifier key bit set. `Primary` is Ctrl on Linux (Cmd on macOS later).
enum class Modifiers : std::uint8_t {
    None = 0,
    Shift = 1 << 0,
    Ctrl = 1 << 1,
    Alt = 1 << 2,
    Super = 1 << 3,
};
constexpr Modifiers operator|(Modifiers a, Modifiers b) {
    return static_cast<Modifiers>(static_cast<std::uint8_t>(a) | static_cast<std::uint8_t>(b));
}
constexpr bool hasModifier(Modifiers set, Modifiers m) {
    return (static_cast<std::uint8_t>(set) & static_cast<std::uint8_t>(m)) != 0;
}

enum class Key : std::uint16_t {
    Unknown,
    Enter, Escape, Backspace, Tab, Space, Delete, Insert,
    Left, Right, Up, Down, Home, End, PageUp, PageDown,
    A, B, C, D, E, F, G, H, I, J, K, L, M, N, O, P, Q, R, S, T, U, V, W, X, Y, Z,
    Num0, Num1, Num2, Num3, Num4, Num5, Num6, Num7, Num8, Num9,
    F1, F2, F3, F4, F5, F6, F7, F8, F9, F10, F11, F12,
    Comma, Period, Minus, Equals, Slash,
};

struct PointerEvent {
    Point position;              ///< window coordinates, in points
    MouseButton button = MouseButton::None;
    int clickCount = 0;          ///< 1 = single, 2 = double, ...
    Modifiers modifiers = Modifiers::None;
};

enum class ScrollPhase : std::uint8_t {
    Changed,  ///< movement (wheel notch or touchpad delta)
    Ended,    ///< the fingers left the touchpad (delta is zero); only sent when `reportsEnd` was set
};

struct ScrollEvent {
    Point position;
    Vec2 delta;                  ///< points; positive y scrolls content up (towards the end)
    bool precise = false;        ///< true for touchpads (continuous deltas), false for wheel notches
    Modifiers modifiers = Modifiers::None;
    ScrollPhase phase = ScrollPhase::Changed;
    /// The platform tells when this touchpad gesture ends (an Ended event follows), so widgets don't have
    /// to guess from a pause in the events.
    bool reportsEnd = false;
};

/// Touchpad pinch. `scale` is the change since the previous event (>1 = zoom in).
struct MagnifyEvent {
    Point position;
    float scale = 1;
};

struct KeyEvent {
    Key key = Key::Unknown;
    Modifiers modifiers = Modifiers::None;
    bool repeat = false;

    [[nodiscard]] bool shift() const { return hasModifier(modifiers, Modifiers::Shift); }
    [[nodiscard]] bool primary() const { return hasModifier(modifiers, Modifiers::Ctrl); }
    [[nodiscard]] bool alt() const { return hasModifier(modifiers, Modifiers::Alt); }
};

struct TextInputEvent {
    std::string text;            ///< UTF-8
};

enum class CursorShape : std::uint8_t {
    Arrow, IBeam, PointingHand, ResizeHorizontal, ResizeVertical, NotAllowed,
    Crosshair, Move, ResizeNWSE, ResizeNESW,
};

}  // namespace mayo
