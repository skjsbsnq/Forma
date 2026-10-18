#version 330 core
#ifdef VULKAN
// The same source is compiled to SPIR-V; GLSL 330 needs these for explicit locations and bindings.
#extension GL_ARB_separate_shader_objects : require
#extension GL_ARB_shading_language_420pack : require
#define LOC(n) layout(location = n)
#define BINDING(n) layout(set = 0, binding = n)
#else
#define LOC(n)
#define BINDING(n)
#endif
// Uber-shader for every 2D primitive. Shapes are signed distance fields evaluated in pixels, so
// edges are anti-aliased over exactly one device pixel at any scale.

LOC(0) in vec2 v_pixel;
LOC(1) in vec2 v_uv;
LOC(2) flat in vec4 v_shape;
LOC(3) flat in vec4 v_radii;
LOC(4) flat in vec4 v_color;
LOC(5) flat in vec4 v_color2;
LOC(6) flat in vec4 v_gradient;
LOC(7) flat in vec4 v_params;
LOC(8) flat in vec4 v_clip;
LOC(9) flat in vec4 v_rclip;
LOC(10) flat in vec4 v_clip2;

BINDING(0) uniform sampler2D u_texture;
BINDING(1) uniform sampler2D u_texture1;  // video chroma (UV, or U)
BINDING(2) uniform sampler2D u_texture2;  // video V (planar formats)
#ifdef VULKAN
layout(push_constant) uniform Push { vec2 u_viewport; };
#else
uniform vec2 u_viewport;
#endif

LOC(0) out vec4 o_color;

const int kFill = 0;
const int kShadow = 1;
const int kImage = 2;
const int kGlyph = 3;
const int kLine = 4;
const int kGradient = 5;
const int kBackdrop = 6;
const int kArc = 7;
const int kTriangle = 8;
const int kVideo = 9;

// Limited/full-range YUV to sRGB for the common matrices.
vec3 yuvToRgb(float y, vec2 uv, int matrix, bool fullRange) {
    if (fullRange) {
        uv -= 0.5;
    } else {
        y = (y - 16.0 / 255.0) * (255.0 / 219.0);
        uv = (uv - 128.0 / 255.0) * (255.0 / 224.0);
    }
    vec3 rgb;
    if (matrix == 0)
        rgb = vec3(y + 1.402 * uv.y, y - 0.344136 * uv.x - 0.714136 * uv.y, y + 1.772 * uv.x);
    else if (matrix == 2)
        rgb = vec3(y + 1.4746 * uv.y, y - 0.164553 * uv.x - 0.571353 * uv.y, y + 1.8814 * uv.x);
    else
        rgb = vec3(y + 1.5748 * uv.y, y - 0.187324 * uv.x - 0.468124 * uv.y, y + 1.8556 * uv.x);
    return clamp(rgb, 0.0, 1.0);
}

// Signed distance to a triangle (Inigo Quilez).
float sdTriangle(vec2 p, vec2 p0, vec2 p1, vec2 p2) {
    vec2 e0 = p1 - p0, e1 = p2 - p1, e2 = p0 - p2;
    vec2 v0 = p - p0, v1 = p - p1, v2 = p - p2;
    vec2 pq0 = v0 - e0 * clamp(dot(v0, e0) / dot(e0, e0), 0.0, 1.0);
    vec2 pq1 = v1 - e1 * clamp(dot(v1, e1) / dot(e1, e1), 0.0, 1.0);
    vec2 pq2 = v2 - e2 * clamp(dot(v2, e2) / dot(e2, e2), 0.0, 1.0);
    float s = sign(e0.x * e2.y - e0.y * e2.x);
    vec2 d = min(min(vec2(dot(pq0, pq0), s * (v0.x * e0.y - v0.y * e0.x)),
                     vec2(dot(pq1, pq1), s * (v1.x * e1.y - v1.y * e1.x))),
                 vec2(dot(pq2, pq2), s * (v2.x * e2.y - v2.y * e2.x)));
    return -sqrt(d.x) * sign(d.y);
}

// Rounded box with per-corner radii (tl, tr, br, bl); y grows downwards.
float sdRoundBox(vec2 p, vec2 halfSize, vec4 r) {
    float radius = p.x > 0.0 ? (p.y > 0.0 ? r.z : r.y) : (p.y > 0.0 ? r.w : r.x);
    vec2 q = abs(p) - halfSize + radius;
    return min(max(q.x, q.y), 0.0) + length(max(q, 0.0)) - radius;
}

// Lens profile adapted from MyGo's MIT-licensed glass plugin.
// See third_party/mygo/LICENSE and NOTICE. Guard the exact outer edge: the
// profile's tangent becomes vertical there, and must stay finite on both GPUs.
float glassLens(float depth, float bezel) {
    if (bezel <= 0.0 || depth >= bezel) return 0.0;
    float u = 1.0 - clamp(depth / bezel, 0.0, 1.0);
    float v = max(1.0 - u*u*u*u, 0.0);
    float a = sqrt(v * sqrt(v));
    float b = u*u*u;
    float n = max(length(vec2(a, b)), 0.0001);
    float sn = b / n, cs = a / n;
    float st = sn / 1.5, ct = sqrt(max(1.0 - st*st, 0.0));
    return (sn*ct - cs*st) / max(cs*ct + sn*st, 0.0001) / 1.118034;
}

