// Vector line icons on a 20x20 design grid, scaled into the target rect.
#include <cmath>
#include <numbers>

#include "mayonnaise/icon.hpp"
#include "mayonnaise/painter.hpp"

namespace mayo {

namespace {

constexpr float kPi = std::numbers::pi_v<float>;

struct Pen {
    Painter& p;
    Point origin;
    float u;
    float w;
    Color c;

    [[nodiscard]] Point at(float x, float y) const { return {origin.x + x * u, origin.y + y * u}; }
    void line(float x1, float y1, float x2, float y2) const { p.drawLine(at(x1, y1), at(x2, y2), c, w); }
    void poly(std::initializer_list<Point> pts, bool closed = false) const {
        const Point* prev = nullptr;
        for (const Point& pt : pts) {
            if (prev) line(prev->x, prev->y, pt.x, pt.y);
            prev = &pt;
        }
        if (closed && pts.size() > 2) line(prev->x, prev->y, pts.begin()->x, pts.begin()->y);
    }
    void circle(float cx, float cy, float r) const { p.drawArc(at(cx, cy), r * u, 0, 2 * kPi, c, w); }
    void arc(float cx, float cy, float r, float startDeg, float sweepDeg) const {
        p.drawArc(at(cx, cy), r * u, startDeg * kPi / 180.0f, sweepDeg * kPi / 180.0f, c, w);
    }
    void dot(float cx, float cy, float r) const { p.fillCircle(at(cx, cy), r * u, c); }
    void rrect(float x, float y, float rw, float rh, float r) const {
        p.strokeRoundedRect({origin.x + x * u - w * 0.5f, origin.y + y * u - w * 0.5f, rw * u + w, rh * u + w},
                            r * u + w * 0.5f, c, w);
    }
    void fillRect(float x, float y, float rw, float rh, float r) const {
        p.fillRoundedRect({origin.x + x * u, origin.y + y * u, rw * u, rh * u}, r * u, c);
    }
};

void drawStar(const Pen& pen) {
    Point pts[10];
    for (int i = 0; i < 10; ++i) {
        float a = -kPi / 2 + static_cast<float>(i) * kPi / 5;
        float r = i % 2 == 0 ? 8.2f : 3.6f;
        pts[i] = {10 + r * std::cos(a), 10.6f + r * std::sin(a)};
    }
    for (int i = 0; i < 10; ++i) {
        const Point& a = pts[i];
        const Point& b = pts[(i + 1) % 10];
        pen.line(a.x, a.y, b.x, b.y);
    }
}

}  // namespace

void Painter::drawIcon(Icon icon, const Rect& r, const Color& color, float weight) {
    const float side = std::min(r.width, r.height);
    const float u = side / 20.0f;
    Pen pen{*this, {r.center().x - side * 0.5f, r.center().y - side * 0.5f}, u, std::max(1.7f * u * weight, 1.0f / scale()),
            color};
    switch (icon) {
        case Icon::None: break;
        case Icon::Checkmark: pen.poly({{4.5f, 10.5f}, {8.2f, 14.5f}, {15.5f, 5.5f}}); break;
        case Icon::Xmark:
            pen.line(5, 5, 15, 15);
            pen.line(5, 15, 15, 5);
            break;
        case Icon::Plus:
            pen.line(10, 4, 10, 16);
            pen.line(4, 10, 16, 10);
            break;
        case Icon::Minus: pen.line(4, 10, 16, 10); break;
        case Icon::Ellipsis:
            pen.dot(4.5f, 10, 1.4f);
            pen.dot(10, 10, 1.4f);
            pen.dot(15.5f, 10, 1.4f);
            break;
        case Icon::ChevronLeft: pen.poly({{12.5f, 4}, {6.5f, 10}, {12.5f, 16}}); break;
        case Icon::ChevronRight: pen.poly({{7.5f, 4}, {13.5f, 10}, {7.5f, 16}}); break;
        case Icon::ChevronUp: pen.poly({{4, 12.5f}, {10, 6.5f}, {16, 12.5f}}); break;
        case Icon::ChevronDown: pen.poly({{4, 7.5f}, {10, 13.5f}, {16, 7.5f}}); break;
        case Icon::ChevronUpDown:
            pen.poly({{6, 7.5f}, {10, 3.5f}, {14, 7.5f}});
            pen.poly({{6, 12.5f}, {10, 16.5f}, {14, 12.5f}});
            break;
        case Icon::ArrowLeft:
            pen.line(16, 10, 4, 10);
            pen.poly({{9, 5}, {4, 10}, {9, 15}});
            break;
        case Icon::ArrowRight:
            pen.line(4, 10, 16, 10);
            pen.poly({{11, 5}, {16, 10}, {11, 15}});
            break;
        case Icon::ArrowUp:
            pen.line(10, 16, 10, 4);
            pen.poly({{5, 9}, {10, 4}, {15, 9}});
            break;
        case Icon::ArrowDown:
            pen.line(10, 4, 10, 16);
            pen.poly({{5, 11}, {10, 16}, {15, 11}});
            break;
        case Icon::Magnifier:
            pen.circle(8.5f, 8.5f, 5.5f);
            pen.line(12.6f, 12.6f, 16.5f, 16.5f);
            break;
        case Icon::Gear: {
            pen.circle(10, 10, 2.6f);
            pen.circle(10, 10, 5.6f);
            for (int i = 0; i < 8; ++i) {
                float a = static_cast<float>(i) * kPi / 4;
                pen.line(10 + 5.8f * std::cos(a), 10 + 5.8f * std::sin(a), 10 + 8.0f * std::cos(a),
                         10 + 8.0f * std::sin(a));
            }
            break;
        }
        case Icon::Sidebar:
            pen.rrect(2.5f, 4, 15, 12, 2.5f);
            pen.line(8, 4, 8, 16);
            pen.line(4.5f, 7, 6, 7);
            pen.line(4.5f, 9.5f, 6, 9.5f);
            break;
        case Icon::Info:
            pen.circle(10, 10, 8);
            pen.line(10, 9, 10, 14.5f);
            pen.dot(10, 6, 1.1f);
            break;
        case Icon::Bell:
            pen.arc(10, 8.5f, 4.8f, 180, 180);
            pen.line(5.2f, 8.5f, 5.2f, 12.5f);
            pen.line(14.8f, 8.5f, 14.8f, 12.5f);
            pen.line(3.5f, 14.5f, 16.5f, 14.5f);
            pen.line(5.2f, 12.5f, 3.5f, 14.5f);
            pen.line(14.8f, 12.5f, 16.5f, 14.5f);
            pen.arc(10, 15.5f, 2, 0, 180);
            break;
        case Icon::Person:
            pen.circle(10, 6.5f, 3.5f);
            pen.arc(10, 18, 6.5f, 180, 180);
            break;
        case Icon::PersonCircle:
            pen.circle(10, 10, 8.5f);
            pen.circle(10, 8, 2.8f);
            pen.arc(10, 17.5f, 5.5f, 210, 120);
            break;
        case Icon::Wifi:
            pen.arc(10, 15.5f, 11, 225, 90);
            pen.arc(10, 15.5f, 7, 225, 90);
            pen.arc(10, 15.5f, 3, 225, 90);
            pen.dot(10, 15.5f, 1.2f);
            break;
        case Icon::Bluetooth:
            pen.poly({{6, 6.5f}, {14, 13.5f}, {10, 17}, {10, 3}, {14, 6.5f}, {6, 13.5f}});
            break;
        case Icon::Globe:
            pen.circle(10, 10, 8);
            pen.line(2, 10, 18, 10);
            pen.p.strokeRoundedRect({pen.origin.x + 6.2f * u, pen.origin.y + 2 * u, 7.6f * u, 16 * u}, 3.8f * u, color,
                                    pen.w);
            break;
        case Icon::Moon:
            pen.arc(10, 10, 7, 60, 270);
            pen.arc(13.6f, 6.8f, 5.6f, 75, 135);
            break;
        case Icon::Sun:
            pen.circle(10, 10, 3.6f);
            for (int i = 0; i < 8; ++i) {
                float a = static_cast<float>(i) * kPi / 4;
                pen.line(10 + 6 * std::cos(a), 10 + 6 * std::sin(a), 10 + 8 * std::cos(a), 10 + 8 * std::sin(a));
            }
            break;
        case Icon::Lock:
            pen.rrect(4.5f, 9, 11, 8.5f, 2);
            pen.arc(10, 8.5f, 3.4f, 180, 180);
            pen.line(6.6f, 8.5f, 6.6f, 9);
            pen.line(13.4f, 8.5f, 13.4f, 9);
            break;
        case Icon::Paintbrush:
            // Handle, ferrule, and a filled bristle tip sweeping to a point.
            pen.line(16.5f, 3.5f, 10.8f, 9.2f);
            pen.p.drawLine(pen.at(10.8f, 9.2f), pen.at(9.4f, 10.6f), color, pen.w * 2.2f);
            pen.dot(7.4f, 12.6f, 2.6f);
            pen.p.drawLine(pen.at(6.2f, 14.2f), pen.at(3.4f, 16.8f), color, pen.w * 1.6f);
            break;
        case Icon::Display:
            pen.rrect(2.5f, 3.5f, 15, 10, 1.8f);
            pen.line(10, 13.5f, 10, 16.5f);
            pen.line(6.5f, 16.5f, 13.5f, 16.5f);
            break;
        case Icon::Battery:
            pen.rrect(2, 6.5f, 14, 7, 2);
            pen.fillRect(3.8f, 8.3f, 7, 3.4f, 0.8f);
            pen.line(18, 9, 18, 11);
            break;
        case Icon::Speaker:
            pen.poly({{3, 8}, {6, 8}, {10, 4.5f}, {10, 15.5f}, {6, 12}, {3, 12}}, true);
            pen.arc(10, 10, 3.5f, -50, 100);
            pen.arc(10, 10, 6.5f, -50, 100);
            break;
        case Icon::Heart:
            pen.arc(6.8f, 7.6f, 3.6f, 135, 225);
            pen.arc(13.2f, 7.6f, 3.6f, 180, 225);
            pen.line(4.25f, 10.15f, 10, 16);
            pen.line(15.75f, 10.15f, 10, 16);
            break;
        case Icon::Star: drawStar(pen); break;
        case Icon::Folder:
            pen.poly({{2.5f, 15.5f}, {2.5f, 5}, {7.5f, 5}, {9, 6.8f}, {17.5f, 6.8f}, {17.5f, 15.5f}}, true);
            pen.line(2.5f, 9, 17.5f, 9);
            break;
        case Icon::Document:
            pen.poly({{4.5f, 2.5f}, {11.5f, 2.5f}, {15.5f, 6.5f}, {15.5f, 17.5f}, {4.5f, 17.5f}}, true);
            pen.poly({{11.5f, 2.5f}, {11.5f, 6.5f}, {15.5f, 6.5f}});
            break;
        case Icon::Trash:
            pen.line(3, 5, 17, 5);
            pen.poly({{7.5f, 5}, {8, 2.8f}, {12, 2.8f}, {12.5f, 5}});
            pen.poly({{4.8f, 5}, {5.8f, 17}, {14.2f, 17}, {15.2f, 5}});
            pen.line(8.3f, 8, 8.5f, 14);
            pen.line(11.7f, 8, 11.5f, 14);
            break;
        case Icon::Share:
            pen.poly({{7, 7.5f}, {4.5f, 7.5f}, {4.5f, 17.5f}, {15.5f, 17.5f}, {15.5f, 7.5f}, {13, 7.5f}});
            pen.line(10, 2.5f, 10, 12);
            pen.poly({{7, 5.5f}, {10, 2.5f}, {13, 5.5f}});
            break;
        case Icon::House:
            pen.poly({{2.5f, 9.5f}, {10, 3}, {17.5f, 9.5f}});
            pen.poly({{4.5f, 8}, {4.5f, 17}, {15.5f, 17}, {15.5f, 8}});
            pen.poly({{8.2f, 17}, {8.2f, 12}, {11.8f, 12}, {11.8f, 17}});
            break;
        case Icon::Clock:
            pen.circle(10, 10, 8);
            pen.poly({{10, 5}, {10, 10}, {13.5f, 12}});
            break;
        case Icon::Hand:
            pen.rrect(5, 8, 10, 9.5f, 4);
            pen.line(7.3f, 8.5f, 7.3f, 4);
            pen.line(10, 8, 10, 2.8f);
            pen.line(12.7f, 8.5f, 12.7f, 3.8f);
            break;
        case Icon::Keyboard:
            pen.rrect(2, 5, 16, 10, 2);
            for (int i = 0; i < 5; ++i) pen.dot(5 + static_cast<float>(i) * 2.5f, 8.3f, 0.7f);
            pen.line(6.5f, 12, 13.5f, 12);
            break;
        case Icon::Photo:
            pen.rrect(2.5f, 4, 15, 12, 2);
            pen.poly({{2.5f, 14}, {7.5f, 9}, {11, 12.5f}, {13, 10.5f}, {17.5f, 15}});
            pen.dot(13.5f, 7.5f, 1.3f);
            break;
        case Icon::Square: pen.rrect(3, 3, 14, 14, 3); break;
        case Icon::Circle: pen.circle(10, 10, 7.5f); break;
        case Icon::Grid:
            pen.rrect(3, 3, 5.5f, 5.5f, 1.4f);
            pen.rrect(11.5f, 3, 5.5f, 5.5f, 1.4f);
            pen.rrect(3, 11.5f, 5.5f, 5.5f, 1.4f);
            pen.rrect(11.5f, 11.5f, 5.5f, 5.5f, 1.4f);
            break;
        case Icon::List:
            for (int i = 0; i < 3; ++i) {
                float y = 5 + static_cast<float>(i) * 5;
                pen.dot(4, y, 1.1f);
                pen.line(7.5f, y, 17, y);
            }
            break;
        case Icon::Bolt: pen.poly({{11.5f, 2}, {4.5f, 11.5f}, {9.5f, 11.5f}, {8.5f, 18}, {15.5f, 8.5f}, {10.5f, 8.5f}}, true); break;
        case Icon::Cloud:
            pen.arc(7, 11.5f, 3.5f, 90, 180);
            pen.arc(10.5f, 9, 4.5f, 190, 150);
            pen.arc(14.5f, 12, 3, 280, 170);
            pen.line(7, 15, 14.5f, 15);
            break;
        case Icon::Envelope:
            pen.rrect(2.5f, 4.5f, 15, 11, 2);
            pen.poly({{3, 5.5f}, {10, 11}, {17, 5.5f}});
            break;
        case Icon::Calendar:
            pen.rrect(2.5f, 4, 15, 13, 2.2f);
            pen.line(2.5f, 8, 17.5f, 8);
            pen.line(6.5f, 2.5f, 6.5f, 5);
            pen.line(13.5f, 2.5f, 13.5f, 5);
            break;
        case Icon::HeartFill: {
            // Two lobes plus a scanline-filled wedge (no polygon primitive needed at icon sizes).
            pen.dot(6.6f, 7.8f, 4.0f);
            pen.dot(13.4f, 7.8f, 4.0f);
            const float step = 0.5f;
            for (float y = 7.8f; y <= 16.6f; y += step) {
                float t = (y - 7.8f) / (16.6f - 7.8f);
                float half = 7.6f * (1.0f - t);
                pen.p.drawLine(pen.at(10 - half, y), pen.at(10 + half, y), color, step * u * 1.6f);
            }
            break;
        }
        case Icon::Import:
            pen.line(10, 2.5f, 10, 12.5f);
            pen.poly({{6.5f, 9}, {10, 12.5f}, {13.5f, 9}});
            pen.poly({{3, 12}, {3, 17}, {17, 17}, {17, 12}});
            break;
        case Icon::ZoomIn:
        case Icon::ZoomOut:
            pen.circle(8.5f, 8.5f, 5.8f);
            pen.line(12.8f, 12.8f, 17, 17);
            pen.line(6, 8.5f, 11, 8.5f);
            if (icon == Icon::ZoomIn) pen.line(8.5f, 6, 8.5f, 11);
            break;
        case Icon::Play: pen.p.fillTriangle(pen.at(6, 3.5f), pen.at(16.5f, 10), pen.at(6, 16.5f), color, 1.2f * u); break;
        case Icon::Pause:
            pen.fillRect(4.8f, 3.5f, 3.6f, 13, 1.2f);
            pen.fillRect(11.6f, 3.5f, 3.6f, 13, 1.2f);
            break;
        case Icon::Backward:
        case Icon::Forward: {
            // Two touching triangles (⏪ / ⏩).
            const bool fwd = icon == Icon::Forward;
            auto tri = [&](float x0, float x1) {
                if (!fwd) std::swap(x0, x1);
                pen.p.fillTriangle(pen.at(x0, 5), pen.at(x1, 10), pen.at(x0, 15), color, 0.8f * u);
            };
            tri(2.5f, 10);
            tri(10, 17.5f);
            break;
        }
        case Icon::SkipBack:
        case Icon::SkipForward: {
            // Open circle with an arrowhead at the top and "10" inside (jump back / forward 10 s).
            const bool fwd = icon == Icon::SkipForward;
            // The back glyph mirrors the forward one: angle θ maps to 180° − θ.
            pen.arc(10, 10.5f, 7.2f, fwd ? -50.0f : -80.0f, 310.0f);
            const float tipX = fwd ? 12.6f : 7.4f;
            const float dir = fwd ? -1.0f : 1.0f;
            pen.p.fillTriangle(pen.at(tipX + dir * 3.6f, 0.9f), pen.at(tipX + dir * 3.6f, 5.7f), pen.at(tipX, 3.3f), color,
                               0.25f * u);
            Font f{8.2f * u, FontWeight::Semibold, 9.0f * u};
            drawText("10", Rect{pen.origin.x, pen.origin.y + 6.0f * u, 20 * u, 9.5f * u}, f, color, TextAlign::Center);
            break;
        }
        case Icon::Expand:
            pen.poly({{3.5f, 8}, {3.5f, 3.5f}, {8, 3.5f}});
            pen.poly({{12, 16.5f}, {16.5f, 16.5f}, {16.5f, 12}});
            pen.line(3.5f, 3.5f, 8.5f, 8.5f);
            pen.line(16.5f, 16.5f, 11.5f, 11.5f);
            break;
        case Icon::Contract:
            pen.poly({{4, 8.5f}, {8.5f, 8.5f}, {8.5f, 4}});
            pen.poly({{11.5f, 16}, {11.5f, 11.5f}, {16, 11.5f}});
            pen.line(3, 3, 8.5f, 8.5f);
            pen.line(17, 17, 11.5f, 11.5f);
            break;
        case Icon::SpeakerSlash:
            pen.poly({{3, 8}, {6, 8}, {10, 4.5f}, {10, 15.5f}, {6, 12}, {3, 12}}, true);
            pen.line(12.5f, 7.5f, 17.5f, 12.5f);
            pen.line(17.5f, 7.5f, 12.5f, 12.5f);
            break;
        case Icon::Film:
            pen.rrect(3, 2.5f, 14, 15, 2);
            pen.line(6.5f, 2.5f, 6.5f, 17.5f);
            pen.line(13.5f, 2.5f, 13.5f, 17.5f);
            pen.line(3, 10, 6.5f, 10);
            pen.line(13.5f, 10, 17, 10);
            pen.line(3, 6.2f, 6.5f, 6.2f);
            pen.line(3, 13.8f, 6.5f, 13.8f);
            pen.line(13.5f, 6.2f, 17, 6.2f);
            pen.line(13.5f, 13.8f, 17, 13.8f);
            break;
        case Icon::Repeat:
            pen.poly({{4, 9}, {4, 6}, {15, 6}});
            pen.poly({{12.5f, 3.5f}, {15, 6}, {12.5f, 8.5f}});
            pen.poly({{16, 11}, {16, 14}, {5, 14}});
            pen.poly({{7.5f, 11.5f}, {5, 14}, {7.5f, 16.5f}});
            break;
        case Icon::ArrowDownCircle:
            pen.circle(10, 10, 7.8f);
            pen.line(10, 5.6f, 10, 14);
            pen.poly({{6.6f, 10.8f}, {10, 14.2f}, {13.4f, 10.8f}});
            break;
        case Icon::MusicNote:
            pen.line(7.6f, 14.5f, 7.6f, 4.8f);
            pen.line(15.4f, 12.8f, 15.4f, 3.2f);
            pen.p.drawLine(pen.at(7.6f, 4.6f), pen.at(15.4f, 3.0f), color, pen.w * 1.6f);
            pen.dot(5.7f, 14.8f, 2.25f);
            pen.dot(13.5f, 13.1f, 2.25f);
            break;
        case Icon::HardDrive:
            pen.rrect(2.5f, 5.5f, 15, 9, 2.2f);
            pen.line(2.5f, 10.8f, 17.5f, 10.8f);
            pen.dot(14.6f, 12.7f, 0.9f);
            break;
        case Icon::UsbDrive:
            pen.rrect(6.5f, 2, 7, 5, 0.6f);
            pen.line(8.5f, 3.5f, 8.5f, 5);
            pen.line(11.5f, 3.5f, 11.5f, 5);
            pen.rrect(5, 7, 10, 11, 2);
            pen.line(8.5f, 14.5f, 11.5f, 14.5f);
            break;
        case Icon::Eject:
            pen.poly({{10, 4}, {16, 11}, {4, 11}}, true);
            pen.line(4, 15, 16, 15);
            break;
        case Icon::Columns:
            pen.rrect(2.5f, 4, 15, 12, 2);
            pen.line(7.5f, 4, 7.5f, 16);
            pen.line(12.5f, 4, 12.5f, 16);
            break;
        case Icon::Tag:
            pen.poly({{3, 4.5f}, {3, 10}, {10.5f, 17.5f}, {17.5f, 10.5f}, {10, 3}, {4.5f, 3}}, true);
            pen.dot(6.8f, 6.8f, 1.2f);
            break;
        case Icon::DocOnDoc:
            pen.poly({{3.5f, 13.5f}, {3.5f, 4.5f}, {5, 3}, {12.5f, 3}});
            pen.rrect(6.5f, 6, 10, 11.5f, 1.8f);
            break;
        case Icon::FolderPlus:
            pen.poly({{2.5f, 15.5f}, {2.5f, 5}, {7.5f, 5}, {9, 6.8f}, {17.5f, 6.8f}, {17.5f, 15.5f}}, true);
            pen.line(10, 9.5f, 10, 13.5f);
            pen.line(8, 11.5f, 12, 11.5f);
            break;
        case Icon::ArrowUpRight:
            pen.rrect(2.5f, 2.5f, 15, 15, 3);
            pen.line(7, 13, 13, 7);
            pen.poly({{8.5f, 7}, {13, 7}, {13, 11.5f}});
            break;
        case Icon::Terminal:
            pen.rrect(2.5f, 3.5f, 15, 13, 2.2f);
            pen.poly({{5.5f, 8}, {8, 10}, {5.5f, 12}});
            pen.line(9.5f, 12.5f, 13.5f, 12.5f);
            break;
        case Icon::Pencil:
            pen.poly({{13.5f, 3}, {17, 6.5f}, {7, 16.5f}, {3, 17}, {3.5f, 13}}, true);
            pen.line(11.5f, 5, 15, 8.5f);
            break;
        case Icon::Mouse:
            pen.rrect(5.5f, 2.5f, 9, 15, 4.5f);
            pen.line(10, 2.8f, 10, 8);
            pen.line(5.6f, 8, 14.4f, 8);
            break;
        case Icon::Accessibility:
            pen.circle(10, 10, 8.5f);
            pen.dot(10, 5.6f, 1.3f);
            pen.line(5.5f, 8.2f, 14.5f, 8.2f);
            pen.line(10, 8.2f, 10, 11.5f);
            pen.poly({{7.3f, 15.5f}, {10, 11.5f}, {12.7f, 15.5f}});
            break;
        case Icon::Network:
            pen.rrect(7.5f, 2.5f, 5, 4, 1);
            pen.rrect(2.5f, 13.5f, 5, 4, 1);
            pen.rrect(12.5f, 13.5f, 5, 4, 1);
            pen.line(10, 6.5f, 10, 10);
            pen.poly({{5, 13.5f}, {5, 10}, {15, 10}, {15, 13.5f}});
            break;
        case Icon::Windows:
            pen.rrect(2.5f, 6, 11, 9, 1.8f);
            pen.poly({{6.5f, 6}, {6.5f, 3}, {17.5f, 3}, {17.5f, 12}, {13.5f, 12}});
            break;
        case Icon::Hourglass:
            pen.line(5, 2.5f, 15, 2.5f);
            pen.line(5, 17.5f, 15, 17.5f);
            pen.poly({{6, 2.5f}, {6, 5.5f}, {10, 10}, {6, 14.5f}, {6, 17.5f}});
            pen.poly({{14, 2.5f}, {14, 5.5f}, {10, 10}, {14, 14.5f}, {14, 17.5f}});
            break;
        case Icon::Shield:
            pen.poly({{10, 2.5f}, {16.5f, 5}, {16, 11}, {10, 17.5f}, {4, 11}, {3.5f, 5}}, true);
            pen.poly({{7.2f, 10}, {9.3f, 12.2f}, {13, 7.8f}});
            break;
        case Icon::Printer:
            pen.rrect(2.5f, 7, 15, 7.5f, 1.8f);
            pen.poly({{5.5f, 7}, {5.5f, 2.5f}, {14.5f, 2.5f}, {14.5f, 7}});
            pen.rrect(5.5f, 11.5f, 9, 6, 0.8f);
            break;
        case Icon::Sparkles:
            pen.poly({{8, 3}, {9.5f, 8.5f}, {15, 10}, {9.5f, 11.5f}, {8, 17}, {6.5f, 11.5f}, {1, 10}, {6.5f, 8.5f}}, true);
            pen.line(15, 2.5f, 15, 6.5f);
            pen.line(13, 4.5f, 17, 4.5f);
            break;
        case Icon::Captions:
            pen.rrect(2.5f, 4.5f, 15, 11, 2.5f);
            pen.arc(7.6f, 10, 2.1f, 45, 270);
            pen.arc(13.1f, 10, 2.1f, 45, 270);
            break;
        case Icon::Library:
            pen.rrect(2.5f, 6.5f, 15, 10.5f, 2);
            pen.line(4.5f, 4.2f, 15.5f, 4.2f);
            pen.line(6.5f, 2.2f, 13.5f, 2.2f);
            pen.p.fillTriangle(pen.at(8.5f, 9.3f), pen.at(12.6f, 11.75f), pen.at(8.5f, 14.2f), color, 0.6f * u);
            break;
        case Icon::Shuffle:
            pen.poly({{3, 6}, {6.5f, 6}, {12.5f, 14}, {16, 14}});
            pen.poly({{13.5f, 11.5f}, {16, 14}, {13.5f, 16.5f}});
            pen.poly({{3, 14}, {6.5f, 14}, {12.5f, 6}, {16, 6}});
            pen.poly({{13.5f, 3.5f}, {16, 6}, {13.5f, 8.5f}});
            break;
        case Icon::RepeatOne:
            pen.poly({{4, 9}, {4, 6}, {15, 6}});
            pen.poly({{12.5f, 3.5f}, {15, 6}, {12.5f, 8.5f}});
            pen.poly({{16, 11}, {16, 14}, {5, 14}});
            pen.poly({{7.5f, 11.5f}, {5, 14}, {7.5f, 16.5f}});
            pen.poly({{8.9f, 9.1f}, {10.2f, 8.2f}, {10.2f, 11.8f}});
            break;
        case Icon::Album:
            pen.rrect(2.5f, 2.5f, 15, 15, 2.5f);
            pen.circle(10, 10, 4.3f);
            pen.dot(10, 10, 1.1f);
            break;
    }
}

}  // namespace mayo
