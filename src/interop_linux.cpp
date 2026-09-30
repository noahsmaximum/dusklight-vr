#include "interop.hpp"

#include "mods/svc/log.hpp"

#define VK_NO_PROTOTYPES
#include <vulkan/vulkan.h>

#define XR_USE_GRAPHICS_API_VULKAN
#include <openxr/openxr.h>
#include <openxr/openxr_platform.h>

#include <dlfcn.h>

#include <array>
#include <cstring>
#include <string>

// Linux frame handoff: OpenXR runs on Dawn's own Vulkan device, like the D3D12 path on Windows.
//
//   - Dusklight's Linux builds export Dawn's internals (they link with -rdynamic), so the mod reads
//     Dawn's VkInstance/VkPhysicalDevice/VkDevice/VkQueue and the VkImage behind a Dawn texture
//     through a handful of accessors, looked up by name at run time.
//   - The runtime creates its swapchain images on that device (XR_KHR_vulkan_enable), and the copy
//     from our composited targets runs on Dawn's queue right after Dawn submitted the frame, from
//     the same thread. Queue order does the synchronisation; there is nothing to share or import.
//   - Dawn believes its textures stay in the layout it left them in, so the copy puts them back.
//
// This works with the official Dusklight AppImages; no special build is needed.
namespace vr::interop {
namespace {

constexpr uint32_t kCopyRing = 3;

// --- Dawn accessors (Itanium-mangled member functions; `this` is the first argument) ------------

// dawn::native::vulkan::Aspect::Color
constexpr uint8_t kDawnAspectColor = 1;

struct DawnVk {
    const void* (*physicalDevice)(const void* device) = nullptr;       // DeviceBase::GetPhysicalDevice
    VkPhysicalDevice (*vkPhysicalDevice)(const void* physical) = nullptr;
    VkDevice (*vkDevice)(const void* device) = nullptr;
    VkInstance (*vkInstance)(const void* device) = nullptr;
    uint32_t (*queueFamily)(const void* device) = nullptr;
    VkQueue (*vkQueue)(const void* queue) = nullptr;
    VkImage (*image)(const void* texture) = nullptr;
    VkImageLayout (*layout)(const void* texture, uint8_t aspect, uint32_t layer, uint32_t level) = nullptr;

    template <typename Fn>
    static bool find(Fn& out, const char* name) {
        out = reinterpret_cast<Fn>(dlsym(RTLD_DEFAULT, name));
        if (out == nullptr) {
            mods::log::error("Dawn accessor not found: {}", name);
        }
        return out != nullptr;
    }