vec2 glassNormal(vec2 p, vec2 halfSize, vec4 radii) {
    float radius = p.x > 0.0 ? (p.y > 0.0 ? radii.z : radii.y) : (p.y > 0.0 ? radii.w : radii.x);
    vec2 q = abs(p) - halfSize + radius;
    vec2 corner = max(q, 0.0);
    if (dot(corner, corner) > 0.0001) return sign(p) * normalize(corner);
    return q.x > q.y ? vec2(sign(p.x), 0.0) : vec2(0.0, sign(p.y));
}

float sdSegment(vec2 p, vec2 a, vec2 b) {
    vec2 pa = p - a;
    vec2 ba = b - a;
    float h = clamp(dot(pa, ba) / max(dot(ba, ba), 1e-6), 0.0, 1.0);
    return length(pa - ba * h);
}

// Arc of a circle centred at the origin, from angle a0 sweeping `sweep` radians (clockwise on screen).
float sdArc(vec2 p, float radius, float a0, float sweep) {
    float a = atan(p.y, p.x) - a0;
    a = mod(a, 6.28318530718);
    if (a <= sweep) return abs(length(p) - radius);
    vec2 e0 = radius * vec2(cos(a0), sin(a0));
    vec2 e1 = radius * vec2(cos(a0 + sweep), sin(a0 + sweep));
    return min(length(p - e0), length(p - e1));
}

// --- Gaussian rounded-box shadow (Evan Wallace, "Fast rounded rectangle shadows") -------------
float gaussian(float x, float sigma) {
    return exp(-(x * x) / (2.0 * sigma * sigma)) / (2.50662827463 * sigma);
}
vec2 erf2(vec2 x) {
    vec2 s = sign(x), a = abs(x);
    x = 1.0 + (0.278393 + (0.230389 + 0.078108 * (a * a)) * a) * a;
    x *= x;
    return s - s / (x * x);
}
float shadowX(float x, float y, float sigma, float corner, vec2 halfSize) {
    float delta = min(halfSize.y - corner - abs(y), 0.0);
    float curved = halfSize.x - corner + sqrt(max(0.0, corner * corner - delta * delta));
    vec2 integral = 0.5 + 0.5 * erf2((x + vec2(-curved, curved)) * (0.70710678 / sigma));
    return integral.y - integral.x;
}
float roundedBoxShadow(vec2 p, vec2 halfSize, float sigma, float corner) {
    float low = p.y - halfSize.y;
    float high = p.y + halfSize.y;
    float start = clamp(-3.0 * sigma, low, high);
    float end = clamp(3.0 * sigma, low, high);
    float stepSize = (end - start) / 4.0;
    float y = start + stepSize * 0.5;
    float value = 0.0;
    for (int i = 0; i < 4; i++) {
        value += shadowX(p.x, p.y - y, sigma, corner, halfSize) * gaussian(y, sigma) * stepSize;
        y += stepSize;
    }
    return value;
}

float hash(vec2 p) {
    return fract(sin(dot(p, vec2(12.9898, 78.233))) * 43758.5453);
}

float coverage(float d) { return clamp(0.5 - d, 0.0, 1.0); }

