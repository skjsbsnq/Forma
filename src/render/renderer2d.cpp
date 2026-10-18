#include "renderer2d.hpp"

#include <algorithm>
#include <bit>
#include <cmath>
#include <cstddef>
#include <cstring>
#include <span>
#include <tuple>

#include "embedded.hpp"

namespace mayo::render {

namespace {
constexpr float kNoClip = 1e7f;

#ifdef MAYONNAISE_HAS_VULKAN
std::span<const unsigned char> spirv(std::span<const unsigned char> code) { return code; }
#define MAYO_SPIRV(name) spirv(embedded::name##_spv)
#else
#define MAYO_SPIRV(name) std::span<const unsigned char>()
#endif
}  // namespace

Renderer2D::Renderer2D() : m_device(*rhi::device()) {
    using embedded::text;
    m_program = m_device.createPipeline({
        .name = "ui",
        .vertexGlsl = text(embedded::ui_vert),
        .fragmentGlsl = text(embedded::ui_frag),
        .vertexSpirv = MAYO_SPIRV(ui_vert),
        .fragmentSpirv = MAYO_SPIRV(ui_frag),
        .instanceVec4s = static_cast<int>(sizeof(Instance) / 16),
        .textures = {"u_texture", "u_texture1", "u_texture2"},
        .uniforms = {{"u_viewport", 2, 0}},
        .blend = rhi::Blend::Premultiplied,
        .topology = rhi::Topology::TriangleStrip,
    });
    for (auto [pipeline, name, frag, fragSpirv] :
         {std::tuple{&m_blurDown, "blur_down", text(embedded::blur_down_frag), MAYO_SPIRV(blur_down_frag)},
          std::tuple{&m_blurUp, "blur_up", text(embedded::blur_up_frag), MAYO_SPIRV(blur_up_frag)}}) {
        *pipeline = m_device.createPipeline({
            .name = name,
            .vertexGlsl = text(embedded::fullscreen_vert),
            .fragmentGlsl = frag,
            .vertexSpirv = MAYO_SPIRV(fullscreen_vert),
            .fragmentSpirv = fragSpirv,
            .textures = {"u_source"},
            .uniforms = {{"u_halfPixel", 2, 0}, {"u_offset", 1, 8}},
        });
    }
    m_instances.reserve(4096);
}

Renderer2D::~Renderer2D() = default;

void Renderer2D::ensureTarget(std::unique_ptr<rhi::Texture>& target, int width, int height) {
    width = std::max(1, width);
    height = std::max(1, height);
    if (target && target->width() == width && target->height() == height) return;
    target = m_device.createTexture({.width = width, .height = height, .renderTarget = true});
}

void Renderer2D::beginFrame(platform::PlatformWindow& window, int pixelWidth, int pixelHeight, float scale) {
    m_width = std::max(1, pixelWidth);
    m_height = std::max(1, pixelHeight);
    m_scale = scale;
    m_device.beginFrame(window);
    ensureTarget(m_scene, m_width, m_height);
    m_clips.clear();
    m_clips.push_back({-kNoClip, -kNoClip, kNoClip, kNoClip, -kNoClip, -kNoClip, kNoClip, kNoClip, 0});
    m_opacity.assign(1, 1.0f);
    m_instances.clear();
    m_batchTexture = nullptr;
    m_drawCalls = 0;
    const float transparent[4] = {0, 0, 0, 0};
    m_device.beginPass(*m_scene, transparent);
}

void Renderer2D::endFrame() {
    flush();
    m_drawCallsLast = m_drawCalls;
}

void Renderer2D::present(platform::PlatformWindow& window) { m_device.endFrame(window, *m_scene); }

std::vector<std::uint8_t> Renderer2D::readPixels(int& width, int& height) {
    flush();
    width = m_scene->width();
    height = m_scene->height();
    return m_device.readPixels(*m_scene);
}

// --- submission ------------------------------------------------------------------------------

void Renderer2D::setColor(float* out, const Color& c) const {
    float a = c.a * m_opacity.back();
    out[0] = c.r * a;
    out[1] = c.g * a;
    out[2] = c.b * a;
    out[3] = a;
}

bool Renderer2D::culled(float l, float t, float r, float b) const {
    const Clip& c = m_clips.back();
    return r <= c.l || b <= c.t || l >= c.r || t >= c.b || r <= l || b <= t;
}

Renderer2D::Instance& Renderer2D::push(Type type, float l, float t, float r, float b, rhi::Texture* texture) {
    if (texture && texture != m_batchTexture) {
        if (!m_instances.empty() && m_batchTexture) flush();
        m_batchTexture = texture;
    }
    const Clip& c = m_clips.back();
    Instance& in = m_instances.emplace_back();
    // Shrinking the quad to the clip saves fill rate; textured quads keep theirs so UVs stay valid.
    if (type != Type::Image && type != Type::Glyph) {
        l = std::max(l, c.l);
        t = std::max(t, c.t);
        r = std::min(r, c.r);
        b = std::min(b, c.b);
    }
    in.quad[0] = l;
    in.quad[1] = t;
    in.quad[2] = r;
    in.quad[3] = b;
    in.params[0] = static_cast<float>(type);
    in.params[1] = 0;
    in.params[2] = 0;
    in.params[3] = 1;
    in.clip[0] = c.l;
    in.clip[1] = c.t;
    in.clip[2] = c.r;
    in.clip[3] = c.b;
    in.rclip[0] = c.rl;
    in.rclip[1] = c.rt;
    in.rclip[2] = c.rr;
    in.rclip[3] = c.rb;
    in.clip2[0] = c.radius;
    in.clip2[1] = in.clip2[2] = in.clip2[3] = 0;
    return in;
}

void Renderer2D::fillRoundedRect(const Rect& rect, const CornerRadii& radii, const Color& fill, const Color& border,
                                 float borderWidth) {
    const float s = m_scale;
    // Snap edges to device pixels so hairlines and control edges stay crisp.
    float l = std::round(rect.left() * s);
    float t = std::round(rect.top() * s);
    float r = std::round(rect.right() * s);
    float b = std::round(rect.bottom() * s);
    if (r <= l || b <= t) return;
    if (fill.a <= 0 && (border.a <= 0 || borderWidth <= 0)) return;
    if (culled(l - 1, t - 1, r + 1, b + 1)) return;
    Instance& in = push(Type::Fill, l - 1, t - 1, r + 1, b + 1, nullptr);
    in.shape[0] = (l + r) * 0.5f;
    in.shape[1] = (t + b) * 0.5f;
    in.shape[2] = (r - l) * 0.5f;
    in.shape[3] = (b - t) * 0.5f;
    const float maxR = std::min(in.shape[2], in.shape[3]);
    in.radii[0] = std::min(radii.topLeft * s, maxR);
    in.radii[1] = std::min(radii.topRight * s, maxR);
    in.radii[2] = std::min(radii.bottomRight * s, maxR);
    in.radii[3] = std::min(radii.bottomLeft * s, maxR);
    setColor(in.color, fill);
    setColor(in.color2, border);
    in.params[1] = borderWidth > 0 ? std::max(borderWidth * s, 1.0f) : 0.0f;
}

void Renderer2D::strokeRoundedRect(const Rect& rect, const CornerRadii& radii, const Color& color, float width) {
    fillRoundedRect(rect, radii, Color::transparent(), color, width);
}

void Renderer2D::fillGradient(const Rect& rect, const CornerRadii& radii, const Color& from, const Color& to, Point p0,
                              Point p1) {
    const float s = m_scale;
    float l = std::round(rect.left() * s);
    float t = std::round(rect.top() * s);
    float r = std::round(rect.right() * s);
    float b = std::round(rect.bottom() * s);
    if (r <= l || b <= t || culled(l - 1, t - 1, r + 1, b + 1)) return;
    Instance& in = push(Type::Gradient, l - 1, t - 1, r + 1, b + 1, nullptr);
    in.shape[0] = (l + r) * 0.5f;
    in.shape[1] = (t + b) * 0.5f;
    in.shape[2] = (r - l) * 0.5f;
    in.shape[3] = (b - t) * 0.5f;
    const float maxR = std::min(in.shape[2], in.shape[3]);
    in.radii[0] = std::min(radii.topLeft * s, maxR);
    in.radii[1] = std::min(radii.topRight * s, maxR);
    in.radii[2] = std::min(radii.bottomRight * s, maxR);
    in.radii[3] = std::min(radii.bottomLeft * s, maxR);
    setColor(in.color, from);
    setColor(in.color2, to);
    in.uv[0] = p0.x * s;
    in.uv[1] = p0.y * s;
    in.uv[2] = p1.x * s;
    in.uv[3] = p1.y * s;
}

void Renderer2D::shadow(const Rect& rect, float radius, const Color& color, float blur, Vec2 offset, float spread,
                        bool exterior) {
    if (color.a <= 0) return;
    const float s = m_scale;
    Rect shape = rect.translated(offset).outset(spread);
    float sigma = std::max(blur * 0.5f * s, 0.5f);
    float margin = sigma * 3.0f;
    float l = shape.left() * s - margin;
    float t = shape.top() * s - margin;
    float r = shape.right() * s + margin;
    float b = shape.bottom() * s + margin;
    if (culled(l, t, r, b)) return;
    Instance& in = push(Type::Shadow, l, t, r, b, nullptr);
    in.shape[0] = shape.center().x * s;
    in.shape[1] = shape.center().y * s;
    in.shape[2] = shape.width * 0.5f * s;
    in.shape[3] = shape.height * 0.5f * s;
    float maxR = std::min(in.shape[2], in.shape[3]);
    in.radii[0] = std::min(std::max(0.0f, (radius + spread) * s), maxR);
    setColor(in.color, color);
    in.params[2] = sigma;
    if (exterior) {
        in.params[1] = 1;
        in.uv[0] = rect.center().x * s;
        in.uv[1] = rect.center().y * s;
        in.uv[2] = rect.width * 0.5f * s;
        in.uv[3] = rect.height * 0.5f * s;
        in.color2[0] = std::clamp(radius * s, 0.0f, std::min(in.uv[2], in.uv[3]));
    }
}

void Renderer2D::line(Point a, Point b, const Color& color, float width) {
    const float s = m_scale;
    float hw = std::max(width * s * 0.5f, 0.5f);
    float l = std::min(a.x, b.x) * s - hw - 1;
    float t = std::min(a.y, b.y) * s - hw - 1;
    float r = std::max(a.x, b.x) * s + hw + 1;
    float bt = std::max(a.y, b.y) * s + hw + 1;
    if (culled(l, t, r, bt)) return;
    Instance& in = push(Type::Line, l, t, r, bt, nullptr);
    in.shape[0] = a.x * s;
    in.shape[1] = a.y * s;
    in.shape[2] = b.x * s;
    in.shape[3] = b.y * s;
    setColor(in.color, color);
    in.params[1] = hw;
}

void Renderer2D::triangle(Point a, Point b, Point c, const Color& color, float cornerRadius) {
    const float s = m_scale;
    const float r = std::max(0.0f, cornerRadius) * s;
    float l = std::min({a.x, b.x, c.x}) * s - r - 1;
    float t = std::min({a.y, b.y, c.y}) * s - r - 1;
    float rr = std::max({a.x, b.x, c.x}) * s + r + 1;
    float bb = std::max({a.y, b.y, c.y}) * s + r + 1;
    if (culled(l, t, rr, bb)) return;
    Instance& in = push(Type::Triangle, l, t, rr, bb, nullptr);
    in.shape[0] = a.x * s;
    in.shape[1] = a.y * s;
    in.shape[2] = b.x * s;
    in.shape[3] = b.y * s;
    in.uv[0] = c.x * s;
    in.uv[1] = c.y * s;
    in.uv[2] = r;
    setColor(in.color, color);
}

void Renderer2D::arc(Point center, float radius, float startAngle, float sweep, const Color& color, float width) {
    const float s = m_scale;
    float hw = std::max(width * s * 0.5f, 0.5f);
    float ext = radius * s + hw + 1;
    float cx = center.x * s;
    float cy = center.y * s;
    if (culled(cx - ext, cy - ext, cx + ext, cy + ext)) return;
    Instance& in = push(Type::Arc, cx - ext, cy - ext, cx + ext, cy + ext, nullptr);
    in.shape[0] = cx;
    in.shape[1] = cy;
    in.shape[2] = radius * s;
    in.uv[0] = std::fmod(std::fmod(startAngle, 6.2831853f) + 6.2831853f, 6.2831853f);
    in.uv[1] = std::clamp(sweep, 0.0f, 6.2831853f);
    setColor(in.color, color);
    in.params[1] = hw;
}

void Renderer2D::image(const Rect& rect, rhi::Texture* texture, const Rect& uv, const CornerRadii& radii,
                       float opacity) {
    const float s = m_scale;
    float l = std::round(rect.left() * s);
    float t = std::round(rect.top() * s);
    float r = std::round(rect.right() * s);
    float b = std::round(rect.bottom() * s);
    if (culled(l, t, r, b)) return;
    Instance& in = push(Type::Image, l, t, r, b, texture);
    in.shape[0] = (l + r) * 0.5f;
    in.shape[1] = (t + b) * 0.5f;
    in.shape[2] = (r - l) * 0.5f;
    in.shape[3] = (b - t) * 0.5f;
    const float maxR = std::min(in.shape[2], in.shape[3]);
    in.radii[0] = std::min(radii.topLeft * s, maxR);
    in.radii[1] = std::min(radii.topRight * s, maxR);
    in.radii[2] = std::min(radii.bottomRight * s, maxR);
    in.radii[3] = std::min(radii.bottomLeft * s, maxR);
    in.uv[0] = uv.left();
    in.uv[1] = uv.top();
    in.uv[2] = uv.right();
    in.uv[3] = uv.bottom();
    in.color[0] = in.color[1] = in.color[2] = 1;
    in.color[3] = opacity * m_opacity.back();
}

void Renderer2D::video(const Rect& rect, rhi::Texture* const (&tex)[3], bool planar, int matrix, bool fullRange,
                       const CornerRadii& radii, float opacity) {
    const float s = m_scale;
    float l = std::round(rect.left() * s);
    float t = std::round(rect.top() * s);
    float r = std::round(rect.right() * s);
    float b = std::round(rect.bottom() * s);
    if (!tex[0] || !tex[1] || culled(l, t, r, b)) return;
    // Video needs extra texture units; give it a batch of its own.
    flush();
    m_extraTextures[0] = tex[1];
    m_extraTextures[1] = tex[2];
    Instance& in = push(Type::Video, l, t, r, b, tex[0]);
    in.shape[0] = (l + r) * 0.5f;
    in.shape[1] = (t + b) * 0.5f;
    in.shape[2] = (r - l) * 0.5f;
    in.shape[3] = (b - t) * 0.5f;
    const float maxR = std::min(in.shape[2], in.shape[3]);
    in.radii[0] = std::min(radii.topLeft * s, maxR);
    in.radii[1] = std::min(radii.topRight * s, maxR);
    in.radii[2] = std::min(radii.bottomRight * s, maxR);
    in.radii[3] = std::min(radii.bottomLeft * s, maxR);
    in.uv[0] = 0;
    in.uv[1] = 0;
    in.uv[2] = 1;
    in.uv[3] = 1;
    in.color[3] = opacity * m_opacity.back();
    in.params[1] = planar ? 1.0f : 0.0f;
    in.params[2] = static_cast<float>(matrix);
    in.params[3] = fullRange ? 1.0f : 0.0f;
    flush();
    m_extraTextures[0] = m_extraTextures[1] = nullptr;
}

void Renderer2D::glyphPx(const Rect& px, rhi::Texture* atlas, const Rect& uv, const Color& color, float gamma) {
    if (culled(px.left(), px.top(), px.right(), px.bottom())) return;
    Instance& in = push(Type::Glyph, px.left(), px.top(), px.right(), px.bottom(), atlas);
    in.uv[0] = uv.left();
    in.uv[1] = uv.top();
    in.uv[2] = uv.right();
    in.uv[3] = uv.bottom();
    setColor(in.color, color);
    in.params[3] = gamma;
}

void Renderer2D::backdrop(const Rect& rect, const CornerRadii& radii, const MaterialStyle& style) {
    const float s = m_scale;
    float l = std::round(rect.left() * s);
    float t = std::round(rect.top() * s);
    float r = std::round(rect.right() * s);
    float b = std::round(rect.bottom() * s);
    if (culled(l, t, r, b)) return;
    flush();
    // Refraction can read beyond the panel. Include that footprint in the regional blur,
    // otherwise another material's stale pixels can leak into the lens edge.
    const float displacement = std::max(0.0f, style.refraction) * s;
    rhi::Texture* blurred = blurScene(style.blurRadius * s, Rect::fromLTRB(l, t, r, b).outset(displacement));
    Instance& in = push(Type::Backdrop, l - 1, t - 1, r + 1, b + 1, blurred);
    in.shape[0] = (l + r) * 0.5f;
    in.shape[1] = (t + b) * 0.5f;
    in.shape[2] = (r - l) * 0.5f;
    in.shape[3] = (b - t) * 0.5f;
    const float maxR = std::min(in.shape[2], in.shape[3]);
    in.radii[0] = std::min(radii.topLeft * s, maxR);
    in.radii[1] = std::min(radii.topRight * s, maxR);
    in.radii[2] = std::min(radii.bottomRight * s, maxR);
    in.radii[3] = std::min(radii.bottomLeft * s, maxR);
    // Tint is premultiplied but must not be scaled by group opacity twice; backdrop alpha is the shape.
    Color tint = style.tint;
    in.color[0] = tint.r * tint.a;
    in.color[1] = tint.g * tint.a;
    in.color[2] = tint.b * tint.a;
    in.color[3] = tint.a;
    in.params[1] = style.fadeBottom;
    in.clip2[1] = m_opacity.back();
    in.params[2] = style.saturation;
    in.params[3] = style.noise;
    in.uv[0] = std::max(0.0f, style.bezelWidth) * s;
    in.uv[1] = displacement;
    in.uv[2] = std::clamp(style.rimStrength, 0.0f, 1.0f);
    in.uv[3] = s;
    in.color2[0] = style.toneLow;
    in.color2[1] = style.toneHigh;
    in.color2[2] = std::max(0.01f, style.toneCurve);
    // The blur chain is reused by the next material, so draw this one right away.
    flush();
}

void Renderer2D::pushClip(const Rect& rect, float radius) {
    const float s = m_scale;
    const Clip& cur = m_clips.back();
    const float l = std::round(rect.left() * s);
    const float t = std::round(rect.top() * s);
    const float r = std::round(rect.right() * s);
    const float b = std::round(rect.bottom() * s);
    Clip c = cur;
    c.l = std::max(cur.l, l);
    c.t = std::max(cur.t, t);
    c.r = std::max(c.l, std::min(cur.r, r));
    c.b = std::max(c.t, std::min(cur.b, b));
    // The shader evaluates one rounded clip plus the plain intersection. A nested rounded clip
    // replaces the outer one; nested rectangular clips keep it, so corners stay rounded.
    if (radius > 0) {
        c.rl = l;
        c.rt = t;
        c.rr = r;
        c.rb = b;
        c.radius = radius * s;
    }
    m_clips.push_back(c);
}

void Renderer2D::popClip() {
    if (m_clips.size() > 1) m_clips.pop_back();
}

Rect Renderer2D::currentClip() const {
    const Clip& c = m_clips.back();
    return Rect::fromLTRB(c.l / m_scale, c.t / m_scale, c.r / m_scale, c.b / m_scale);
}

void Renderer2D::pushOpacity(float opacity) { m_opacity.push_back(m_opacity.back() * opacity); }
void Renderer2D::popOpacity() {
    if (m_opacity.size() > 1) m_opacity.pop_back();
}

void Renderer2D::flush() {
    if (m_instances.empty()) return;
    m_device.beginPass(*m_scene);
    const float viewport[2] = {static_cast<float>(m_width), static_cast<float>(m_height)};
    rhi::Texture* const textures[3] = {m_batchTexture, m_extraTextures[0], m_extraTextures[1]};
    m_device.draw(*m_program, textures, viewport, sizeof viewport, std::as_bytes(std::span(m_instances)), 4,
                  static_cast<int>(m_instances.size()));
    ++m_drawCalls;
    m_instances.clear();
    m_batchTexture = nullptr;
}

rhi::Texture* Renderer2D::blurScene(float radiusPx, const Rect& regionPx) {
    // Each dual-Kawase level roughly doubles the blur radius.
    int levels = std::clamp(static_cast<int>(std::ceil(std::log2(std::max(radiusPx, 2.0f) / 2.5f))), 1, 6);
    if (static_cast<int>(m_blurChain.size()) < levels) m_blurChain.resize(static_cast<size_t>(levels));
    for (int i = 0; i < levels; ++i)
        ensureTarget(m_blurChain[static_cast<size_t>(i)], m_width >> (i + 1), m_height >> (i + 1));

    // Only blur around the region that will be sampled (plus the kernel footprint).
    const float margin = radiusPx * 2.0f + 16.0f;
    Rect area =
        regionPx.outset(margin).intersected(Rect(0, 0, static_cast<float>(m_width), static_cast<float>(m_height)));
    auto scissorFor = [&](int w, int h) {
        float kx = static_cast<float>(w) / static_cast<float>(m_width);
        float ky = static_cast<float>(h) / static_cast<float>(m_height);
        // Rect is top-down; target rows are bottom-up (rhi.hpp).
        int x0 = static_cast<int>(std::floor(area.left() * kx));
        int x1 = static_cast<int>(std::ceil(area.right() * kx));
        int y0 = static_cast<int>(std::floor((static_cast<float>(m_height) - area.bottom()) * ky));
        int y1 = static_cast<int>(std::ceil((static_cast<float>(m_height) - area.top()) * ky));
        return rhi::Scissor{x0, y0, x1 - x0, y1 - y0};
    };

    auto pass = [&](rhi::Pipeline& pipeline, rhi::Texture* src, rhi::Texture& dst) {
        m_device.beginPass(dst);
        const rhi::Scissor scissor = scissorFor(dst.width(), dst.height());
        m_device.setScissor(&scissor);
        const float uniforms[3] = {0.5f / static_cast<float>(src->width()), 0.5f / static_cast<float>(src->height()),
                                   1.0f};
        rhi::Texture* const textures[1] = {src};
        m_device.draw(pipeline, textures, uniforms, sizeof uniforms, {}, 3, 1);
    };

    rhi::Texture* src = m_scene.get();
    for (int i = 0; i < levels; ++i) {
        auto& dst = *m_blurChain[static_cast<size_t>(i)];
        pass(*m_blurDown, src, dst);
        src = &dst;
    }
    for (int i = levels - 1; i > 0; --i)
        pass(*m_blurUp, m_blurChain[static_cast<size_t>(i)].get(), *m_blurChain[static_cast<size_t>(i - 1)]);

    m_device.beginPass(*m_scene);
    return m_blurChain[0].get();
}

}  // namespace mayo::render