    bool load() {
        // Evaluate all so every missing name is logged.
        bool ok = find(physicalDevice, "_ZNK4dawn6native10DeviceBase17GetPhysicalDeviceEv");
        ok = find(vkPhysicalDevice, "_ZNK4dawn6native6vulkan14PhysicalDevice19GetVkPhysicalDeviceEv") && ok;
        ok = find(vkDevice, "_ZNK4dawn6native6vulkan6Device11GetVkDeviceEv") && ok;
        ok = find(vkInstance, "_ZNK4dawn6native6vulkan6Device13GetVkInstanceEv") && ok;
        ok = find(queueFamily, "_ZNK4dawn6native6vulkan6Device22GetGraphicsQueueFamilyEv") && ok;
        ok = find(vkQueue, "_ZNK4dawn6native6vulkan5Queue10GetVkQueueEv") && ok;
        ok = find(image, "_ZNK4dawn6native6vulkan7Texture9GetHandleEv") && ok;
        ok = find(layout, "_ZNK4dawn6native6vulkan7Texture16GetCurrentLayoutENS0_6AspectEjj") && ok;
        return ok;
    }
};

// --- Vulkan entry points (from the loader Dawn already opened) -----------------------------------

struct Vk {
    PFN_vkGetInstanceProcAddr getInstanceProcAddr = nullptr;
    PFN_vkGetDeviceProcAddr getDeviceProcAddr = nullptr;
    PFN_vkGetPhysicalDeviceMemoryProperties getPhysicalDeviceMemoryProperties = nullptr;
    PFN_vkGetPhysicalDeviceProperties getPhysicalDeviceProperties = nullptr;
    PFN_vkCreateCommandPool createCommandPool = nullptr;
    PFN_vkDestroyCommandPool destroyCommandPool = nullptr;
    PFN_vkAllocateCommandBuffers allocateCommandBuffers = nullptr;
    PFN_vkResetCommandBuffer resetCommandBuffer = nullptr;
    PFN_vkBeginCommandBuffer beginCommandBuffer = nullptr;
    PFN_vkEndCommandBuffer endCommandBuffer = nullptr;
    PFN_vkCmdPipelineBarrier cmdPipelineBarrier = nullptr;
    PFN_vkCmdCopyImage cmdCopyImage = nullptr;
    PFN_vkCmdCopyImageToBuffer cmdCopyImageToBuffer = nullptr;
    PFN_vkQueueSubmit queueSubmit = nullptr;
    PFN_vkCreateFence createFence = nullptr;
    PFN_vkDestroyFence destroyFence = nullptr;
    PFN_vkWaitForFences waitForFences = nullptr;
    PFN_vkResetFences resetFences = nullptr;
    PFN_vkCreateBuffer createBuffer = nullptr;
    PFN_vkDestroyBuffer destroyBuffer = nullptr;
    PFN_vkGetBufferMemoryRequirements getBufferMemoryRequirements = nullptr;
    PFN_vkAllocateMemory allocateMemory = nullptr;
    PFN_vkFreeMemory freeMemory = nullptr;
    PFN_vkBindBufferMemory bindBufferMemory = nullptr;
    PFN_vkMapMemory mapMemory = nullptr;
    PFN_vkUnmapMemory unmapMemory = nullptr;

    bool load(VkInstance instance, VkDevice device) {
        void* lib = dlopen("libvulkan.so.1", RTLD_NOW | RTLD_NOLOAD);
        if (lib == nullptr) {
            lib = dlopen("libvulkan.so.1", RTLD_NOW);
        }
        if (lib == nullptr) {
            mods::log::error("could not open libvulkan.so.1");
            return false;
        }
        getInstanceProcAddr = reinterpret_cast<PFN_vkGetInstanceProcAddr>(dlsym(lib, "vkGetInstanceProcAddr"));
        if (getInstanceProcAddr == nullptr) {
            return false;
        }
        const auto inst = [&](auto& fn, const char* name) {
            fn = reinterpret_cast<std::remove_reference_t<decltype(fn)>>(getInstanceProcAddr(instance, name));
            return fn != nullptr;
        };
        bool ok = inst(getDeviceProcAddr, "vkGetDeviceProcAddr");
        ok = inst(getPhysicalDeviceMemoryProperties, "vkGetPhysicalDeviceMemoryProperties") && ok;
        ok = inst(getPhysicalDeviceProperties, "vkGetPhysicalDeviceProperties") && ok;
        if (!ok) {
            return false;
        }
        const auto dev = [&](auto& fn, const char* name) {
            fn = reinterpret_cast<std::remove_reference_t<decltype(fn)>>(getDeviceProcAddr(device, name));
            if (fn == nullptr) {
                mods::log::error("Vulkan entry point missing: {}", name);
            }
            return fn != nullptr;
        };
        ok = dev(createCommandPool, "vkCreateCommandPool");
        ok = dev(destroyCommandPool, "vkDestroyCommandPool") && ok;
        ok = dev(allocateCommandBuffers, "vkAllocateCommandBuffers") && ok;
        ok = dev(resetCommandBuffer, "vkResetCommandBuffer") && ok;
        ok = dev(beginCommandBuffer, "vkBeginCommandBuffer") && ok;
        ok = dev(endCommandBuffer, "vkEndCommandBuffer") && ok;
        ok = dev(cmdPipelineBarrier, "vkCmdPipelineBarrier") && ok;
        ok = dev(cmdCopyImage, "vkCmdCopyImage") && ok;
        ok = dev(cmdCopyImageToBuffer, "vkCmdCopyImageToBuffer") && ok;
        ok = dev(queueSubmit, "vkQueueSubmit") && ok;
        ok = dev(createFence, "vkCreateFence") && ok;
        ok = dev(destroyFence, "vkDestroyFence") && ok;
        ok = dev(waitForFences, "vkWaitForFences") && ok;
        ok = dev(resetFences, "vkResetFences") && ok;
        ok = dev(createBuffer, "vkCreateBuffer") && ok;
        ok = dev(destroyBuffer, "vkDestroyBuffer") && ok;
        ok = dev(getBufferMemoryRequirements, "vkGetBufferMemoryRequirements") && ok;
        ok = dev(allocateMemory, "vkAllocateMemory") && ok;
        ok = dev(freeMemory, "vkFreeMemory") && ok;
        ok = dev(bindBufferMemory, "vkBindBufferMemory") && ok;
        ok = dev(mapMemory, "vkMapMemory") && ok;
        ok = dev(unmapMemory, "vkUnmapMemory") && ok;
        return ok;
    }
};

struct TargetNative {
    VkImage image = VK_NULL_HANDLE;
};

struct State {
    WGPUDevice device = nullptr;
    DawnVk dawn;
    Vk vk;

