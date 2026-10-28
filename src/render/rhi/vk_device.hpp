#pragma once

// Vulkan 1.3 implementation of the RHI (dynamic rendering + synchronization2).
//
// Commands are recorded into one command buffer per frame slot and submitted when a window's frame ends. Draws
// open a rendering instance lazily and keep it open until something that can't run inside one (an upload, a
// layout change, another target) closes it, so a frame without materials is a single rendering instance.
// Resource lifetimes follow the slots: a destroyed texture is freed once the slot that was recording when it
// died has finished on the GPU.

#include <array>
#include <memory>
#include <optional>
#include <string>
#include <unordered_map>
#include <unordered_set>
#include <vector>

#include "rhi.hpp"
#include "vk_allocator.hpp"

namespace mayo::rhi::vk {

class VulkanDevice;

class VulkanTexture final : public Texture {
public:
    VulkanTexture(const TextureDesc& desc, VulkanDevice& owner);
    ~VulkanTexture() override;  // any thread

    VkImage image = VK_NULL_HANDLE;
    VkImageView view = VK_NULL_HANDLE;
    Allocation allocation;                     // sub-allocated memory, or
    VkDeviceMemory ownMemory = VK_NULL_HANDLE;  // imported dma-buf memory
    VkImageLayout layout = VK_IMAGE_LAYOUT_UNDEFINED;  // as of the commands recorded so far
    std::uint32_t mipLevels = 1;
    bool foreign = false;  // imported: acquire from the foreign queue family before the first use

private:
    friend class VulkanDevice;
    VulkanDevice* m_owner;
};

class VulkanPipeline final : public Pipeline {
public:
    VkPipeline pipeline = VK_NULL_HANDLE;
    VkPipelineLayout layout = VK_NULL_HANDLE;
    VkDescriptorSetLayout setLayout = VK_NULL_HANDLE;
    std::uint32_t textures = 0;
    std::uint32_t pushBytes = 0;
    std::uint32_t instanceStride = 0;
};

class VulkanDevice final : public Device {
public:
    /// Null when Vulkan isn't usable here; the reason goes to stderr with MAYO_VERBOSE.
    static std::unique_ptr<VulkanDevice> create();
    ~VulkanDevice() override;

    [[nodiscard]] Backend backend() const override { return Backend::Vulkan; }
    [[nodiscard]] std::string description() const override { return m_description; }
    [[nodiscard]] int maxTextureSize() const override { return m_maxTextureSize; }

    std::unique_ptr<Texture> createTexture(const TextureDesc& desc) override;
    void upload(Texture& texture, int x, int y, int w, int h, const void* data, int rowBytes) override;
    [[nodiscard]] bool supportsDmaBuf() const override { return m_dmaBuf; }
    std::unique_ptr<Texture> importDmaBuf(const DmaBufPlane& plane) override;

    std::unique_ptr<Pipeline> createPipeline(const PipelineDesc& desc) override;

    void beginFrame(platform::PlatformWindow& window) override;
    void endFrame(platform::PlatformWindow& window, Texture& scene) override;
    void releaseWindow(platform::PlatformWindow& window) override;
    bool trimIdleResources() override;

    void beginPass(Texture& target, const float* clear) override;
    void setScissor(const Scissor* scissor) override;
    void draw(Pipeline& pipeline, std::span<Texture* const> textures, const void* uniforms, std::size_t uniformBytes,
              std::span<const std::byte> instances, int vertexCount, int instanceCount) override;
    std::vector<std::uint8_t> readPixels(Texture& texture) override;

