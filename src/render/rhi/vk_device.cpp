#include "vk_device.hpp"

#include <algorithm>
#include <bit>
#include <cmath>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <mutex>
#include <string_view>

#include "../../platform/platform.hpp"

namespace mayo::rhi {

std::unique_ptr<Device> createVulkanDevice() {
    const char* choice = std::getenv("MAYO_RENDERER");
    if (choice && (std::string_view(choice) == "gl" || std::string_view(choice) == "opengl")) return nullptr;
    return vk::VulkanDevice::create();
}

}  // namespace mayo::rhi

namespace mayo::rhi::vk {

namespace {

constexpr VkFormat kTargetFormat = VK_FORMAT_R8G8B8A8_UNORM;
constexpr VkDeviceSize kChunkBytes = 1ull << 20;
constexpr std::uint64_t kAcquireTimeout = 100'000'000;  // ns; hidden Wayland windows may never release images

// Textures may die on any thread (an ImageData dropped by a loader thread); this guards the device pointer they
// report to and the lists they touch.
std::mutex g_lifetime;
VulkanDevice* g_device = nullptr;

bool verbose() {
    const char* v = std::getenv("MAYO_VERBOSE");
    return v && *v && *v != '0';
}

void report(const char* why) {
    if (verbose()) std::fprintf(stderr, "mayonnaise: not using Vulkan: %s\n", why);
}

VkFormat vkFormat(Format f) {
    switch (f) {
        case Format::R8: return VK_FORMAT_R8_UNORM;
        case Format::RG8: return VK_FORMAT_R8G8_UNORM;
        case Format::R16: return VK_FORMAT_R16_UNORM;
        case Format::RG16: return VK_FORMAT_R16G16_UNORM;
        case Format::RGBA8: return VK_FORMAT_R8G8B8A8_UNORM;
    }
    return VK_FORMAT_R8G8B8A8_UNORM;
}

bool hasExtension(const std::vector<VkExtensionProperties>& list, const char* name) {
    return std::any_of(list.begin(), list.end(),
                       [&](const VkExtensionProperties& e) { return std::strcmp(e.extensionName, name) == 0; });
}

std::vector<VkExtensionProperties> deviceExtensions(VkPhysicalDevice device) {
    std::uint32_t count = 0;
    vkEnumerateDeviceExtensionProperties(device, nullptr, &count, nullptr);
    std::vector<VkExtensionProperties> list(count);
    vkEnumerateDeviceExtensionProperties(device, nullptr, &count, list.data());
    return list;
}

VkImageSubresourceRange allLevels(std::uint32_t levels) {
    return {VK_IMAGE_ASPECT_COLOR_BIT, 0, levels, 0, 1};
}

/// A barrier that orders everything before it against everything after it. UI frames are tiny; precise stage
/// masks wouldn't be measurable, and this can't be wrong.
VkImageMemoryBarrier2 fullBarrier(VkImage image, VkImageLayout from, VkImageLayout to, std::uint32_t levels) {
    VkImageMemoryBarrier2 b{VK_STRUCTURE_TYPE_IMAGE_MEMORY_BARRIER_2};
    b.srcStageMask = VK_PIPELINE_STAGE_2_ALL_COMMANDS_BIT;
    b.srcAccessMask = VK_ACCESS_2_MEMORY_WRITE_BIT;
    b.dstStageMask = VK_PIPELINE_STAGE_2_ALL_COMMANDS_BIT;
    b.dstAccessMask = VK_ACCESS_2_MEMORY_READ_BIT | VK_ACCESS_2_MEMORY_WRITE_BIT;
    b.oldLayout = from;
    b.newLayout = to;
    b.srcQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED;
    b.dstQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED;
    b.image = image;
    b.subresourceRange = allLevels(levels);
    return b;
}

void barrier(VkCommandBuffer cmd, const VkImageMemoryBarrier2& b) {
    VkDependencyInfo dep{VK_STRUCTURE_TYPE_DEPENDENCY_INFO};
    dep.imageMemoryBarrierCount = 1;
    dep.pImageMemoryBarriers = &b;
    vkCmdPipelineBarrier2(cmd, &dep);
}

}  // namespace

// --- textures --------------------------------------------------------------------------------

VulkanTexture::VulkanTexture(const TextureDesc& desc, VulkanDevice& owner) : Texture(desc), m_owner(&owner) {}

VulkanTexture::~VulkanTexture() {
    std::lock_guard lock(g_lifetime);
    if (g_device == m_owner) m_owner->retire(*this);
}

void VulkanDevice::registerTexture(VulkanTexture& t) {
    std::lock_guard lock(g_lifetime);
    m_live.insert(&t);
}

void VulkanDevice::retire(VulkanTexture& t) {
    m_live.erase(&t);
    m_orphans.push_back({t.image, t.view, t.allocation, t.ownMemory, VK_NULL_HANDLE});
    t.image = VK_NULL_HANDLE;
    t.view = VK_NULL_HANDLE;
}

// --- creation --------------------------------------------------------------------------------

std::unique_ptr<VulkanDevice> VulkanDevice::create() {
    std::unique_ptr<VulkanDevice> device(new VulkanDevice());
    if (!device->init()) {
        // Whatever init() created is torn down by the destructor; the platform goes back to GL.
        device.reset();
        platform::Platform::endVulkanDriverRestriction();
        return nullptr;
    }
    return device;
}

bool VulkanDevice::init() {
    using platform::Platform;
    Platform::restrictVulkanDrivers();
    if (!Platform::loadVulkan()) return report("no Vulkan loader"), false;
    if (!loadGlobal(reinterpret_cast<PFN_vkGetInstanceProcAddr>(Platform::vulkanGetInstanceProcAddr())))
        return report("loader lacks entry points"), false;
    std::uint32_t apiVersion = 0;
    vkEnumerateInstanceVersion(&apiVersion);
    if (apiVersion < VK_API_VERSION_1_3) return report("the loader is older than Vulkan 1.3"), false;

    std::vector<const char*> extensions = Platform::vulkanInstanceExtensions();
    if (extensions.empty()) return report("SDL has no Vulkan surface support"), false;
    std::vector<const char*> layers;
    if (const char* v = std::getenv("MAYO_VK_VALIDATION"); v && *v == '1') {
        layers.push_back("VK_LAYER_KHRONOS_validation");
    }

    VkApplicationInfo app{VK_STRUCTURE_TYPE_APPLICATION_INFO};
    app.pApplicationName = "mayonnaise";
    app.pEngineName = "mayonnaise";
    app.apiVersion = VK_API_VERSION_1_3;
    VkInstanceCreateInfo ici{VK_STRUCTURE_TYPE_INSTANCE_CREATE_INFO};
    ici.pApplicationInfo = &app;
    ici.enabledExtensionCount = static_cast<std::uint32_t>(extensions.size());
    ici.ppEnabledExtensionNames = extensions.data();
    ici.enabledLayerCount = static_cast<std::uint32_t>(layers.size());
    ici.ppEnabledLayerNames = layers.data();
    if (vkCreateInstance(&ici, nullptr, &m_instance) != VK_SUCCESS) return report("vkCreateInstance failed"), false;
    if (!loadInstance(m_instance)) return report("instance lacks entry points"), false;

    std::uint32_t count = 0;
    vkEnumeratePhysicalDevices(m_instance, &count, nullptr);
    std::vector<VkPhysicalDevice> physicals(count);
    vkEnumeratePhysicalDevices(m_instance, &count, physicals.data());
    Platform::endVulkanDriverRestriction();

    // The first suitable device in the loader's order: Mesa's device-select layer puts the GPU that drives the
    // display first, and PRIME offload (DRI_PRIME, prime-run) reorders it, as with GL.
    const char* reason = "no GPU with Vulkan 1.3";
    for (VkPhysicalDevice pd : physicals) {
        VkPhysicalDeviceProperties props;
        vkGetPhysicalDeviceProperties(pd, &props);
        if (props.apiVersion < VK_API_VERSION_1_3) continue;
        if (props.deviceType == VK_PHYSICAL_DEVICE_TYPE_CPU) {
            reason = "only a software Vulkan device";
            continue;
        }
        VkPhysicalDeviceVulkan13Features f13{VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_VULKAN_1_3_FEATURES};
        VkPhysicalDeviceFeatures2 f2{VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_FEATURES_2};
        f2.pNext = &f13;
        vkGetPhysicalDeviceFeatures2(pd, &f2);
        if (!f13.dynamicRendering || !f13.synchronization2) continue;
        if (!hasExtension(deviceExtensions(pd), VK_KHR_SWAPCHAIN_EXTENSION_NAME)) continue;
        VkFormatProperties fp;
        vkGetPhysicalDeviceFormatProperties(pd, kTargetFormat, &fp);
        const VkFormatFeatureFlags need = VK_FORMAT_FEATURE_COLOR_ATTACHMENT_BLEND_BIT |
                                          VK_FORMAT_FEATURE_SAMPLED_IMAGE_FILTER_LINEAR_BIT |
                                          VK_FORMAT_FEATURE_BLIT_SRC_BIT | VK_FORMAT_FEATURE_BLIT_DST_BIT;
        if ((fp.optimalTilingFeatures & need) != need) continue;

        std::uint32_t families = 0;
        vkGetPhysicalDeviceQueueFamilyProperties(pd, &families, nullptr);
        std::vector<VkQueueFamilyProperties> fprops(families);
        vkGetPhysicalDeviceQueueFamilyProperties(pd, &families, fprops.data());
        for (std::uint32_t i = 0; i < families; ++i) {
            if (!(fprops[i].queueFlags & VK_QUEUE_GRAPHICS_BIT)) continue;
            if (!Platform::vulkanPresentationSupport(m_instance, pd, i)) {
                reason = "the GPU can't present to this display";
                continue;
            }
            m_physical = pd;
            m_queueFamily = i;
            m_maxTextureSize = static_cast<int>(props.limits.maxImageDimension2D);
            m_description = std::string("Vulkan ") + std::to_string(VK_API_VERSION_MAJOR(props.apiVersion)) + "." +
                            std::to_string(VK_API_VERSION_MINOR(props.apiVersion)) + " on " + props.deviceName;
            break;
        }
        if (m_physical) break;
    }
    if (!m_physical) return report(reason), false;

    const auto available = deviceExtensions(m_physical);
    std::vector<const char*> enabled{VK_KHR_SWAPCHAIN_EXTENSION_NAME};
    // Zero-copy video: VAAPI frames arrive as dma-bufs with DRM format modifiers.
    m_dmaBuf = hasExtension(available, VK_KHR_EXTERNAL_MEMORY_FD_EXTENSION_NAME) &&
               hasExtension(available, VK_EXT_EXTERNAL_MEMORY_DMA_BUF_EXTENSION_NAME) &&
               hasExtension(available, VK_EXT_IMAGE_DRM_FORMAT_MODIFIER_EXTENSION_NAME);
    if (m_dmaBuf) {
        enabled.push_back(VK_KHR_EXTERNAL_MEMORY_FD_EXTENSION_NAME);
        enabled.push_back(VK_EXT_EXTERNAL_MEMORY_DMA_BUF_EXTENSION_NAME);
        enabled.push_back(VK_EXT_IMAGE_DRM_FORMAT_MODIFIER_EXTENSION_NAME);
        m_foreignQueue = hasExtension(available, VK_EXT_QUEUE_FAMILY_FOREIGN_EXTENSION_NAME);
        if (m_foreignQueue) enabled.push_back(VK_EXT_QUEUE_FAMILY_FOREIGN_EXTENSION_NAME);
    }

    const float priority = 1.0f;
    VkDeviceQueueCreateInfo qci{VK_STRUCTURE_TYPE_DEVICE_QUEUE_CREATE_INFO};
    qci.queueFamilyIndex = m_queueFamily;
    qci.queueCount = 1;
    qci.pQueuePriorities = &priority;
    VkPhysicalDeviceVulkan13Features f13{VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_VULKAN_1_3_FEATURES};
    f13.dynamicRendering = VK_TRUE;
    f13.synchronization2 = VK_TRUE;
    VkDeviceCreateInfo dci{VK_STRUCTURE_TYPE_DEVICE_CREATE_INFO};
    dci.pNext = &f13;
    dci.queueCreateInfoCount = 1;
    dci.pQueueCreateInfos = &qci;
    dci.enabledExtensionCount = static_cast<std::uint32_t>(enabled.size());
    dci.ppEnabledExtensionNames = enabled.data();
    if (vkCreateDevice(m_physical, &dci, nullptr, &m_device) != VK_SUCCESS) return report("vkCreateDevice failed"), false;
    if (!loadDevice(m_device)) return report("device lacks entry points"), false;
    if (m_dmaBuf && !vkGetMemoryFdPropertiesKHR) m_dmaBuf = false;
    vkGetDeviceQueue(m_device, m_queueFamily, 0, &m_queue);
    m_allocator.init(m_physical, m_device);

    for (Slot& slot : m_slots) {
        VkCommandPoolCreateInfo pci{VK_STRUCTURE_TYPE_COMMAND_POOL_CREATE_INFO};
        pci.flags = VK_COMMAND_POOL_CREATE_TRANSIENT_BIT;
        pci.queueFamilyIndex = m_queueFamily;
        if (vkCreateCommandPool(m_device, &pci, nullptr, &slot.pool) != VK_SUCCESS) return false;
        VkCommandBufferAllocateInfo cai{VK_STRUCTURE_TYPE_COMMAND_BUFFER_ALLOCATE_INFO};
        cai.commandPool = slot.pool;
        cai.level = VK_COMMAND_BUFFER_LEVEL_PRIMARY;
        cai.commandBufferCount = 1;
        if (vkAllocateCommandBuffers(m_device, &cai, &slot.cmd) != VK_SUCCESS) return false;
        VkFenceCreateInfo fci{VK_STRUCTURE_TYPE_FENCE_CREATE_INFO};
        if (vkCreateFence(m_device, &fci, nullptr, &slot.fence) != VK_SUCCESS) return false;
        VkSemaphoreCreateInfo sci{VK_STRUCTURE_TYPE_SEMAPHORE_CREATE_INFO};
        if (vkCreateSemaphore(m_device, &sci, nullptr, &slot.acquired) != VK_SUCCESS) return false;
    }

    VkSamplerCreateInfo sci{VK_STRUCTURE_TYPE_SAMPLER_CREATE_INFO};
    sci.addressModeU = sci.addressModeV = sci.addressModeW = VK_SAMPLER_ADDRESS_MODE_CLAMP_TO_EDGE;
    sci.magFilter = sci.minFilter = VK_FILTER_NEAREST;
    sci.mipmapMode = VK_SAMPLER_MIPMAP_MODE_NEAREST;
    if (vkCreateSampler(m_device, &sci, nullptr, &m_nearest) != VK_SUCCESS) return false;
    sci.magFilter = sci.minFilter = VK_FILTER_LINEAR;
    if (vkCreateSampler(m_device, &sci, nullptr, &m_linear) != VK_SUCCESS) return false;
    sci.mipmapMode = VK_SAMPLER_MIPMAP_MODE_LINEAR;
    sci.maxLod = VK_LOD_CLAMP_NONE;
    if (vkCreateSampler(m_device, &sci, nullptr, &m_linearMip) != VK_SUCCESS) return false;

    {
        std::lock_guard lock(g_lifetime);
        g_device = this;
    }
    auto white = createTexture({.filter = Filter::Nearest});
    m_white.reset(static_cast<VulkanTexture*>(white.release()));
    const std::uint8_t pixel[4] = {255, 255, 255, 255};
    upload(*m_white, 0, 0, 1, 1, pixel, 0);
    if (!m_white->image) return false;
    return true;
}

VulkanDevice::~VulkanDevice() {
    if (m_device) vkDeviceWaitIdle(m_device);
    m_white.reset();
    {
        // Textures that outlive the device (cached images) keep their objects nulled out.
        std::lock_guard lock(g_lifetime);
        for (VulkanTexture* t : m_live) {
            m_orphans.push_back({t->image, t->view, t->allocation, t->ownMemory, VK_NULL_HANDLE});
            t->image = VK_NULL_HANDLE;
            t->view = VK_NULL_HANDLE;
        }
        m_live.clear();
        if (g_device == this) g_device = nullptr;
    }
    if (m_device) {
        for (auto& [window, ws] : m_windows) {
            destroyWindowState(ws);
            if (ws.surface) vkDestroySurfaceKHR(m_instance, ws.surface, nullptr);
        }
        m_windows.clear();
        for (Slot& slot : m_slots) {
            collect(slot);
            for (Chunk& c : slot.chunks) {
                vkDestroyBuffer(m_device, c.buffer, nullptr);
                m_allocator.free(c.allocation);
            }
            for (VkDescriptorPool p : slot.descriptorPools) vkDestroyDescriptorPool(m_device, p, nullptr);
            if (slot.acquired) vkDestroySemaphore(m_device, slot.acquired, nullptr);
            if (slot.fence) vkDestroyFence(m_device, slot.fence, nullptr);
            if (slot.pool) vkDestroyCommandPool(m_device, slot.pool, nullptr);
        }
        Slot leftovers;
        leftovers.garbage = std::move(m_orphans);
        collect(leftovers);
        for (VkSampler s : {m_nearest, m_linear, m_linearMip}) {
            if (s) vkDestroySampler(m_device, s, nullptr);
        }
        m_allocator.destroy();
        vkDestroyDevice(m_device, nullptr);
    }
    if (m_instance) vkDestroyInstance(m_instance, nullptr);
}

// --- recording -------------------------------------------------------------------------------

VkCommandBuffer VulkanDevice::cmd() {
    if (!m_recording) startSlot();
    return m_slots[m_slot].cmd;
}

void VulkanDevice::collect(Slot& slot) {
    for (Garbage& g : slot.garbage) {
        if (g.view) vkDestroyImageView(m_device, g.view, nullptr);
        if (g.image) vkDestroyImage(m_device, g.image, nullptr);
        if (g.buffer) vkDestroyBuffer(m_device, g.buffer, nullptr);
        if (g.memory) vkFreeMemory(m_device, g.memory, nullptr);
        m_allocator.free(g.allocation);
    }
    slot.garbage.clear();
}

void VulkanDevice::trimChunks(Slot& slot, bool idle) {
    // Reuse up to two chunks during active rendering, including large video upload buffers. Once idle,
    // retain only small chunks so a one-off photo upload cannot pin hundreds of MB indefinitely.
    size_t kept = 0;
    std::erase_if(slot.chunks, [&](const Chunk& c) {
        if ((!idle || c.size <= kChunkBytes) && kept < 2) {
            ++kept;
            return false;
        }
        vkDestroyBuffer(m_device, c.buffer, nullptr);
        Allocation allocation = c.allocation;
        m_allocator.free(allocation);
        return true;
    });
}

bool VulkanDevice::trimIdleResources() {
    bool complete = true;
    for (size_t i = 0; i < m_slots.size(); ++i) {
        Slot& slot = m_slots[i];
        // Unsubmitted commands still reference their buffers, even if the previous fence is signalled.
        // They need new rendering work to complete, so leave them for the next submission instead of polling.
        if (m_recording && i == m_slot) {
            continue;
        }
        if (slot.submitted && vkWaitForFences(m_device, 1, &slot.fence, VK_TRUE, 0) != VK_SUCCESS) {
            complete = false;
            continue;
        }
        collect(slot);
        trimChunks(slot, true);
    }
    return complete;
}

void VulkanDevice::startSlot() {
    Slot& slot = m_slots[m_slot];
    if (slot.submitted) {
        vkWaitForFences(m_device, 1, &slot.fence, VK_TRUE, UINT64_MAX);
        vkResetFences(m_device, 1, &slot.fence);
        slot.submitted = false;
    }
    collect(slot);
    for (size_t i = 0; i <= slot.descriptorPool && i < slot.descriptorPools.size(); ++i)
        vkResetDescriptorPool(m_device, slot.descriptorPools[i], 0);
    slot.descriptorPool = 0;
    trimChunks(slot, false);
    for (Chunk& c : slot.chunks) c.used = 0;
    slot.chunk = 0;

    vkResetCommandPool(m_device, slot.pool, 0);
    VkCommandBufferBeginInfo bi{VK_STRUCTURE_TYPE_COMMAND_BUFFER_BEGIN_INFO};
    bi.flags = VK_COMMAND_BUFFER_USAGE_ONE_TIME_SUBMIT_BIT;
    vkBeginCommandBuffer(slot.cmd, &bi);
    m_recording = true;
    m_rendering = false;
    m_boundPipeline = VK_NULL_HANDLE;
}

void VulkanDevice::submit(VkSemaphore wait, VkSemaphore signal) {
    cmd();
    endRendering();
    flushClear();
    Slot& slot = m_slots[m_slot];
    {
        std::lock_guard lock(g_lifetime);
        for (Garbage& g : m_orphans) slot.garbage.push_back(g);
        m_orphans.clear();
    }
    vkEndCommandBuffer(slot.cmd);

    VkCommandBufferSubmitInfo cbi{VK_STRUCTURE_TYPE_COMMAND_BUFFER_SUBMIT_INFO};
    cbi.commandBuffer = slot.cmd;
    VkSemaphoreSubmitInfo waitInfo{VK_STRUCTURE_TYPE_SEMAPHORE_SUBMIT_INFO};
    waitInfo.semaphore = wait;
    waitInfo.stageMask = VK_PIPELINE_STAGE_2_BLIT_BIT;  // only the copy into the swapchain image waits
    VkSemaphoreSubmitInfo signalInfo{VK_STRUCTURE_TYPE_SEMAPHORE_SUBMIT_INFO};
    signalInfo.semaphore = signal;
    signalInfo.stageMask = VK_PIPELINE_STAGE_2_ALL_COMMANDS_BIT;
    VkSubmitInfo2 si{VK_STRUCTURE_TYPE_SUBMIT_INFO_2};
    si.commandBufferInfoCount = 1;
    si.pCommandBufferInfos = &cbi;
    si.waitSemaphoreInfoCount = wait ? 1 : 0;
    si.pWaitSemaphoreInfos = &waitInfo;
    si.signalSemaphoreInfoCount = signal ? 1 : 0;
    si.pSignalSemaphoreInfos = &signalInfo;
    if (vkQueueSubmit2(m_queue, 1, &si, slot.fence) != VK_SUCCESS) {
        std::fprintf(stderr, "mayonnaise: vkQueueSubmit2 failed (device lost?)\n");
        std::abort();
    }
    slot.submitted = true;
    m_slot = (m_slot + 1) % kSlots;
    m_recording = false;
    m_rendering = false;
}

VulkanDevice::Transient VulkanDevice::allocTransient(VkDeviceSize size, VkDeviceSize alignment) {
    cmd();
    Slot& slot = m_slots[m_slot];
    for (; slot.chunk < slot.chunks.size(); ++slot.chunk) {
        Chunk& c = slot.chunks[slot.chunk];
        const VkDeviceSize start = (c.used + alignment - 1) / alignment * alignment;
        if (start + size <= c.size) {
            c.used = start + size;
            return {c.buffer, start, static_cast<char*>(c.allocation.mapped) + start};
        }
    }
    Chunk c;
    c.size = std::max(kChunkBytes, std::bit_ceil(size));
    VkBufferCreateInfo bci{VK_STRUCTURE_TYPE_BUFFER_CREATE_INFO};
    bci.size = c.size;
    bci.usage = VK_BUFFER_USAGE_VERTEX_BUFFER_BIT | VK_BUFFER_USAGE_TRANSFER_SRC_BIT;
    bci.sharingMode = VK_SHARING_MODE_EXCLUSIVE;
    VkMemoryRequirements req;
    if (vkCreateBuffer(m_device, &bci, nullptr, &c.buffer) != VK_SUCCESS) std::abort();
    vkGetBufferMemoryRequirements(m_device, c.buffer, &req);
    if (!m_allocator.allocate(req, Usage::Upload, true, c.allocation)) {
        std::fprintf(stderr, "mayonnaise: out of host-visible GPU memory\n");
        std::abort();
    }
    vkBindBufferMemory(m_device, c.buffer, c.allocation.memory, c.allocation.offset);
    c.used = size;
    slot.chunks.push_back(c);
    slot.chunk = slot.chunks.size() - 1;
    return {c.buffer, 0, c.allocation.mapped};
}

void VulkanDevice::transition(VulkanTexture& t, VkImageLayout layout) {
    if (t.layout == layout && !t.foreign) return;
    VkCommandBuffer c = cmd();
    endRendering();
    VkImageMemoryBarrier2 b = fullBarrier(t.image, t.layout, layout, t.mipLevels);
    if (t.foreign) {
        // A dma-buf written by the video decoder: take it over from the foreign (or external) queue family.
        b.oldLayout = VK_IMAGE_LAYOUT_GENERAL;
        b.srcQueueFamilyIndex = m_foreignQueue ? VK_QUEUE_FAMILY_FOREIGN_EXT : VK_QUEUE_FAMILY_EXTERNAL;
        b.dstQueueFamilyIndex = m_queueFamily;
        t.foreign = false;
    }
    barrier(c, b);
    t.layout = layout;
}

void VulkanDevice::flushClear() {
    if (!m_clear || !m_target) return;
    beginRendering();  // the clear happens as the load op
    endRendering();
}

void VulkanDevice::beginRendering() {
    if (m_rendering || !m_target) return;
    VkCommandBuffer c = cmd();
    transition(*m_target, VK_IMAGE_LAYOUT_COLOR_ATTACHMENT_OPTIMAL);
    VkRenderingAttachmentInfo color{VK_STRUCTURE_TYPE_RENDERING_ATTACHMENT_INFO};
    color.imageView = m_target->view;
    color.imageLayout = VK_IMAGE_LAYOUT_COLOR_ATTACHMENT_OPTIMAL;
    color.loadOp = m_clear ? VK_ATTACHMENT_LOAD_OP_CLEAR : VK_ATTACHMENT_LOAD_OP_LOAD;
    color.storeOp = VK_ATTACHMENT_STORE_OP_STORE;
    if (m_clear) std::memcpy(color.clearValue.color.float32, m_clear->data(), sizeof(float) * 4);
    VkRenderingInfo ri{VK_STRUCTURE_TYPE_RENDERING_INFO};
    ri.renderArea = {{0, 0},
                     {static_cast<std::uint32_t>(m_target->width()), static_cast<std::uint32_t>(m_target->height())}};
    ri.layerCount = 1;
    ri.colorAttachmentCount = 1;
    ri.pColorAttachments = &color;
    vkCmdBeginRendering(c, &ri);
    m_clear.reset();
    m_rendering = true;
    m_boundPipeline = VK_NULL_HANDLE;
    VkViewport vp{0, 0, static_cast<float>(m_target->width()), static_cast<float>(m_target->height()), 0, 1};
    vkCmdSetViewport(c, 0, 1, &vp);
    applyScissor();
}

void VulkanDevice::endRendering() {
    if (!m_rendering) return;
    vkCmdEndRendering(m_slots[m_slot].cmd);
    m_rendering = false;
}

void VulkanDevice::applyScissor() {
    if (!m_rendering || !m_target) return;
    VkRect2D r{{0, 0},
               {static_cast<std::uint32_t>(m_target->width()), static_cast<std::uint32_t>(m_target->height())}};
    if (m_scissor) {
        // Same row convention as GL (see rhi.hpp), so GL-style scissors carry over; Vulkan wants them in bounds.
        const int x0 = std::clamp(m_scissor->x, 0, m_target->width());
        const int y0 = std::clamp(m_scissor->y, 0, m_target->height());
        const int x1 = std::clamp(m_scissor->x + m_scissor->width, x0, m_target->width());
        const int y1 = std::clamp(m_scissor->y + m_scissor->height, y0, m_target->height());
        r = {{x0, y0}, {static_cast<std::uint32_t>(x1 - x0), static_cast<std::uint32_t>(y1 - y0)}};
    }
    vkCmdSetScissor(m_slots[m_slot].cmd, 0, 1, &r);
}

// --- textures --------------------------------------------------------------------------------

bool VulkanDevice::createImage(VulkanTexture& t, VkFormat format, VkImageUsageFlags usage) {
    VkImageCreateInfo ici{VK_STRUCTURE_TYPE_IMAGE_CREATE_INFO};
    ici.imageType = VK_IMAGE_TYPE_2D;
    ici.format = format;
    ici.extent = {static_cast<std::uint32_t>(t.width()), static_cast<std::uint32_t>(t.height()), 1};
    ici.mipLevels = t.mipLevels;
    ici.arrayLayers = 1;
    ici.samples = VK_SAMPLE_COUNT_1_BIT;
    ici.tiling = VK_IMAGE_TILING_OPTIMAL;
    ici.usage = usage;
    ici.sharingMode = VK_SHARING_MODE_EXCLUSIVE;
    ici.initialLayout = VK_IMAGE_LAYOUT_UNDEFINED;
    if (vkCreateImage(m_device, &ici, nullptr, &t.image) != VK_SUCCESS) return false;
    VkMemoryRequirements req;
    vkGetImageMemoryRequirements(m_device, t.image, &req);
    if (!m_allocator.allocate(req, Usage::Device, false, t.allocation)) {
        vkDestroyImage(m_device, t.image, nullptr);
        t.image = VK_NULL_HANDLE;
        return false;
    }
    vkBindImageMemory(m_device, t.image, t.allocation.memory, t.allocation.offset);
    VkImageViewCreateInfo vci{VK_STRUCTURE_TYPE_IMAGE_VIEW_CREATE_INFO};
    vci.image = t.image;
    vci.viewType = VK_IMAGE_VIEW_TYPE_2D;
    vci.format = format;
    vci.subresourceRange = allLevels(t.mipLevels);
    vkCreateImageView(m_device, &vci, nullptr, &t.view);
    return true;
}

std::unique_ptr<Texture> VulkanDevice::createTexture(const TextureDesc& requested) {
    TextureDesc desc = requested;
    desc.width = std::clamp(desc.width, 1, m_maxTextureSize);
    desc.height = std::clamp(desc.height, 1, m_maxTextureSize);
    auto t = std::make_unique<VulkanTexture>(desc, *this);
    if (desc.mipmaps)
        t->mipLevels = static_cast<std::uint32_t>(std::bit_width(static_cast<unsigned>(std::max(desc.width, desc.height))));
    VkImageUsageFlags usage =
        VK_IMAGE_USAGE_SAMPLED_BIT | VK_IMAGE_USAGE_TRANSFER_DST_BIT | VK_IMAGE_USAGE_TRANSFER_SRC_BIT;
    if (desc.renderTarget) usage |= VK_IMAGE_USAGE_COLOR_ATTACHMENT_BIT;
    if (!createImage(*t, vkFormat(desc.format), usage)) {
        std::fprintf(stderr, "mayonnaise: could not create a %dx%d texture\n", desc.width, desc.height);
        std::abort();
    }
    registerTexture(*t);
    // Contents start zeroed, as with the GL backend.
    transition(*t, VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL);
    const VkClearColorValue zero{};
    const VkImageSubresourceRange range = allLevels(t->mipLevels);
    vkCmdClearColorImage(cmd(), t->image, VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL, &zero, 1, &range);
    return t;
}

void VulkanDevice::upload(Texture& texture, int x, int y, int w, int h, const void* data, int rowBytes) {
    auto& t = static_cast<VulkanTexture&>(texture);
    if (!t.image || w <= 0 || h <= 0) return;
    const int bpp = bytesPerPixel(t.desc().format);
    const size_t tight = static_cast<size_t>(w) * static_cast<size_t>(bpp);
    const size_t pitch = rowBytes ? static_cast<size_t>(rowBytes) : tight;
    Transient staging = allocTransient(tight * static_cast<size_t>(h), 16);
    auto* dst = static_cast<std::uint8_t*>(staging.data);
    const auto* src = static_cast<const std::uint8_t*>(data);
    for (int row = 0; row < h; ++row)
        std::memcpy(dst + static_cast<size_t>(row) * tight, src + static_cast<size_t>(row) * pitch, tight);

    transition(t, VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL);
    VkBufferImageCopy region{};
    region.bufferOffset = staging.offset;
    region.imageSubresource = {VK_IMAGE_ASPECT_COLOR_BIT, 0, 0, 1};
    region.imageOffset = {x, y, 0};
    region.imageExtent = {static_cast<std::uint32_t>(w), static_cast<std::uint32_t>(h), 1};
    vkCmdCopyBufferToImage(cmd(), staging.buffer, t.image, VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL, 1, &region);
    if (t.mipLevels > 1) generateMipmaps(t);
}

void VulkanDevice::generateMipmaps(VulkanTexture& t) {
    // Expects every level in TRANSFER_DST; leaves every level in SHADER_READ_ONLY.
    VkCommandBuffer c = cmd();
    int w = t.width();
    int h = t.height();
    for (std::uint32_t level = 1; level < t.mipLevels; ++level) {
        VkImageMemoryBarrier2 b = fullBarrier(t.image, VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL,
                                              VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL, 1);
        b.subresourceRange.baseMipLevel = level - 1;
        barrier(c, b);
        const int nw = std::max(1, w / 2);
        const int nh = std::max(1, h / 2);
        VkImageBlit blit{};
        blit.srcSubresource = {VK_IMAGE_ASPECT_COLOR_BIT, level - 1, 0, 1};
        blit.srcOffsets[1] = {w, h, 1};
        blit.dstSubresource = {VK_IMAGE_ASPECT_COLOR_BIT, level, 0, 1};
        blit.dstOffsets[1] = {nw, nh, 1};
        vkCmdBlitImage(c, t.image, VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL, t.image, VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL,
                       1, &blit, VK_FILTER_LINEAR);
        w = nw;
        h = nh;
    }
    VkImageMemoryBarrier2 bs[2];
    bs[0] = fullBarrier(t.image, VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL, VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL,
                        t.mipLevels - 1);
    bs[1] = fullBarrier(t.image, VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL, VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL, 1);
    bs[1].subresourceRange.baseMipLevel = t.mipLevels - 1;
    VkDependencyInfo dep{VK_STRUCTURE_TYPE_DEPENDENCY_INFO};
    dep.imageMemoryBarrierCount = 2;
    dep.pImageMemoryBarriers = bs;
    vkCmdPipelineBarrier2(c, &dep);
    t.layout = VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL;
}

VkSampler VulkanDevice::samplerFor(const VulkanTexture& t) const {
    if (t.mipLevels > 1) return m_linearMip;
    return t.desc().filter == Filter::Nearest ? m_nearest : m_linear;
}

// --- pipelines -------------------------------------------------------------------------------

std::unique_ptr<Pipeline> VulkanDevice::createPipeline(const PipelineDesc& desc) {
    auto p = std::make_unique<VulkanPipeline>();
    p->textures = static_cast<std::uint32_t>(desc.textures.size());
    for (const UniformDesc& u : desc.uniforms)
        p->pushBytes = std::max(p->pushBytes, static_cast<std::uint32_t>(u.offset + u.floats * 4));
    p->instanceStride = static_cast<std::uint32_t>(desc.instanceVec4s) * 16;

    std::vector<VkDescriptorSetLayoutBinding> bindings(p->textures);
    for (std::uint32_t i = 0; i < p->textures; ++i) {
        bindings[i] = {i, VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER, 1, VK_SHADER_STAGE_FRAGMENT_BIT, nullptr};
    }
    VkDescriptorSetLayoutCreateInfo dl{VK_STRUCTURE_TYPE_DESCRIPTOR_SET_LAYOUT_CREATE_INFO};
    dl.bindingCount = p->textures;
    dl.pBindings = bindings.data();
    vkCreateDescriptorSetLayout(m_device, &dl, nullptr, &p->setLayout);
    VkPushConstantRange push{VK_SHADER_STAGE_VERTEX_BIT | VK_SHADER_STAGE_FRAGMENT_BIT, 0, p->pushBytes};
    VkPipelineLayoutCreateInfo pl{VK_STRUCTURE_TYPE_PIPELINE_LAYOUT_CREATE_INFO};
    pl.setLayoutCount = 1;
    pl.pSetLayouts = &p->setLayout;
    pl.pushConstantRangeCount = p->pushBytes ? 1 : 0;
    pl.pPushConstantRanges = &push;
    vkCreatePipelineLayout(m_device, &pl, nullptr, &p->layout);

    auto module = [&](std::span<const unsigned char> code) {
        VkShaderModuleCreateInfo info{VK_STRUCTURE_TYPE_SHADER_MODULE_CREATE_INFO};
        info.codeSize = code.size();
        info.pCode = reinterpret_cast<const std::uint32_t*>(code.data());  // embedded with 16-byte alignment
        VkShaderModule m = VK_NULL_HANDLE;
        vkCreateShaderModule(m_device, &info, nullptr, &m);
        return m;
    };
    VkShaderModule vs = module(desc.vertexSpirv);
    VkShaderModule fs = module(desc.fragmentSpirv);
    VkPipelineShaderStageCreateInfo stages[2]{};
    stages[0] = {VK_STRUCTURE_TYPE_PIPELINE_SHADER_STAGE_CREATE_INFO, nullptr, 0, VK_SHADER_STAGE_VERTEX_BIT, vs,
                 "main", nullptr};
    stages[1] = {VK_STRUCTURE_TYPE_PIPELINE_SHADER_STAGE_CREATE_INFO, nullptr, 0, VK_SHADER_STAGE_FRAGMENT_BIT, fs,
                 "main", nullptr};

    VkVertexInputBindingDescription binding{0, p->instanceStride, VK_VERTEX_INPUT_RATE_INSTANCE};
    std::vector<VkVertexInputAttributeDescription> attributes;
    for (std::uint32_t i = 0; i < static_cast<std::uint32_t>(desc.instanceVec4s); ++i)
        attributes.push_back({i, 0, VK_FORMAT_R32G32B32A32_SFLOAT, i * 16});
    VkPipelineVertexInputStateCreateInfo vi{VK_STRUCTURE_TYPE_PIPELINE_VERTEX_INPUT_STATE_CREATE_INFO};
    if (desc.instanceVec4s > 0) {
        vi.vertexBindingDescriptionCount = 1;
        vi.pVertexBindingDescriptions = &binding;
        vi.vertexAttributeDescriptionCount = static_cast<std::uint32_t>(attributes.size());
        vi.pVertexAttributeDescriptions = attributes.data();
    }
    VkPipelineInputAssemblyStateCreateInfo ia{VK_STRUCTURE_TYPE_PIPELINE_INPUT_ASSEMBLY_STATE_CREATE_INFO};
    ia.topology = desc.topology == Topology::TriangleStrip ? VK_PRIMITIVE_TOPOLOGY_TRIANGLE_STRIP
                                                           : VK_PRIMITIVE_TOPOLOGY_TRIANGLE_LIST;
    VkPipelineViewportStateCreateInfo vp{VK_STRUCTURE_TYPE_PIPELINE_VIEWPORT_STATE_CREATE_INFO};
    vp.viewportCount = 1;
    vp.scissorCount = 1;
    VkPipelineRasterizationStateCreateInfo rs{VK_STRUCTURE_TYPE_PIPELINE_RASTERIZATION_STATE_CREATE_INFO};
    rs.polygonMode = VK_POLYGON_MODE_FILL;
    rs.cullMode = VK_CULL_MODE_NONE;
    rs.lineWidth = 1.0f;
    VkPipelineMultisampleStateCreateInfo ms{VK_STRUCTURE_TYPE_PIPELINE_MULTISAMPLE_STATE_CREATE_INFO};
    ms.rasterizationSamples = VK_SAMPLE_COUNT_1_BIT;
    VkPipelineColorBlendAttachmentState blend{};
    blend.colorWriteMask = VK_COLOR_COMPONENT_R_BIT | VK_COLOR_COMPONENT_G_BIT | VK_COLOR_COMPONENT_B_BIT |
                           VK_COLOR_COMPONENT_A_BIT;
    if (desc.blend == Blend::Premultiplied) {
        blend.blendEnable = VK_TRUE;
        blend.srcColorBlendFactor = blend.srcAlphaBlendFactor = VK_BLEND_FACTOR_ONE;
        blend.dstColorBlendFactor = blend.dstAlphaBlendFactor = VK_BLEND_FACTOR_ONE_MINUS_SRC_ALPHA;
        blend.colorBlendOp = blend.alphaBlendOp = VK_BLEND_OP_ADD;
    }
    VkPipelineColorBlendStateCreateInfo cb{VK_STRUCTURE_TYPE_PIPELINE_COLOR_BLEND_STATE_CREATE_INFO};
    cb.attachmentCount = 1;
    cb.pAttachments = &blend;
    const VkDynamicState dynamic[] = {VK_DYNAMIC_STATE_VIEWPORT, VK_DYNAMIC_STATE_SCISSOR};
    VkPipelineDynamicStateCreateInfo ds{VK_STRUCTURE_TYPE_PIPELINE_DYNAMIC_STATE_CREATE_INFO};
    ds.dynamicStateCount = 2;
    ds.pDynamicStates = dynamic;
    VkPipelineRenderingCreateInfo rendering{VK_STRUCTURE_TYPE_PIPELINE_RENDERING_CREATE_INFO};
    rendering.colorAttachmentCount = 1;
    rendering.pColorAttachmentFormats = &kTargetFormat;

    VkGraphicsPipelineCreateInfo gp{VK_STRUCTURE_TYPE_GRAPHICS_PIPELINE_CREATE_INFO};
    gp.pNext = &rendering;
    gp.stageCount = 2;
    gp.pStages = stages;
    gp.pVertexInputState = &vi;
    gp.pInputAssemblyState = &ia;
    gp.pViewportState = &vp;
    gp.pRasterizationState = &rs;
    gp.pMultisampleState = &ms;
    gp.pColorBlendState = &cb;
    gp.pDynamicState = &ds;
    gp.layout = p->layout;
    const VkResult result = vkCreateGraphicsPipelines(m_device, VK_NULL_HANDLE, 1, &gp, nullptr, &p->pipeline);
    vkDestroyShaderModule(m_device, vs, nullptr);
    vkDestroyShaderModule(m_device, fs, nullptr);
    if (result != VK_SUCCESS) {
        std::fprintf(stderr, "mayonnaise: failed to create the %.*s pipeline\n", static_cast<int>(desc.name.size()),
                     desc.name.data());
        std::abort();
    }
    return p;
}

VkDescriptorSet VulkanDevice::allocateDescriptorSet(VkDescriptorSetLayout layout) {
    Slot& slot = m_slots[m_slot];
    for (;;) {
        if (slot.descriptorPool == slot.descriptorPools.size()) {
            const VkDescriptorPoolSize size{VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER, 768};
            VkDescriptorPoolCreateInfo info{VK_STRUCTURE_TYPE_DESCRIPTOR_POOL_CREATE_INFO};
            info.maxSets = 256;
            info.poolSizeCount = 1;
            info.pPoolSizes = &size;
            VkDescriptorPool pool = VK_NULL_HANDLE;
            if (vkCreateDescriptorPool(m_device, &info, nullptr, &pool) != VK_SUCCESS) std::abort();
            slot.descriptorPools.push_back(pool);
        }
        VkDescriptorSetAllocateInfo ai{VK_STRUCTURE_TYPE_DESCRIPTOR_SET_ALLOCATE_INFO};
        ai.descriptorPool = slot.descriptorPools[slot.descriptorPool];
        ai.descriptorSetCount = 1;
        ai.pSetLayouts = &layout;
        VkDescriptorSet set = VK_NULL_HANDLE;
        if (vkAllocateDescriptorSets(m_device, &ai, &set) == VK_SUCCESS) return set;
        ++slot.descriptorPool;  // full: move on to the next pool (reset with the slot)
    }
}

// --- passes and draws ------------------------------------------------------------------------

void VulkanDevice::beginPass(Texture& target, const float* clear) {
    auto* t = &static_cast<VulkanTexture&>(target);
    cmd();
    if (t != m_target || clear) {
        endRendering();
        flushClear();
        m_target = t;
        if (clear) m_clear = std::array<float, 4>{clear[0], clear[1], clear[2], clear[3]};
    }
    m_scissor.reset();
    applyScissor();
}

void VulkanDevice::setScissor(const Scissor* scissor) {
    if (scissor)
        m_scissor = *scissor;
    else
        m_scissor.reset();
    applyScissor();
}

void VulkanDevice::draw(Pipeline& pipeline, std::span<Texture* const> textures, const void* uniforms,
                        std::size_t uniformBytes, std::span<const std::byte> instances, int vertexCount,
                        int instanceCount) {
    auto& p = static_cast<VulkanPipeline&>(pipeline);
    if (!m_target) return;
    VkCommandBuffer c = cmd();
    VulkanTexture* bound[4] = {};
    for (std::uint32_t i = 0; i < p.textures && i < 4; ++i) {
        Texture* t = i < textures.size() && textures[i] ? textures[i] : m_white.get();
        bound[i] = static_cast<VulkanTexture*>(t);
        if (!bound[i]->image || bound[i] == m_target) bound[i] = m_white.get();
        transition(*bound[i], VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL);  // closes rendering if it must change
    }
    beginRendering();

    if (m_boundPipeline != p.pipeline) {
        vkCmdBindPipeline(c, VK_PIPELINE_BIND_POINT_GRAPHICS, p.pipeline);
        m_boundPipeline = p.pipeline;
    }
    if (p.textures) {
        VkDescriptorSet set = allocateDescriptorSet(p.setLayout);
        VkDescriptorImageInfo infos[4];
        VkWriteDescriptorSet writes[4];
        for (std::uint32_t i = 0; i < p.textures && i < 4; ++i) {
            infos[i] = {samplerFor(*bound[i]), bound[i]->view, VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL};
            writes[i] = {VK_STRUCTURE_TYPE_WRITE_DESCRIPTOR_SET};
            writes[i].dstSet = set;
            writes[i].dstBinding = i;
            writes[i].descriptorCount = 1;
            writes[i].descriptorType = VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER;
            writes[i].pImageInfo = &infos[i];
        }
        vkUpdateDescriptorSets(m_device, std::min(p.textures, 4u), writes, 0, nullptr);
        vkCmdBindDescriptorSets(c, VK_PIPELINE_BIND_POINT_GRAPHICS, p.layout, 0, 1, &set, 0, nullptr);
    }
    if (p.pushBytes && uniforms) {
        const auto bytes = static_cast<std::uint32_t>(std::min<std::size_t>(uniformBytes, p.pushBytes)) & ~3u;
        vkCmdPushConstants(c, p.layout, VK_SHADER_STAGE_VERTEX_BIT | VK_SHADER_STAGE_FRAGMENT_BIT, 0, bytes, uniforms);
    }
    if (p.instanceStride) {
        Transient t = allocTransient(instances.size(), 16);
        std::memcpy(t.data, instances.data(), instances.size());
        vkCmdBindVertexBuffers(c, 0, 1, &t.buffer, &t.offset);
    }
    vkCmdDraw(c, static_cast<std::uint32_t>(vertexCount), static_cast<std::uint32_t>(std::max(instanceCount, 1)), 0, 0);
}

// --- frames ----------------------------------------------------------------------------------

void VulkanDevice::beginFrame(platform::PlatformWindow& /*window*/) { cmd(); }

void VulkanDevice::destroyWindowState(WindowState& ws) {
    for (VkSemaphore s : ws.rendered) vkDestroySemaphore(m_device, s, nullptr);
    ws.rendered.clear();
    if (ws.swapchain) vkDestroySwapchainKHR(m_device, ws.swapchain, nullptr);
    ws.swapchain = VK_NULL_HANDLE;
    ws.images.clear();
}

void VulkanDevice::releaseWindow(platform::PlatformWindow& window) {
    auto it = m_windows.find(&window);
    if (it == m_windows.end()) return;
    vkQueueWaitIdle(m_queue);
    destroyWindowState(it->second);
    if (it->second.surface) vkDestroySurfaceKHR(m_instance, it->second.surface, nullptr);
    m_windows.erase(it);
}

bool VulkanDevice::ensureSwapchain(platform::PlatformWindow& window, WindowState& ws) {
    if (!ws.surface) {
        ws.surface = reinterpret_cast<VkSurfaceKHR>(window.createVulkanSurface(m_instance));
        if (!ws.surface) return false;
        VkBool32 supported = VK_FALSE;
        vkGetPhysicalDeviceSurfaceSupportKHR(m_physical, m_queueFamily, ws.surface, &supported);
        if (!supported) return false;
        ws.transparent = window.isTransparent();
    }
    int pw = 0;
    int ph = 0;
    window.pixelSize(pw, ph);
    if (pw <= 0 || ph <= 0) return false;
    const VkExtent2D want{static_cast<std::uint32_t>(pw), static_cast<std::uint32_t>(ph)};
    if (ws.swapchain && !ws.stale && ws.extent.width == want.width && ws.extent.height == want.height) return true;

    VkSurfaceCapabilitiesKHR caps;
    if (vkGetPhysicalDeviceSurfaceCapabilitiesKHR(m_physical, ws.surface, &caps) != VK_SUCCESS) return false;
    VkExtent2D extent = caps.currentExtent;
    if (extent.width == 0xFFFFFFFFu) {  // Wayland: the client decides
        extent.width = std::clamp(want.width, caps.minImageExtent.width, caps.maxImageExtent.width);
        extent.height = std::clamp(want.height, caps.minImageExtent.height, caps.maxImageExtent.height);
    }
    if (extent.width == 0 || extent.height == 0) return false;
    if (!(caps.supportedUsageFlags & VK_IMAGE_USAGE_TRANSFER_DST_BIT)) return false;

    if (ws.format == VK_FORMAT_UNDEFINED) {
        std::uint32_t n = 0;
        vkGetPhysicalDeviceSurfaceFormatsKHR(m_physical, ws.surface, &n, nullptr);
        std::vector<VkSurfaceFormatKHR> formats(n);
        vkGetPhysicalDeviceSurfaceFormatsKHR(m_physical, ws.surface, &n, formats.data());
        // UNORM like GL's default framebuffer: the colours are already sRGB-encoded.
        for (VkFormat f : {VK_FORMAT_B8G8R8A8_UNORM, VK_FORMAT_R8G8B8A8_UNORM}) {
            VkFormatProperties fp;
            vkGetPhysicalDeviceFormatProperties(m_physical, f, &fp);
            if (!(fp.optimalTilingFeatures & VK_FORMAT_FEATURE_BLIT_DST_BIT)) continue;
            for (const auto& sf : formats) {
                if (sf.format == f && sf.colorSpace == VK_COLOR_SPACE_SRGB_NONLINEAR_KHR) ws.format = f;
            }
            if (ws.format != VK_FORMAT_UNDEFINED) break;
        }
        if (ws.format == VK_FORMAT_UNDEFINED) return false;
    }

    // Rounded corners need the compositor to honour alpha; our pixels are premultiplied.
    VkCompositeAlphaFlagBitsKHR alpha = VK_COMPOSITE_ALPHA_OPAQUE_BIT_KHR;
    const VkCompositeAlphaFlagBitsKHR order[] = {
        VK_COMPOSITE_ALPHA_PRE_MULTIPLIED_BIT_KHR, VK_COMPOSITE_ALPHA_INHERIT_BIT_KHR, VK_COMPOSITE_ALPHA_OPAQUE_BIT_KHR};
    const VkCompositeAlphaFlagBitsKHR opaqueOrder[] = {
        VK_COMPOSITE_ALPHA_OPAQUE_BIT_KHR, VK_COMPOSITE_ALPHA_INHERIT_BIT_KHR, VK_COMPOSITE_ALPHA_PRE_MULTIPLIED_BIT_KHR};
    for (VkCompositeAlphaFlagBitsKHR a : ws.transparent ? order : opaqueOrder) {
        if (caps.supportedCompositeAlpha & a) {
            alpha = a;
            break;
        }
    }

    std::uint32_t images = std::max(caps.minImageCount, 2u);
    if (caps.maxImageCount) images = std::min(images, caps.maxImageCount);

    VkSwapchainKHR old = ws.swapchain;
    VkSwapchainCreateInfoKHR sci{VK_STRUCTURE_TYPE_SWAPCHAIN_CREATE_INFO_KHR};
    sci.surface = ws.surface;
    sci.minImageCount = images;
    sci.imageFormat = ws.format;
    sci.imageColorSpace = VK_COLOR_SPACE_SRGB_NONLINEAR_KHR;
    sci.imageExtent = extent;
    sci.imageArrayLayers = 1;
    sci.imageUsage = VK_IMAGE_USAGE_TRANSFER_DST_BIT;
    sci.imageSharingMode = VK_SHARING_MODE_EXCLUSIVE;
    sci.preTransform = caps.currentTransform;
    sci.compositeAlpha = alpha;
    sci.presentMode = VK_PRESENT_MODE_FIFO_KHR;  // vsync, like SDL_GL_SetSwapInterval(1)
    sci.clipped = VK_TRUE;
    sci.oldSwapchain = old;
    VkSwapchainKHR swapchain = VK_NULL_HANDLE;
    const VkResult result = vkCreateSwapchainKHR(m_device, &sci, nullptr, &swapchain);
    if (old) {
        // Resizes are rare enough that waiting for the GPU beats tracking which old image is still in flight.
        vkQueueWaitIdle(m_queue);
        destroyWindowState(ws);
    }
    if (result != VK_SUCCESS) return false;
    ws.swapchain = swapchain;
    ws.extent = extent;
    ws.stale = false;
    std::uint32_t n = 0;
    vkGetSwapchainImagesKHR(m_device, swapchain, &n, nullptr);
    ws.images.resize(n);
    vkGetSwapchainImagesKHR(m_device, swapchain, &n, ws.images.data());
    ws.rendered.resize(n);
    for (auto& s : ws.rendered) {
        VkSemaphoreCreateInfo info{VK_STRUCTURE_TYPE_SEMAPHORE_CREATE_INFO};
        vkCreateSemaphore(m_device, &info, nullptr, &s);
    }
    return true;
}

void VulkanDevice::endFrame(platform::PlatformWindow& window, Texture& sceneTexture) {
    auto& scene = static_cast<VulkanTexture&>(sceneTexture);
    VkCommandBuffer c = cmd();
    endRendering();
    flushClear();
    m_target = nullptr;

    WindowState& ws = m_windows[&window];
    std::uint32_t index = 0;
    VkSemaphore acquired = m_slots[m_slot].acquired;
    // SDL withholds GL swaps for hidden windows; with Vulkan that's ours to do (Wayland forbids buffers on a
    // surface before it is shown).
    bool present = window.isVisible() && ensureSwapchain(window, ws);
    if (present) {
        VkResult r = vkAcquireNextImageKHR(m_device, ws.swapchain, kAcquireTimeout, acquired, VK_NULL_HANDLE, &index);
        if (r == VK_ERROR_OUT_OF_DATE_KHR) {
            ws.stale = true;
            present = ensureSwapchain(window, ws);
            if (present)
                r = vkAcquireNextImageKHR(m_device, ws.swapchain, kAcquireTimeout, acquired, VK_NULL_HANDLE, &index);
        }
        if (r == VK_SUBOPTIMAL_KHR) ws.stale = true;
        present = present && (r == VK_SUCCESS || r == VK_SUBOPTIMAL_KHR);
    }
    if (!present) {
        submit(VK_NULL_HANDLE, VK_NULL_HANDLE);
        return;
    }

    VkImage target = ws.images[index];
    transition(scene, VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL);
    VkImageMemoryBarrier2 toDst = fullBarrier(target, VK_IMAGE_LAYOUT_UNDEFINED, VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL, 1);
    toDst.srcStageMask = VK_PIPELINE_STAGE_2_BLIT_BIT;  // chained to the acquire semaphore's wait stage
    toDst.srcAccessMask = 0;
    barrier(c, toDst);
    // Flip: the scene's row 0 is the bottom of the window (see rhi.hpp), the swapchain's is the top.
    VkImageBlit blit{};
    blit.srcSubresource = {VK_IMAGE_ASPECT_COLOR_BIT, 0, 0, 1};
    blit.srcOffsets[1] = {scene.width(), scene.height(), 1};
    blit.dstSubresource = {VK_IMAGE_ASPECT_COLOR_BIT, 0, 0, 1};
    blit.dstOffsets[0] = {0, static_cast<std::int32_t>(ws.extent.height), 0};
    blit.dstOffsets[1] = {static_cast<std::int32_t>(ws.extent.width), 0, 1};
    const bool sameSize = static_cast<std::uint32_t>(scene.width()) == ws.extent.width &&
                          static_cast<std::uint32_t>(scene.height()) == ws.extent.height;
    vkCmdBlitImage(c, scene.image, VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL, target, VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL, 1,
                   &blit, sameSize ? VK_FILTER_NEAREST : VK_FILTER_LINEAR);
    VkImageMemoryBarrier2 toPresent =
        fullBarrier(target, VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL, VK_IMAGE_LAYOUT_PRESENT_SRC_KHR, 1);
    toPresent.dstStageMask = VK_PIPELINE_STAGE_2_NONE;
    toPresent.dstAccessMask = 0;
    barrier(c, toPresent);

    VkSemaphore rendered = ws.rendered[index];
    submit(acquired, rendered);
    VkPresentInfoKHR pi{VK_STRUCTURE_TYPE_PRESENT_INFO_KHR};
    pi.waitSemaphoreCount = 1;
    pi.pWaitSemaphores = &rendered;
    pi.swapchainCount = 1;
    pi.pSwapchains = &ws.swapchain;
    pi.pImageIndices = &index;
    const VkResult r = vkQueuePresentKHR(m_queue, &pi);
    if (r == VK_ERROR_OUT_OF_DATE_KHR || r == VK_SUBOPTIMAL_KHR) ws.stale = true;
}

std::vector<std::uint8_t> VulkanDevice::readPixels(Texture& texture) {
    auto& t = static_cast<VulkanTexture&>(texture);
    const int width = t.width();
    const int height = t.height();
    const size_t stride = static_cast<size_t>(width) * 4;
    std::vector<std::uint8_t> pixels(stride * static_cast<size_t>(height));

    VkBuffer buffer = VK_NULL_HANDLE;
    Allocation allocation;
    VkBufferCreateInfo bci{VK_STRUCTURE_TYPE_BUFFER_CREATE_INFO};
    bci.size = pixels.size();
    bci.usage = VK_BUFFER_USAGE_TRANSFER_DST_BIT;
    bci.sharingMode = VK_SHARING_MODE_EXCLUSIVE;
    if (vkCreateBuffer(m_device, &bci, nullptr, &buffer) != VK_SUCCESS) return pixels;
    VkMemoryRequirements req;
    vkGetBufferMemoryRequirements(m_device, buffer, &req);
    if (!m_allocator.allocate(req, Usage::Readback, true, allocation)) {
        vkDestroyBuffer(m_device, buffer, nullptr);
        return pixels;
    }
    vkBindBufferMemory(m_device, buffer, allocation.memory, allocation.offset);

    VkCommandBuffer c = cmd();
    endRendering();
    flushClear();
    transition(t, VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL);
    VkBufferImageCopy region{};
    region.imageSubresource = {VK_IMAGE_ASPECT_COLOR_BIT, 0, 0, 1};
    region.imageExtent = {static_cast<std::uint32_t>(width), static_cast<std::uint32_t>(height), 1};
    vkCmdCopyImageToBuffer(c, t.image, VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL, buffer, 1, &region);
    VkMemoryBarrier2 host{VK_STRUCTURE_TYPE_MEMORY_BARRIER_2};
    host.srcStageMask = VK_PIPELINE_STAGE_2_COPY_BIT;
    host.srcAccessMask = VK_ACCESS_2_TRANSFER_WRITE_BIT;
    host.dstStageMask = VK_PIPELINE_STAGE_2_HOST_BIT;
    host.dstAccessMask = VK_ACCESS_2_HOST_READ_BIT;
    VkDependencyInfo dep{VK_STRUCTURE_TYPE_DEPENDENCY_INFO};
    dep.memoryBarrierCount = 1;
    dep.pMemoryBarriers = &host;
    vkCmdPipelineBarrier2(c, &dep);

    const size_t submitted = m_slot;
    submit(VK_NULL_HANDLE, VK_NULL_HANDLE);
    vkWaitForFences(m_device, 1, &m_slots[submitted].fence, VK_TRUE, UINT64_MAX);
    // Rows come out bottom-up (the GL convention); callers want top-down.
    const auto* src = static_cast<const std::uint8_t*>(allocation.mapped);
    for (int y = 0; y < height; ++y)
        std::memcpy(pixels.data() + static_cast<size_t>(height - 1 - y) * stride, src + static_cast<size_t>(y) * stride,
                    stride);
    vkDestroyBuffer(m_device, buffer, nullptr);
    m_allocator.free(allocation);
    return pixels;
}

}  // namespace mayo::rhi::vk
