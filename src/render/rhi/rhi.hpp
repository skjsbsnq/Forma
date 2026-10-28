#pragma once

// Render hardware interface: the GPU operations Renderer2D, the glyph atlas, images and video need, implemented
// on OpenGL 3.3 (gl_device.cpp) and Vulkan 1.3 (vk_device.cpp). One device serves every window.
//
// Render targets follow GL's convention on both backends: row 0 is the bottom of the picture. The shaders put
// the top of the UI at NDC y = +1, which lands in the last row under GL and Vulkan alike, so scissors, blur
// passes and backdrop sampling are identical; only presenting to a Vulkan swapchain and reading pixels back flip.

#include <cstddef>
#include <cstdint>
#include <memory>
#include <span>
#include <string>
#include <string_view>
#include <vector>

namespace mayo::platform {
class PlatformWindow;
}

namespace mayo::rhi {

enum class Backend : std::uint8_t { OpenGL, Vulkan };
enum class Format : std::uint8_t { R8, RG8, R16, RG16, RGBA8 };
enum class Filter : std::uint8_t { Nearest, Linear };

[[nodiscard]] int bytesPerPixel(Format format);

struct TextureDesc {
    int width = 1;
    int height = 1;
    Format format = Format::RGBA8;
    Filter filter = Filter::Linear;
    bool mipmaps = false;       // regenerated from level 0 after every upload
    bool renderTarget = false;
};

/// A sampled 2D texture (optionally also a render target). Contents start out zeroed. May be destroyed on any
/// thread; the backend defers freeing until the GPU is done with it.
class Texture {
public:
    virtual ~Texture() = default;
    Texture(const Texture&) = delete;
    Texture& operator=(const Texture&) = delete;

    [[nodiscard]] const TextureDesc& desc() const { return m_desc; }
    [[nodiscard]] int width() const { return m_desc.width; }
    [[nodiscard]] int height() const { return m_desc.height; }

protected:
    explicit Texture(const TextureDesc& desc) : m_desc(desc) {}
    TextureDesc m_desc;
};

/// One single-plane dma-buf (DRM fourcc R8 / GR88 / R16 / GR1616) to sample from without copying.
struct DmaBufPlane {
    std::uint32_t drmFormat = 0;
    int width = 0;
    int height = 0;
    int fd = -1;  // borrowed: the backend duplicates it if it needs to keep it
    std::uint32_t offset = 0;
    std::uint32_t pitch = 0;
    std::uint64_t modifier = ~0ull >> 8;  // DRM_FORMAT_MOD_INVALID
};

enum class Blend : std::uint8_t { None, Premultiplied };
enum class Topology : std::uint8_t { TriangleStrip, Triangles };

/// Push-constant style parameters: floats at byte offsets of a small block (std430 layout, at most 64 bytes).
/// Vulkan passes the block as push constants; GL sets the uniform of the same name.
struct UniformDesc {
    std::string_view name;
    int floats = 1;  // 1, 2 or 4
    int offset = 0;  // bytes
};

struct PipelineDesc {
    std::string_view name;
    std::string_view vertexGlsl;
    std::string_view fragmentGlsl;
    std::span<const unsigned char> vertexSpirv;
    std::span<const unsigned char> fragmentSpirv;
    int instanceVec4s = 0;  // per-instance vec4 attributes at locations 0..n-1 (0: no vertex input)
    std::vector<std::string_view> textures;  // sampler names, bound in order (binding / unit i)
    std::vector<UniformDesc> uniforms;
    Blend blend = Blend::None;
    Topology topology = Topology::Triangles;
};

class Pipeline {
public:
    virtual ~Pipeline() = default;
};

/// Pixel rectangle in target rows (row 0 = bottom, as in GL).
struct Scissor {
    int x = 0;
    int y = 0;
    int width = 0;
    int height = 0;
};

class Device {
public:
    virtual ~Device() = default;

    [[nodiscard]] virtual Backend backend() const = 0;
    /// "Vulkan 1.4 on AMD Radeon 860M (RADV)" — for MAYO_VERBOSE.
    [[nodiscard]] virtual std::string description() const = 0;
    [[nodiscard]] virtual int maxTextureSize() const = 0;

    [[nodiscard]] virtual std::unique_ptr<Texture> createTexture(const TextureDesc& desc) = 0;
    /// Replaces a region of level 0. `rowBytes` 0 means tightly packed. Data row 0 goes to texture row 0.
    /// Draws recorded earlier in the frame still see the old contents.
    virtual void upload(Texture& texture, int x, int y, int w, int h, const void* data, int rowBytes = 0) = 0;
    [[nodiscard]] virtual bool supportsDmaBuf() const { return false; }
    /// Wraps a dma-buf plane in a texture without copying; null when it can't be imported.
    [[nodiscard]] virtual std::unique_ptr<Texture> importDmaBuf(const DmaBufPlane& /*plane*/) { return nullptr; }

    [[nodiscard]] virtual std::unique_ptr<Pipeline> createPipeline(const PipelineDesc& desc) = 0;

    /// A window's frame: everything recorded between these goes to the GPU together, and endFrame shows
    /// `scene` (same pixel size as the window) in the window.
    virtual void beginFrame(platform::PlatformWindow& window) = 0;
    virtual void endFrame(platform::PlatformWindow& window, Texture& scene) = 0;
    /// Drops what the device keeps for a window (swapchain, surface); call before the window goes away.
    virtual void releaseWindow(platform::PlatformWindow& /*window*/) {}
    /// Reclaims completed temporary GPU resources without blocking. False requests another idle attempt.
    virtual bool trimIdleResources() { return true; }

    /// Directs the following draws at `target`, cleared first when `clear` (premultiplied RGBA) is given.
    virtual void beginPass(Texture& target, const float* clear = nullptr) = 0;
    /// Limits the following draws in this pass; null = the whole target.
    virtual void setScissor(const Scissor* scissor) = 0;
    /// `instances` holds instanceCount * instanceVec4s * 16 bytes. `textures` may hold fewer entries than the
    /// pipeline samples; the rest are bound to a white 1x1 texture.
    virtual void draw(Pipeline& pipeline, std::span<Texture* const> textures, const void* uniforms,
                      std::size_t uniformBytes, std::span<const std::byte> instances, int vertexCount,
                      int instanceCount) = 0;
    /// Finishes the GPU work recorded so far and returns the texture as top-down RGBA8.
    [[nodiscard]] virtual std::vector<std::uint8_t> readPixels(Texture& texture) = 0;
};

/// The device every window renders with, or null before the first window / after the App is gone.
[[nodiscard]] Device* device();
void setDevice(Device* device);

/// Vulkan when this build has it, the GPU suits (Vulkan 1.3, hardware, can present) and MAYO_RENDERER
/// doesn't say "gl"; null otherwise. Call before creating windows: they are created for the chosen API.
[[nodiscard]] std::unique_ptr<Device> createVulkanDevice();
/// Needs the first window (and with it the GL context) to exist.
[[nodiscard]] std::unique_ptr<Device> createGLDevice();

}  // namespace mayo::rhi