    /// Tracks a texture so the device can free it if it outlives the device.
    void registerTexture(VulkanTexture& texture);
    /// Queues a dead texture's objects for destruction; the caller holds the lifetime lock.
    void retire(VulkanTexture& texture);

private:
    struct Garbage {
        VkImage image = VK_NULL_HANDLE;
        VkImageView view = VK_NULL_HANDLE;
        Allocation allocation;
        VkDeviceMemory memory = VK_NULL_HANDLE;
        VkBuffer buffer = VK_NULL_HANDLE;
    };
    /// Host-visible memory handed out linearly during one slot (instances, staging) and reset when it's reused.
    struct Chunk {
        VkBuffer buffer = VK_NULL_HANDLE;
        Allocation allocation;
        VkDeviceSize size = 0;
        VkDeviceSize used = 0;
    };
    struct Transient {
        VkBuffer buffer = VK_NULL_HANDLE;
        VkDeviceSize offset = 0;
        void* data = nullptr;
    };
    struct Slot {
        VkCommandPool pool = VK_NULL_HANDLE;
        VkCommandBuffer cmd = VK_NULL_HANDLE;
        VkFence fence = VK_NULL_HANDLE;
        VkSemaphore acquired = VK_NULL_HANDLE;
        bool submitted = false;
        std::vector<VkDescriptorPool> descriptorPools;
        size_t descriptorPool = 0;
        std::vector<Chunk> chunks;
        size_t chunk = 0;
        std::vector<Garbage> garbage;
    };
    struct WindowState {
        VkSurfaceKHR surface = VK_NULL_HANDLE;
        VkSwapchainKHR swapchain = VK_NULL_HANDLE;
        VkFormat format = VK_FORMAT_UNDEFINED;
        VkExtent2D extent{};
        std::vector<VkImage> images;
        std::vector<VkSemaphore> rendered;  // per image: signalled by the frame, waited on by present
        bool stale = false;                 // out of date or suboptimal: recreate before the next frame
        bool transparent = false;
    };

    VulkanDevice() = default;
    bool init();
    void destroyWindowState(WindowState& ws);

    VkCommandBuffer cmd();  // starts recording into the next slot if needed
    void startSlot();
    void submit(VkSemaphore wait, VkSemaphore signal);
    void collect(Slot& slot);
    void trimChunks(Slot& slot, bool idle);
    Transient allocTransient(VkDeviceSize size, VkDeviceSize alignment);

    void transition(VulkanTexture& texture, VkImageLayout layout);
    void beginRendering();
    void endRendering();
    void flushClear();
    void applyScissor();
    void generateMipmaps(VulkanTexture& texture);
    bool createImage(VulkanTexture& texture, VkFormat format, VkImageUsageFlags usage);
    VkSampler samplerFor(const VulkanTexture& texture) const;
    VkDescriptorSet allocateDescriptorSet(VkDescriptorSetLayout layout);

    bool ensureSwapchain(platform::PlatformWindow& window, WindowState& ws);

    VkInstance m_instance = VK_NULL_HANDLE;
    VkPhysicalDevice m_physical = VK_NULL_HANDLE;
    VkDevice m_device = VK_NULL_HANDLE;
    VkQueue m_queue = VK_NULL_HANDLE;
    std::uint32_t m_queueFamily = 0;
    Allocator m_allocator;
    std::string m_description;
    int m_maxTextureSize = 4096;
    bool m_dmaBuf = false;
    bool m_foreignQueue = false;  // VK_EXT_queue_family_foreign

    VkSampler m_nearest = VK_NULL_HANDLE;
    VkSampler m_linear = VK_NULL_HANDLE;
    VkSampler m_linearMip = VK_NULL_HANDLE;
    std::unique_ptr<VulkanTexture> m_white;

    static constexpr size_t kSlots = 3;
    std::array<Slot, kSlots> m_slots;
    size_t m_slot = 0;
    bool m_recording = false;

    VulkanTexture* m_target = nullptr;
    bool m_rendering = false;
    std::optional<std::array<float, 4>> m_clear;  // pending clear of m_target
    std::optional<Scissor> m_scissor;
    VkPipeline m_boundPipeline = VK_NULL_HANDLE;

    std::unordered_map<platform::PlatformWindow*, WindowState> m_windows;
    std::unordered_set<VulkanTexture*> m_live;  // to free textures that outlive the device; guarded by the lifetime lock
    std::vector<Garbage> m_orphans;      // retired since the last submit; guarded by the lifetime lock
};

}  // namespace mayo::rhi::vk