    VkInstance instance = VK_NULL_HANDLE;
    VkPhysicalDevice physicalDevice = VK_NULL_HANDLE;
    VkDevice vkDevice = VK_NULL_HANDLE;
    VkQueue queue = VK_NULL_HANDLE;
    uint32_t queueFamily = 0;

    VkCommandPool commandPool = VK_NULL_HANDLE;
    std::array<VkCommandBuffer, kCopyRing> commandBuffers{};
    std::array<VkFence, kCopyRing> fences{};
    uint32_t slot = 0;

    XrGraphicsBindingVulkanKHR binding{XR_TYPE_GRAPHICS_BINDING_VULKAN_KHR};
    std::vector<FormatChoice> formats;
};
State g;

TargetNative* native_of(const Target& t) { return static_cast<TargetNative*>(t.native); }

bool vk_ok(VkResult r, const char* what) {
    if (r != VK_SUCCESS) {
        mods::log::error("{} failed ({})", what, static_cast<int>(r));
        return false;
    }
    return true;
}

void barrier(VkCommandBuffer cmd, VkImage image, VkImageLayout from, VkImageLayout to, VkAccessFlags srcAccess,
    VkAccessFlags dstAccess)
{
    VkImageMemoryBarrier b{VK_STRUCTURE_TYPE_IMAGE_MEMORY_BARRIER};
    b.srcAccessMask = srcAccess;
    b.dstAccessMask = dstAccess;
    b.oldLayout = from;
    b.newLayout = to;
    b.srcQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED;
    b.dstQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED;
    b.image = image;
    b.subresourceRange = {VK_IMAGE_ASPECT_COLOR_BIT, 0, 1, 0, 1};
    // Full stage masks: the other side is Dawn's (or the runtime's) earlier/later work on this queue.
    g.vk.cmdPipelineBarrier(cmd, VK_PIPELINE_STAGE_ALL_COMMANDS_BIT, VK_PIPELINE_STAGE_ALL_COMMANDS_BIT, 0, 0,
        nullptr, 0, nullptr, 1, &b);
}

bool create_copy_infra() {
    if (g.commandPool != VK_NULL_HANDLE) {
        return true;
    }
    VkCommandPoolCreateInfo poolInfo{VK_STRUCTURE_TYPE_COMMAND_POOL_CREATE_INFO};
    poolInfo.flags = VK_COMMAND_POOL_CREATE_RESET_COMMAND_BUFFER_BIT;
    poolInfo.queueFamilyIndex = g.queueFamily;
    if (!vk_ok(g.vk.createCommandPool(g.vkDevice, &poolInfo, nullptr, &g.commandPool), "vkCreateCommandPool")) {
        return false;
    }
    VkCommandBufferAllocateInfo alloc{VK_STRUCTURE_TYPE_COMMAND_BUFFER_ALLOCATE_INFO};
    alloc.commandPool = g.commandPool;
    alloc.level = VK_COMMAND_BUFFER_LEVEL_PRIMARY;
    alloc.commandBufferCount = kCopyRing;
    if (!vk_ok(g.vk.allocateCommandBuffers(g.vkDevice, &alloc, g.commandBuffers.data()), "vkAllocateCommandBuffers")) {
        return false;
    }
    for (uint32_t i = 0; i < kCopyRing; ++i) {
        VkFenceCreateInfo fenceInfo{VK_STRUCTURE_TYPE_FENCE_CREATE_INFO};
        fenceInfo.flags = VK_FENCE_CREATE_SIGNALED_BIT;
        if (!vk_ok(g.vk.createFence(g.vkDevice, &fenceInfo, nullptr, &g.fences[i]), "vkCreateFence")) {
            return false;
        }
    }
    return true;
}

// Takes the next command buffer of the ring once its previous submission has finished.
VkCommandBuffer begin_commands(VkFence& fence) {
    const uint32_t slot = g.slot;
    g.slot = (slot + 1) % kCopyRing;
    fence = g.fences[slot];
    g.vk.waitForFences(g.vkDevice, 1, &fence, VK_TRUE, UINT64_MAX);
    g.vk.resetFences(g.vkDevice, 1, &fence);
    VkCommandBuffer cmd = g.commandBuffers[slot];
    g.vk.resetCommandBuffer(cmd, 0);
    VkCommandBufferBeginInfo begin{VK_STRUCTURE_TYPE_COMMAND_BUFFER_BEGIN_INFO};
    begin.flags = VK_COMMAND_BUFFER_USAGE_ONE_TIME_SUBMIT_BIT;
    g.vk.beginCommandBuffer(cmd, &begin);
    return cmd;
}

bool submit(VkCommandBuffer cmd, VkFence fence) {
    g.vk.endCommandBuffer(cmd);
    VkSubmitInfo info{VK_STRUCTURE_TYPE_SUBMIT_INFO};
    info.commandBufferCount = 1;
    info.pCommandBuffers = &cmd;
    return vk_ok(g.vk.queueSubmit(g.queue, 1, &info, fence), "vkQueueSubmit");
}

} // namespace

const char* const* required_extensions(uint32_t& count) {
    // enable (not enable2): the Vulkan device already exists, Dawn created it.
    static const char* const kExtensions[] = {XR_KHR_VULKAN_ENABLE_EXTENSION_NAME};
    count = 1;
    return kExtensions;
}

const char* backend_name() { return "Vulkan"; }

bool initialize(WGPUDevice device, WGPUAdapter adapter) {
    WGPUAdapterInfo info = WGPU_ADAPTER_INFO_INIT;
    if (wgpuAdapterGetInfo(adapter, &info) != WGPUStatus_Success || info.backendType != WGPUBackendType_Vulkan) {
        mods::log::error("VR needs Dusklight's Vulkan graphics backend (Settings > Graphics > Backend)");
        return false;
    }
    if (!g.dawn.load()) {
        mods::log::error("This Dusklight build does not expose Dawn's Vulkan device; VR unavailable");
        return false;
    }
    // WGPU handles are Dawn's native objects (dawn::native::vulkan::Device : DeviceBase, etc.).
    g.vkDevice = g.dawn.vkDevice(device);
    g.instance = g.dawn.vkInstance(device);
    g.queueFamily = g.dawn.queueFamily(device);
    g.physicalDevice = g.dawn.vkPhysicalDevice(g.dawn.physicalDevice(device));
    WGPUQueue queue = wgpuDeviceGetQueue(device);
    g.queue = g.dawn.vkQueue(queue);
    wgpuQueueRelease(queue);
    if (g.vkDevice == VK_NULL_HANDLE || g.instance == VK_NULL_HANDLE || g.physicalDevice == VK_NULL_HANDLE ||
        g.queue == VK_NULL_HANDLE)
    {
        mods::log::error("could not reach Dawn's Vulkan device");
        return false;
    }
    if (!g.vk.load(g.instance, g.vkDevice)) {
        mods::log::error("could not load the Vulkan entry points");
        return false;
    }
    VkPhysicalDeviceProperties props{};
    g.vk.getPhysicalDeviceProperties(g.physicalDevice, &props);
    mods::log::info("Vulkan: Dawn's device on {} (queue family {})", props.deviceName, g.queueFamily);
    g.device = device;
    // Dusklight renders gamma-encoded colour into UNORM targets, so an *_SRGB swapchain (fed the
    // same bytes) is what makes the runtime display it at the right brightness. vkCmdCopyImage copies
    // bytes, so the channel order of the target must match the swapchain's.
    g.formats = {
        {VK_FORMAT_R8G8B8A8_SRGB, WGPUTextureFormat_RGBA8Unorm},
        {VK_FORMAT_B8G8R8A8_SRGB, WGPUTextureFormat_BGRA8Unorm},
        {VK_FORMAT_R8G8B8A8_UNORM, WGPUTextureFormat_RGBA8Unorm},
        {VK_FORMAT_B8G8R8A8_UNORM, WGPUTextureFormat_BGRA8Unorm},
    };
    return true;
}

void shutdown() {
    wait_idle();
    if (g.vkDevice != VK_NULL_HANDLE) {
        for (VkFence& f : g.fences) {
            if (f != VK_NULL_HANDLE) {
                g.vk.destroyFence(g.vkDevice, f, nullptr);
                f = VK_NULL_HANDLE;
            }
        }
        if (g.commandPool != VK_NULL_HANDLE) {
            g.vk.destroyCommandPool(g.vkDevice, g.commandPool, nullptr);
            g.commandPool = VK_NULL_HANDLE;
        }
    }
    // The device, instance and queue are Dawn's.
    g.vkDevice = VK_NULL_HANDLE;
    g.instance = VK_NULL_HANDLE;
    g.physicalDevice = VK_NULL_HANDLE;
    g.queue = VK_NULL_HANDLE;
    g.device = nullptr;
}

bool prepare_system(uintptr_t instance, uint64_t systemId, const void*& graphicsBinding) {
    auto xrInstance = reinterpret_cast<XrInstance>(instance);
    auto proc = [xrInstance](const char* name) {
        PFN_xrVoidFunction fn = nullptr;
        xrGetInstanceProcAddr(xrInstance, name, &fn);
        return fn;
    };
    auto getRequirements =
        reinterpret_cast<PFN_xrGetVulkanGraphicsRequirementsKHR>(proc("xrGetVulkanGraphicsRequirementsKHR"));
    auto getDevice = reinterpret_cast<PFN_xrGetVulkanGraphicsDeviceKHR>(proc("xrGetVulkanGraphicsDeviceKHR"));
    auto getInstanceExts =
        reinterpret_cast<PFN_xrGetVulkanInstanceExtensionsKHR>(proc("xrGetVulkanInstanceExtensionsKHR"));
    auto getDeviceExts = reinterpret_cast<PFN_xrGetVulkanDeviceExtensionsKHR>(proc("xrGetVulkanDeviceExtensionsKHR"));
    if (getRequirements == nullptr || getDevice == nullptr) {
        mods::log::error("OpenXR runtime is missing XR_KHR_vulkan_enable entry points");
        return false;
    }
    XrGraphicsRequirementsVulkanKHR reqs{XR_TYPE_GRAPHICS_REQUIREMENTS_VULKAN_KHR};
    if (XR_FAILED(getRequirements(xrInstance, systemId, &reqs))) {
        mods::log::error("OpenXR xrGetVulkanGraphicsRequirementsKHR failed");
        return false;
    }

    // Dawn chose the GPU and the extensions long ago. Log what the runtime would have wanted, which
    // is where to look if swapchain creation fails.
    const auto listed = [xrInstance, systemId](auto fn) {
        if (fn == nullptr) {
            return std::string();
        }
        uint32_t size = 0;
        if (XR_FAILED(fn(xrInstance, systemId, 0, &size, nullptr)) || size == 0) {
            return std::string();
        }
        std::string text(size, '\0');
        fn(xrInstance, systemId, size, &size, text.data());
        text.resize(std::strlen(text.c_str()));
        return text;
    };
    mods::log::info("OpenXR wants Vulkan instance extensions: {}", listed(getInstanceExts));
    mods::log::info("OpenXR wants Vulkan device extensions: {}", listed(getDeviceExts));

    VkPhysicalDevice wanted = VK_NULL_HANDLE;
    if (XR_FAILED(getDevice(xrInstance, systemId, g.instance, &wanted))) {
        mods::log::warn("OpenXR could not report its Vulkan physical device");
    } else if (wanted != g.physicalDevice) {
        mods::log::warn("Dusklight renders on a different GPU than the headset expects; this may not work");
    }

    if (!create_copy_infra()) {
        mods::log::error("could not create the Vulkan copy resources");
        return false;
    }
    g.binding = {XR_TYPE_GRAPHICS_BINDING_VULKAN_KHR};
    g.binding.instance = g.instance;
    g.binding.physicalDevice = g.physicalDevice;
    g.binding.device = g.vkDevice;
    g.binding.queueFamilyIndex = g.queueFamily;
    g.binding.queueIndex = 0; // Dawn creates a single queue
    graphicsBinding = &g.binding;
    return true;
}

const std::vector<FormatChoice>& candidate_formats() { return g.formats; }

bool enumerate_swapchain_images(uint64_t swapchain, std::vector<SwapchainImage>& out) {
    auto handle = reinterpret_cast<XrSwapchain>(swapchain);
    uint32_t count = 0;
    xrEnumerateSwapchainImages(handle, 0, &count, nullptr);
    std::vector<XrSwapchainImageVulkanKHR> images(count, {XR_TYPE_SWAPCHAIN_IMAGE_VULKAN_KHR});
    if (XR_FAILED(xrEnumerateSwapchainImages(
            handle, count, &count, reinterpret_cast<XrSwapchainImageBaseHeader*>(images.data()))))
    {
        return false;
    }
    out.clear();
    for (const auto& img : images) {
        out.push_back(reinterpret_cast<SwapchainImage>(img.image));
    }
    return true;
}

bool create_target(uint32_t width, uint32_t height, WGPUTextureFormat format, const char* label, Target& out) {
    destroy_target(out);
    WGPUTextureDescriptor desc = WGPU_TEXTURE_DESCRIPTOR_INIT;
    desc.label = {label, WGPU_STRLEN};
    desc.usage = WGPUTextureUsage_RenderAttachment | WGPUTextureUsage_TextureBinding | WGPUTextureUsage_CopySrc;
    desc.dimension = WGPUTextureDimension_2D;
    desc.size = {width, height, 1};
    desc.format = format;
    desc.mipLevelCount = 1;
    desc.sampleCount = 1;
    out.texture = wgpuDeviceCreateTexture(g.device, &desc);
    if (out.texture == nullptr) {
        mods::log::error("Dawn could not create {}", label);
        return false;
    }
    auto* native = new TargetNative();
    native->image = g.dawn.image(out.texture);
    if (native->image == VK_NULL_HANDLE) {
        mods::log::error("no VkImage behind {}", label);
        delete native;
        wgpuTextureRelease(out.texture);
        out = {};
        return false;
    }
    out.view = wgpuTextureCreateView(out.texture, nullptr);
    out.width = width;
    out.height = height;
    out.format = format;
    out.native = native;
    return true;
}

void destroy_target(Target& target) {
    delete native_of(target);
    if (target.view != nullptr) {
        wgpuTextureViewRelease(target.view);
    }
    if (target.texture != nullptr) {
        wgpuTextureRelease(target.texture);
    }
    target = {};
}

bool copy_to_swapchains(const std::vector<CopyJob>& jobs) {
    if (jobs.empty() || g.commandPool == VK_NULL_HANDLE) {
        return false;
    }
    VkFence fence = VK_NULL_HANDLE;
    VkCommandBuffer cmd = begin_commands(fence);
    for (const auto& job : jobs) {
        const VkImage source = native_of(*job.source)->image;
        const auto destination = reinterpret_cast<VkImage>(job.destination);
        // What Dawn left the target in (and will assume it still is next frame).
        const VkImageLayout dawnLayout = g.dawn.layout(job.source->texture, kDawnAspectColor, 0, 0);
        if (dawnLayout != VK_IMAGE_LAYOUT_UNDEFINED) {
            barrier(cmd, source, dawnLayout, VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL,
                VK_ACCESS_MEMORY_WRITE_BIT, VK_ACCESS_TRANSFER_READ_BIT);
        }
        // The runtime hands its images over as colour attachments and wants them back that way.
        barrier(cmd, destination, VK_IMAGE_LAYOUT_COLOR_ATTACHMENT_OPTIMAL, VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL, 0,
            VK_ACCESS_TRANSFER_WRITE_BIT);
        if (dawnLayout != VK_IMAGE_LAYOUT_UNDEFINED) { // never rendered: leave the swapchain image as is
            VkImageCopy region{};
            region.srcSubresource = {VK_IMAGE_ASPECT_COLOR_BIT, 0, 0, 1};
            region.dstSubresource = {VK_IMAGE_ASPECT_COLOR_BIT, 0, 0, 1};
            region.extent = {job.source->width, job.source->height, 1};
            g.vk.cmdCopyImage(cmd, source, VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL, destination,
                VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL, 1, &region);
            barrier(cmd, source, VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL, dawnLayout, VK_ACCESS_TRANSFER_READ_BIT,
                VK_ACCESS_MEMORY_READ_BIT | VK_ACCESS_MEMORY_WRITE_BIT);
        }
        barrier(cmd, destination, VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL, VK_IMAGE_LAYOUT_COLOR_ATTACHMENT_OPTIMAL,
            VK_ACCESS_TRANSFER_WRITE_BIT, VK_ACCESS_MEMORY_READ_BIT | VK_ACCESS_MEMORY_WRITE_BIT);
    }
    return submit(cmd, fence);
}

void wait_idle() {
    // Not vkQueueWaitIdle: the queue is Dawn's and may be in use on another thread.
    if (g.vkDevice == VK_NULL_HANDLE || g.commandPool == VK_NULL_HANDLE) {
        return;
    }
    g.vk.waitForFences(g.vkDevice, kCopyRing, g.fences.data(), VK_TRUE, 2'000'000'000ull);
}

bool read_center_pixel(const Target& target, uint8_t rgba[4]) {
    if (!target || g.vkDevice == VK_NULL_HANDLE || !create_copy_infra()) {
        return false;
    }
    // Same path as a real frame (Dawn's queue, right after Dawn's submit), into a host buffer.
    VkBufferCreateInfo bufferInfo{VK_STRUCTURE_TYPE_BUFFER_CREATE_INFO};
    bufferInfo.size = 4;
    bufferInfo.usage = VK_BUFFER_USAGE_TRANSFER_DST_BIT;
    bufferInfo.sharingMode = VK_SHARING_MODE_EXCLUSIVE;
    VkBuffer buffer = VK_NULL_HANDLE;
    if (!vk_ok(g.vk.createBuffer(g.vkDevice, &bufferInfo, nullptr, &buffer), "vkCreateBuffer")) {
        return false;
    }
    VkMemoryRequirements reqs{};
    g.vk.getBufferMemoryRequirements(g.vkDevice, buffer, &reqs);
    VkPhysicalDeviceMemoryProperties memoryProps{};
    g.vk.getPhysicalDeviceMemoryProperties(g.physicalDevice, &memoryProps);
    VkMemoryAllocateInfo allocInfo{VK_STRUCTURE_TYPE_MEMORY_ALLOCATE_INFO};
    allocInfo.allocationSize = reqs.size;
    allocInfo.memoryTypeIndex = UINT32_MAX;
    constexpr VkMemoryPropertyFlags kHost = VK_MEMORY_PROPERTY_HOST_VISIBLE_BIT | VK_MEMORY_PROPERTY_HOST_COHERENT_BIT;
    for (uint32_t i = 0; i < memoryProps.memoryTypeCount; ++i) {
        if ((reqs.memoryTypeBits & (1u << i)) != 0 && (memoryProps.memoryTypes[i].propertyFlags & kHost) == kHost) {
            allocInfo.memoryTypeIndex = i;
            break;
        }
    }
    VkDeviceMemory memory = VK_NULL_HANDLE;
    bool ok = allocInfo.memoryTypeIndex != UINT32_MAX &&
              vk_ok(g.vk.allocateMemory(g.vkDevice, &allocInfo, nullptr, &memory), "vkAllocateMemory") &&
              vk_ok(g.vk.bindBufferMemory(g.vkDevice, buffer, memory, 0), "vkBindBufferMemory");
    const VkImageLayout dawnLayout = g.dawn.layout(target.texture, kDawnAspectColor, 0, 0);
    if (ok && dawnLayout != VK_IMAGE_LAYOUT_UNDEFINED) {
        const VkImage source = native_of(target)->image;
        VkFence fence = VK_NULL_HANDLE;
        VkCommandBuffer cmd = begin_commands(fence);
        barrier(cmd, source, dawnLayout, VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL, VK_ACCESS_MEMORY_WRITE_BIT,
            VK_ACCESS_TRANSFER_READ_BIT);
        VkBufferImageCopy region{};
        region.imageSubresource = {VK_IMAGE_ASPECT_COLOR_BIT, 0, 0, 1};
        region.imageOffset = {static_cast<int32_t>(target.width / 2), static_cast<int32_t>(target.height / 2), 0};
        region.imageExtent = {1, 1, 1};
        g.vk.cmdCopyImageToBuffer(cmd, source, VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL, buffer, 1, &region);
        barrier(cmd, source, VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL, dawnLayout, VK_ACCESS_TRANSFER_READ_BIT,
            VK_ACCESS_MEMORY_READ_BIT | VK_ACCESS_MEMORY_WRITE_BIT);
        ok = submit(cmd, fence) && g.vk.waitForFences(g.vkDevice, 1, &fence, VK_TRUE, 2'000'000'000ull) == VK_SUCCESS;
        void* data = nullptr;
        if (ok && g.vk.mapMemory(g.vkDevice, memory, 0, 4, 0, &data) == VK_SUCCESS) {
            std::memcpy(rgba, data, 4);
            g.vk.unmapMemory(g.vkDevice, memory);
        } else {
            ok = false;
        }
    } else {
        ok = false;
    }
    g.vk.destroyBuffer(g.vkDevice, buffer, nullptr);
    if (memory != VK_NULL_HANDLE) {
        g.vk.freeMemory(g.vkDevice, memory, nullptr);
    }
    return ok;
}

} // namespace vr::interop
