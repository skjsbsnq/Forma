#pragma once

#include <cstdint>
#include <vector>

#include "mayonnaise/color.hpp"
#include "mayonnaise/geometry.hpp"
#include "mayonnaise/theme.hpp"
#include "rhi/rhi.hpp"

namespace mayo::render {

/// Batched immediate-mode 2D renderer. All public coordinates are logical points; they are scaled
/// to device pixels on submission. Draws into an offscreen scene target so backdrop materials can
/// sample (and blur) what has been drawn so far.
class Renderer2D {
public:
    Renderer2D();
    ~Renderer2D();
    Renderer2D(const Renderer2D&) = delete;
    Renderer2D& operator=(const Renderer2D&) = delete;

    void beginFrame(platform::PlatformWindow& window, int pixelWidth, int pixelHeight, float scale);
    /// Submits the queued primitives.
    void endFrame();
    /// Shows the scene in the window the frame began for (waits for vsync).
    void present(platform::PlatformWindow& window);
    /// Reads back the scene target as top-down RGBA8 (premultiplied).
    std::vector<std::uint8_t> readPixels(int& width, int& height);

    [[nodiscard]] float scale() const { return m_scale; }

    // --- primitives (points) -------------------------------------------------------------
    void fillRoundedRect(const Rect& r, const CornerRadii& radii, const Color& fill,
                         const Color& border = Color::transparent(), float borderWidth = 0);
    /// Stroke outside or inside the shape edge: `inset` < 0 draws the stroke outside (focus rings).
    void strokeRoundedRect(const Rect& r, const CornerRadii& radii, const Color& color, float width);
    void fillGradient(const Rect& r, const CornerRadii& radii, const Color& from, const Color& to, Point p0, Point p1);
    void shadow(const Rect& r, float radius, const Color& color, float blur, Vec2 offset = {}, float spread = 0,
                bool exterior = false);
    void line(Point a, Point b, const Color& color, float width);
    /// Filled triangle with optionally rounded corners (radius grows it outwards).
    void triangle(Point a, Point b, Point c, const Color& color, float cornerRadius = 0);
    /// Arc around `center`; angles in radians, 0 = +x, positive sweep is clockwise on screen.
    void arc(Point center, float radius, float startAngle, float sweep, const Color& color, float width);
    void image(const Rect& r, rhi::Texture* texture, const Rect& uv, const CornerRadii& radii, float opacity);
    /// YUV frame: tex[0] = Y, tex[1] = UV (NV12) or U, tex[2] = V (I420) or 0. `matrix`: 0 = BT.601,
    /// 1 = BT.709, 2 = BT.2020.
    void video(const Rect& r, rhi::Texture* const (&tex)[3], bool planar, int matrix, bool fullRange,
               const CornerRadii& radii, float opacity);
    /// Glyph quad already in device pixels (text is laid out on the pixel grid).
    void glyphPx(const Rect& px, rhi::Texture* atlas, const Rect& uv, const Color& color, float gamma);
    /// Blurs everything drawn so far and draws it, tinted, inside the shape.
    void backdrop(const Rect& r, const CornerRadii& radii, const MaterialStyle& style);

    void pushClip(const Rect& r, float radius = 0);
    void popClip();
    [[nodiscard]] Rect currentClip() const;  // points
    void pushOpacity(float opacity);
    void popOpacity();

    [[nodiscard]] int drawCallsLastFrame() const { return m_drawCallsLast; }
    /// Submits queued primitives if the current batch samples `texture` (before it is modified).
    void flushIfUsing(const rhi::Texture* texture) {
        if (m_batchTexture == texture) flush();
    }

private:
    struct Instance {
        float quad[4];
        float shape[4];
        float radii[4];
        float color[4];
        float color2[4];
        float uv[4];
        float params[4];
        float clip[4];   // plain clip: intersection of every pushed rect
        float rclip[4];  // innermost rounded clip rect
        float clip2[4];  // rounded clip radius, group opacity (backdrops), -, -
    };
    struct Clip {
        float l, t, r, b;              // pixels, intersection of all clips
        float rl, rt, rr, rb, radius;  // innermost rounded clip
    };
    enum class Type : int { Fill = 0, Shadow, Image, Glyph, Line, Gradient, Backdrop, Arc, Triangle, Video };

    Instance& push(Type type, float l, float t, float r, float b, rhi::Texture* texture);
    bool culled(float l, float t, float r, float b) const;
    void setColor(float* out, const Color& c) const;
    void flush();
    rhi::Texture* blurScene(float radiusPx, const Rect& regionPx);
    /// (Re)creates a render target when its size changed.
    void ensureTarget(std::unique_ptr<rhi::Texture>& target, int width, int height);

    rhi::Device& m_device;
    std::unique_ptr<rhi::Pipeline> m_program;
    std::unique_ptr<rhi::Pipeline> m_blurDown;
    std::unique_ptr<rhi::Pipeline> m_blurUp;
    rhi::Texture* m_extraTextures[2] = {nullptr, nullptr};  // bindings 1 and 2, only for video batches

    std::unique_ptr<rhi::Texture> m_scene;
    std::vector<std::unique_ptr<rhi::Texture>> m_blurChain;

    std::vector<Instance> m_instances;
    rhi::Texture* m_batchTexture = nullptr;
    std::vector<Clip> m_clips;
    std::vector<float> m_opacity;
    int m_width = 0;
    int m_height = 0;
    float m_scale = 1;
    int m_drawCalls = 0;
    int m_drawCallsLast = 0;
};

}  // namespace mayo::render