void main() {
    int type = int(v_params.x + 0.5);
    vec2 p = v_pixel - v_shape.xy;
    vec4 color = v_color;
    float alpha = 1.0;

    if (type == kFill) {
        float d = sdRoundBox(p, v_shape.zw, v_radii);
        float stroke = v_params.y;
        alpha = coverage(d);
        if (stroke > 0.0) {
            // Inner border: blend border colour over fill within `stroke` pixels of the edge.
            float inner = coverage(d + stroke);
            color = mix(v_color2, v_color, inner);
        }
    } else if (type == kShadow) {
        alpha = roundedBoxShadow(p, v_shape.zw, max(v_params.z, 0.5), v_radii.x);
    } else if (type == kImage) {
        alpha = coverage(sdRoundBox(p, v_shape.zw, v_radii));
        color = texture(u_texture, v_uv) * v_color.a;
    } else if (type == kGlyph) {
        float a = texture(u_texture, v_uv).r;
        // Gamma tweak per text colour: dark-on-light text needs a little emboldening to match
        // the weight of macOS text rendering, light-on-dark slightly less.
        alpha = pow(a, v_params.w);
    } else if (type == kLine) {
        float d = sdSegment(v_pixel, v_shape.xy, v_shape.zw) - v_params.y;
        alpha = coverage(d);
    } else if (type == kGradient) {
        alpha = coverage(sdRoundBox(p, v_shape.zw, v_radii));
        vec2 g0 = v_gradient.xy;
        vec2 g1 = v_gradient.zw;
        vec2 dir = g1 - g0;
        float t = clamp(dot(v_pixel - g0, dir) / max(dot(dir, dir), 1e-6), 0.0, 1.0);
        color = mix(v_color, v_color2, t);
    } else if (type == kBackdrop) {
        float distance = sdRoundBox(p, v_shape.zw, v_radii);
        alpha = coverage(distance);
        float depth = max(-distance, 0.0);
        float bezel = min(v_gradient.x, min(v_shape.z, v_shape.w));
        vec2 normal = glassNormal(p, v_shape.zw, v_radii);
        vec2 samplePixel = v_pixel - normal * v_gradient.y * glassLens(depth, bezel);
        // Clamp displaced samples to the scene, including a half-texel inset.
        samplePixel = clamp(samplePixel, vec2(0.5), u_viewport - 0.5);
        vec2 uv = vec2(samplePixel.x / u_viewport.x, 1.0 - samplePixel.y / u_viewport.y);
        vec3 bg = texture(u_texture, uv).rgb;
        // Vibrancy: push the blurred backdrop away from its luminance.
        float l = dot(bg, vec3(0.2126, 0.7152, 0.0722));
        float tone = mix(v_color2.x, v_color2.y, 1.0 - pow(max(1.0-l, 0.0), v_color2.z));
        bg = clamp(vec3(tone) + (bg-l) * v_params.z * (v_color2.y-v_color2.x), 0.0, 1.0);
        vec3 tinted = bg * (1.0 - v_color.a) + v_color.rgb;  // tint is premultiplied
        if (bezel > 0.0 && v_gradient.z > 0.0) {
            // A lens needs its own surface lighting on a uniform backdrop too.
            // The wide shoulder, bright lip and inner shadow describe thickness;
            // only the backdrop is refracted, so foreground text stays crisp.
            float pixel = max(v_gradient.w, 0.5);
            float rim = depth / pixel;
            float shoulder = exp(-depth / max(min(bezel * 0.42, pixel * 6.0), pixel));
            float light = dot(normal, vec2(-0.6, -0.8));
            float inner = (depth - 2.4 * pixel) / (1.3 * pixel);
            float innerShadow = exp(-inner * inner);
            float shade = (0.22 * max(-light, 0.0) * shoulder + 0.06 * innerShadow) * v_gradient.z;
            tinted *= 1.0 - shade;
            float lip = exp(-rim * rim);
            float gleam = (0.85 * max(light, 0.0) * lip + 0.40 * max(light, 0.0) * shoulder) * v_gradient.z;
            tinted = mix(tinted, vec3(1.0), clamp(gleam, 0.0, 1.0));
            float fy = (p.y + v_shape.w) / max(2.0 * v_shape.w, 1.0);
            float sheen = (fy - 0.23) / 0.18;
            tinted = mix(tinted, vec3(1.0), 0.10 * v_gradient.z * exp(-sheen * sheen));
        }
        tinted += (hash(floor(v_pixel)) - 0.5) * v_params.w;
        color = vec4(tinted, 1.0);
        alpha *= v_clip2.y;  // group opacity (fading popovers)
        if (v_params.y > 0.0) {
            float fy = (p.y + v_shape.w) / (2.0 * v_shape.w);
            alpha *= 1.0 - smoothstep(1.0 - v_params.y, 1.0, fy);
        }
    } else if (type == kVideo) {
        alpha = coverage(sdRoundBox(p, v_shape.zw, v_radii));
        float y = texture(u_texture, v_uv).r;
        vec2 uv = v_params.y > 0.5 ? vec2(texture(u_texture1, v_uv).r, texture(u_texture2, v_uv).r)
                                   : texture(u_texture1, v_uv).rg;
        color = vec4(yuvToRgb(y, uv, int(v_params.z + 0.5), v_params.w > 0.5), 1.0) * v_color.a;
    } else if (type == kTriangle) {
        alpha = coverage(sdTriangle(v_pixel, v_shape.xy, v_shape.zw, v_gradient.xy) - v_gradient.z);
    } else if (type == kArc) {
        float d = sdArc(p, v_shape.z, v_gradient.x, v_gradient.y) - v_params.y;
        alpha = coverage(d);
    }

    // Clips (pixels): the plain intersection rect, then the innermost rounded rect.
    vec2 clipCenter = (v_clip.xy + v_clip.zw) * 0.5;
    vec2 clipHalf = (v_clip.zw - v_clip.xy) * 0.5;
    alpha *= coverage(sdRoundBox(v_pixel - clipCenter, clipHalf, vec4(0.0)));
    if (v_clip2.x > 0.0) {
        vec2 rc = (v_rclip.xy + v_rclip.zw) * 0.5;
        vec2 rh = (v_rclip.zw - v_rclip.xy) * 0.5;
        alpha *= coverage(sdRoundBox(v_pixel - rc, rh, vec4(v_clip2.x)));
    }

    o_color = color * alpha;
}
