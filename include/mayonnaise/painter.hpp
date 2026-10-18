#pragma once

#include <string_view>

#include "color.hpp"
#include "geometry.hpp"
#include "theme.hpp"

namespace mayo {

class ImageData;
class VideoTexture;
enum class Icon : std::uint16_t;

namespace render {
class Renderer2D;
class TextSystem;
}  // namespace render

enum class TextAlign : std::uint8_t { Leading, Center, Trailing };

/// 2D drawing API used by every widget and exposed to users through mayo::Canvas.
/// Coordinates are window points. A Painter is only valid during a draw call.
class Painter {
public:
    Painter(render::Renderer2D& renderer, render::TextSystem& text);

    // Shapes ------------------------------------------------------------------------------
    void fillRect(const Rect& r, const Color& color);
    void fillRoundedRect(const Rect& r, const CornerRadii& radii, const Color& color);
    /// Fill plus an inner border in one primitive.
    void fillRoundedRect(const Rect& r, const CornerRadii& radii, const Color& fill, const Color& border,
                         float borderWidth);
    /// Stroke drawn inside the rectangle's edge.
    void strokeRoundedRect(const Rect& r, const CornerRadii& radii, const Color& color, float width = 1);
    void fillCircle(Point center, float radius, const Color& color);
    void strokeCircle(Point center, float radius, const Color& color, float width = 1);
    void fillLinearGradient(const Rect& r, const CornerRadii& radii, const Color& from, const Color& to, Point start,
                            Point end);
    /// Vertical gradient shorthand (top -> bottom).
    void fillVerticalGradient(const Rect& r, const CornerRadii& radii, const Color& top, const Color& bottom);
    /// With exterior=true, draw after the material to keep its backdrop free of self-shadow.
    void drawShadow(const Rect& r, float radius, const Color& color, float blur, Vec2 offset = {}, float spread = 0,
                    bool exterior = false);
    void drawLine(Point a, Point b, const Color& color, float width = 1);
    void fillTriangle(Point a, Point b, Point c, const Color& color, float cornerRadius = 0);
    void drawArc(Point center, float radius, float startAngle, float sweep, const Color& color, float width = 1);
    /// A macOS-style accent focus ring hugging the outside of the shape.
    void drawFocusRing(const Rect& r, const CornerRadii& radii, float progress = 1);

    // Materials -----------------------------------------------------------------------------
    /// Translucent blurred material ("glass") showing what was drawn underneath.
    void drawMaterial(const Rect& r, const CornerRadii& radii, Material material);
    void drawMaterial(const Rect& r, const CornerRadii& radii, const MaterialStyle& style);
    /// An edge overlay over already-painted scrolling content. Does not affect hit testing.
    void drawScrollEdge(const Rect& r, ScrollEdgeStyle style, bool bottom = false, float strength = 1);

    // Images & icons -------------------------------------------------------------------------
    /// Draws `image` stretched into `r` (sample `source`, in normalised 0..1 texture coordinates).
    void drawImage(const Rect& r, const ImageData& image, const CornerRadii& radii = {}, float opacity = 1,
                   const Rect& source = {0, 0, 1, 1});
    /// Draws a YUV video frame (converted to RGB on the GPU) stretched into `r`.
    void drawVideo(const Rect& r, const VideoTexture& video, const CornerRadii& radii = {}, float opacity = 1);
    void drawIcon(Icon icon, const Rect& r, const Color& color, float weight = 1);

    // Text ------------------------------------------------------------------------------
    /// Draws a single line with its baseline at `baseline.y`, starting at `baseline.x`.
    void drawText(std::string_view text, Point baseline, const Font& font, const Color& color);
    /// Draws a single line vertically centred (on cap height) in `r`, aligned horizontally.
    void drawText(std::string_view text, const Rect& r, const Font& font, const Color& color,
                  TextAlign align = TextAlign::Leading);
    [[nodiscard]] float measureText(std::string_view text, const Font& font) const;

    // State -----------------------------------------------------------------------------
    void pushClip(const Rect& r, float radius = 0);
    void popClip();
    [[nodiscard]] Rect clipBounds() const;
    void pushOpacity(float opacity);
    void popOpacity();

    [[nodiscard]] float scale() const;
    /// Rounds a coordinate to the device pixel grid.
    [[nodiscard]] float snap(float v) const;

    [[nodiscard]] render::Renderer2D& renderer() { return m_renderer; }
    [[nodiscard]] render::TextSystem& textSystem() { return m_text; }

private:
    render::Renderer2D& m_renderer;
    render::TextSystem& m_text;
};

}  // namespace mayo
