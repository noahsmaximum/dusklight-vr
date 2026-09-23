#include "interop.hpp"

#include "mods/svc/log.hpp"

#include <jni.h>

#include <android/hardware_buffer.h>
#define VK_USE_PLATFORM_ANDROID_KHR
#include <vulkan/vulkan.h>

#include <jni.h>

#define XR_USE_GRAPHICS_API_VULKAN
#define XR_USE_PLATFORM_ANDROID
#include <openxr/openxr.h>
#include <openxr/openxr_platform.h>

#include <array>
#include <iterator>

#include <unistd.h>
#include <cstring>

// Android frame handoff, entirely through public APIs (the Android build of Dusklight exports no
// engine internals):
//
//   - Render targets are AHardwareBuffers, imported into Dusklight's Dawn device as shared texture
//     memory and into the Vulkan device OpenXR creates as a VkImage.
//   - Dawn composites into them. Its EndAccess hands us sync-fd fences, which we import as Vulkan
//     semaphores so the copy waits for that work.
//   - The copy into the runtime's swapchain images runs on the OpenXR device; it signals a semaphore
//     whose sync fd Dawn waits on before rendering into the targets again (no CPU stall).
//
// Needs a Dusklight build that enables Dawn's shared-texture features (android/patch-dusklight.sh).
namespace vr::interop {
namespace {

constexpr uint32_t kCopyRing = 3;

struct TargetNative {
    AHardwareBuffer* buffer = nullptr;
    WGPUSharedTextureMemory memory = nullptr;
    bool accessOpen = false; // Dawn currently owns the texture
    bool everUsed = false;   // false until Dawn has rendered into it once
    VkImageLayout layout = VK_IMAGE_LAYOUT_UNDEFINED; // what Dawn left the image in
    VkImage image = VK_NULL_HANDLE;
    VkDeviceMemory imageMemory = VK_NULL_HANDLE;
};

struct State {
    WGPUDevice device = nullptr;

    // The Vulkan device belongs to OpenXR (created through xrCreateVulkanDeviceKHR), not to Dawn.
    VkInstance instance = VK_NULL_HANDLE;
    VkPhysicalDevice physicalDevice = VK_NULL_HANDLE;
    VkDevice vkDevice = VK_NULL_HANDLE;
    VkQueue queue = VK_NULL_HANDLE;
    uint32_t queueFamily = 0;

    VkCommandPool commandPool = VK_NULL_HANDLE;
    std::array<VkCommandBuffer, kCopyRing> commandBuffers{};
    std::array<VkFence, kCopyRing> fences{};
    uint32_t slot = 0;

    PFN_vkGetAndroidHardwareBufferPropertiesANDROID getAhbProperties = nullptr;
    PFN_vkImportSemaphoreFdKHR importSemaphoreFd = nullptr;
    PFN_vkGetSemaphoreFdKHR getSemaphoreFd = nullptr;

    // Our copy signals done[slot]; its sync fd becomes the fence Dawn waits on before rendering into
    // the targets again, so neither side stalls on the CPU. Without export support: CPU wait.
    std::array<VkSemaphore, kCopyRing> done{};
    bool exportDone = false;
    // Dawn fences imported for a copy, destroyed once that copy slot is reused.
    std::array<std::vector<VkSemaphore>, kCopyRing> slotWaits{};

    XrGraphicsBindingVulkan2KHR binding{XR_TYPE_GRAPHICS_BINDING_VULKAN2_KHR};
    std::vector<FormatChoice> formats;
};
State g;

TargetNative* native_of(const Target& t) { return static_cast<TargetNative*>(t.native); }

bool vk_ok(VkResult r, const char* what) {
    if (r == VK_SUCCESS) {
        return true;
    }
    mods::log::error("Vulkan {} failed ({})", what, static_cast<int>(r));
    return false;
}

template <class Fn>
Fn device_proc(const char* name) {
    return reinterpret_cast<Fn>(vkGetDeviceProcAddr(g.vkDevice, name));
}

// Dawn hands out its work's completion as sync-fd fences; import them as Vulkan semaphores so the
// copy waits for the composition instead of stalling the CPU.
bool import_dawn_fences(WGPUSharedTextureMemoryEndAccessState& state, std::vector<VkSemaphore>& out) {
    for (size_t i = 0; i < state.fenceCount; ++i) {
        WGPUSharedFenceExportInfo exportInfo = WGPU_SHARED_FENCE_EXPORT_INFO_INIT;
        WGPUSharedFenceSyncFDExportInfo syncFd = WGPU_SHARED_FENCE_SYNC_FD_EXPORT_INFO_INIT;
        exportInfo.nextInChain = &syncFd.chain;
        wgpuSharedFenceExportInfo(state.fences[i], &exportInfo);
        if (exportInfo.type != WGPUSharedFenceType_SyncFD || syncFd.handle < 0) {
            continue;
        }
        VkSemaphore semaphore = VK_NULL_HANDLE;
        VkSemaphoreCreateInfo sci{VK_STRUCTURE_TYPE_SEMAPHORE_CREATE_INFO};
        if (!vk_ok(vkCreateSemaphore(g.vkDevice, &sci, nullptr, &semaphore), "vkCreateSemaphore")) {
            return false;
        }
        VkImportSemaphoreFdInfoKHR import{VK_STRUCTURE_TYPE_IMPORT_SEMAPHORE_FD_INFO_KHR};
        import.semaphore = semaphore;
        import.flags = VK_SEMAPHORE_IMPORT_TEMPORARY_BIT;
        import.handleType = VK_EXTERNAL_SEMAPHORE_HANDLE_TYPE_SYNC_FD_BIT;
        // A successful import takes ownership of the fd, but the one Dawn reports still belongs to its
        // fence (freed with the end-access state). Hand Vulkan a duplicate, or both close it: Android's
        // fdsan aborts on the second close, and the submit before that fails on the stale fd.
        import.fd = dup(syncFd.handle);
        if (import.fd < 0) {
            mods::log::error("dup of a Dawn sync fd failed");
            vkDestroySemaphore(g.vkDevice, semaphore, nullptr);
            return false;
        }
        if (!vk_ok(g.importSemaphoreFd(g.vkDevice, &import), "vkImportSemaphoreFdKHR")) {
            close(import.fd); // a failed import leaves the fd with us
            vkDestroySemaphore(g.vkDevice, semaphore, nullptr);
            return false;
        }
        out.push_back(semaphore);
    }
    return true;
}

void barrier(VkCommandBuffer cmd, VkImage image, VkImageLayout from, VkImageLayout to, VkAccessFlags srcAccess,
    VkAccessFlags dstAccess) {
    VkImageMemoryBarrier b{VK_STRUCTURE_TYPE_IMAGE_MEMORY_BARRIER};
    b.srcAccessMask = srcAccess;
    b.dstAccessMask = dstAccess;
    b.oldLayout = from;
    b.newLayout = to;
    b.srcQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED;
    b.dstQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED;
    b.image = image;
    b.subresourceRange = {VK_IMAGE_ASPECT_COLOR_BIT, 0, 1, 0, 1};
    vkCmdPipelineBarrier(cmd, VK_PIPELINE_STAGE_ALL_COMMANDS_BIT, VK_PIPELINE_STAGE_ALL_COMMANDS_BIT, 0, 0, nullptr, 0,
        nullptr, 1, &b);
}

// Gives the texture back to Dawn for the next frame. `copyDone` is the fence for our copy out of it
// (null when the copy already finished on the CPU timeline).
bool begin_access_texture(TargetNative& native, WGPUTexture texture, WGPUSharedFence copyDone = nullptr) {
    if (native.accessOpen || texture == nullptr) {
        return true;
    }
    // Dawn needs to know the image layout it is taking over, and what to leave it in.
    WGPUSharedTextureMemoryVkImageLayoutBeginState layout =
        WGPU_SHARED_TEXTURE_MEMORY_VK_IMAGE_LAYOUT_BEGIN_STATE_INIT;
    layout.oldLayout = native.everUsed ? VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL : VK_IMAGE_LAYOUT_UNDEFINED;
    layout.newLayout = VK_IMAGE_LAYOUT_COLOR_ATTACHMENT_OPTIMAL;
    WGPUSharedTextureMemoryBeginAccessDescriptor begin = WGPU_SHARED_TEXTURE_MEMORY_BEGIN_ACCESS_DESCRIPTOR_INIT;
    begin.nextInChain = &layout.chain;
    begin.initialized = native.everUsed;
    const uint64_t signaled = 1; // sync fds are binary
    if (copyDone != nullptr) {
        begin.fenceCount = 1;
        begin.fences = &copyDone;
        begin.signaledValues = &signaled;
    } else {
        begin.fenceCount = 0;
    }
    if (wgpuSharedTextureMemoryBeginAccess(native.memory, texture, &begin) != WGPUStatus_Success) {
        mods::log::error("wgpuSharedTextureMemoryBeginAccess failed");
        return false;
    }
    native.accessOpen = true;
    return true;
}

} // namespace

const char* const* required_extensions(uint32_t& count) {
    static const char* const kExtensions[] = {XR_KHR_VULKAN_ENABLE2_EXTENSION_NAME};
    count = 1;
    return kExtensions;
}

const char* backend_name() { return "Vulkan"; }

bool initialize(WGPUDevice device, WGPUAdapter adapter) {
    WGPUAdapterInfo info = WGPU_ADAPTER_INFO_INIT;
    if (wgpuAdapterGetInfo(adapter, &info) != WGPUStatus_Success || info.backendType != WGPUBackendType_Vulkan) {
        mods::log::error("VR needs Dusklight's Vulkan graphics backend on Android");
        return false;
    }
    if (!wgpuDeviceHasFeature(device, WGPUFeatureName_SharedTextureMemoryAHardwareBuffer) ||
        !wgpuDeviceHasFeature(device, WGPUFeatureName_SharedFenceSyncFD))
    {
        mods::log::error(
            "This Dusklight build cannot share textures with OpenXR. Use the Dusklight VR edition "
            "build (see docs/android.md).");
        return false;
    }
    g.device = device;
    g.formats = {
        {VK_FORMAT_R8G8B8A8_SRGB, WGPUTextureFormat_RGBA8Unorm},
        {VK_FORMAT_R8G8B8A8_UNORM, WGPUTextureFormat_RGBA8Unorm},
    };
    return true;
}

void shutdown() {
    wait_idle();
    if (g.vkDevice != VK_NULL_HANDLE) {
        for (auto& waits : g.slotWaits) {
            for (VkSemaphore s : waits) {
                vkDestroySemaphore(g.vkDevice, s, nullptr);
            }
            waits.clear();
        }
        for (VkSemaphore& s : g.done) {
            if (s != VK_NULL_HANDLE) {
                vkDestroySemaphore(g.vkDevice, s, nullptr);
                s = VK_NULL_HANDLE;
            }
        }
        g.exportDone = false;
        for (VkFence f : g.fences) {
            if (f != VK_NULL_HANDLE) {
                vkDestroyFence(g.vkDevice, f, nullptr);
            }
        }
        g.fences = {};
        if (g.commandPool != VK_NULL_HANDLE) {
            vkDestroyCommandPool(g.vkDevice, g.commandPool, nullptr);
            g.commandPool = VK_NULL_HANDLE;
        }
        vkDestroyDevice(g.vkDevice, nullptr);
        g.vkDevice = VK_NULL_HANDLE;
    }
    if (g.instance != VK_NULL_HANDLE) {
        vkDestroyInstance(g.instance, nullptr);
        g.instance = VK_NULL_HANDLE;
    }
    g.device = nullptr;
}

bool prepare_system(uintptr_t instance, uint64_t systemId, const void*& graphicsBinding) {
    auto xrInstance = reinterpret_cast<XrInstance>(instance);
    auto proc = [xrInstance](const char* name) {
        PFN_xrVoidFunction fn = nullptr;
        xrGetInstanceProcAddr(xrInstance, name, &fn);
        return fn;
    };
    auto getRequirements = reinterpret_cast<PFN_xrGetVulkanGraphicsRequirements2KHR>(
        proc("xrGetVulkanGraphicsRequirements2KHR"));
    auto createVkInstance = reinterpret_cast<PFN_xrCreateVulkanInstanceKHR>(proc("xrCreateVulkanInstanceKHR"));
    auto getVkPhysicalDevice =
        reinterpret_cast<PFN_xrGetVulkanGraphicsDevice2KHR>(proc("xrGetVulkanGraphicsDevice2KHR"));
    auto createVkDevice = reinterpret_cast<PFN_xrCreateVulkanDeviceKHR>(proc("xrCreateVulkanDeviceKHR"));
    if (getRequirements == nullptr || createVkInstance == nullptr || getVkPhysicalDevice == nullptr ||
        createVkDevice == nullptr)
    {
        mods::log::error("OpenXR runtime is missing XR_KHR_vulkan_enable2 entry points");
        return false;
    }
    XrGraphicsRequirementsVulkan2KHR reqs{XR_TYPE_GRAPHICS_REQUIREMENTS_VULKAN2_KHR};
    if (XR_FAILED(getRequirements(xrInstance, systemId, &reqs))) {
        mods::log::error("OpenXR xrGetVulkanGraphicsRequirements2KHR failed");
        return false;
    }

    // The runtime creates the instance/device so its own required extensions are enabled.
    VkApplicationInfo appInfo{VK_STRUCTURE_TYPE_APPLICATION_INFO};
    appInfo.pApplicationName = "Dusklight VR";
    appInfo.apiVersion = VK_API_VERSION_1_1;
    VkInstanceCreateInfo instanceInfo{VK_STRUCTURE_TYPE_INSTANCE_CREATE_INFO};
    instanceInfo.pApplicationInfo = &appInfo;
    XrVulkanInstanceCreateInfoKHR instanceCreate{XR_TYPE_VULKAN_INSTANCE_CREATE_INFO_KHR};
    instanceCreate.systemId = systemId;
    instanceCreate.pfnGetInstanceProcAddr = &vkGetInstanceProcAddr;
    instanceCreate.vulkanCreateInfo = &instanceInfo;
    VkResult vkResult = VK_SUCCESS;
    if (XR_FAILED(createVkInstance(xrInstance, &instanceCreate, &g.instance, &vkResult)) || vkResult != VK_SUCCESS) {
        mods::log::error("OpenXR could not create its Vulkan instance ({})", static_cast<int>(vkResult));
        return false;
    }

    XrVulkanGraphicsDeviceGetInfoKHR deviceGet{XR_TYPE_VULKAN_GRAPHICS_DEVICE_GET_INFO_KHR};
    deviceGet.systemId = systemId;
    deviceGet.vulkanInstance = g.instance;
    if (XR_FAILED(getVkPhysicalDevice(xrInstance, &deviceGet, &g.physicalDevice))) {
        mods::log::error("OpenXR could not report its Vulkan physical device");
        return false;
    }

    uint32_t familyCount = 0;
    vkGetPhysicalDeviceQueueFamilyProperties(g.physicalDevice, &familyCount, nullptr);
    std::vector<VkQueueFamilyProperties> families(familyCount);
    vkGetPhysicalDeviceQueueFamilyProperties(g.physicalDevice, &familyCount, families.data());
    g.queueFamily = 0;
    for (uint32_t i = 0; i < familyCount; ++i) {
        if ((families[i].queueFlags & VK_QUEUE_GRAPHICS_BIT) != 0) {
            g.queueFamily = i;
            break;
        }
    }

    const float priority = 1.0f;
    VkDeviceQueueCreateInfo queueInfo{VK_STRUCTURE_TYPE_DEVICE_QUEUE_CREATE_INFO};
    queueInfo.queueFamilyIndex = g.queueFamily;
    queueInfo.queueCount = 1;
    queueInfo.pQueuePriorities = &priority;
    // Importing AHardwareBuffers and Dawn's sync-fd fences.
    const char* deviceExtensions[] = {
        VK_ANDROID_EXTERNAL_MEMORY_ANDROID_HARDWARE_BUFFER_EXTENSION_NAME,
        VK_EXT_QUEUE_FAMILY_FOREIGN_EXTENSION_NAME,
        VK_KHR_EXTERNAL_MEMORY_FD_EXTENSION_NAME,
        VK_KHR_EXTERNAL_SEMAPHORE_FD_EXTENSION_NAME,
        VK_KHR_SAMPLER_YCBCR_CONVERSION_EXTENSION_NAME,
    };
    VkDeviceCreateInfo deviceInfo{VK_STRUCTURE_TYPE_DEVICE_CREATE_INFO};
    deviceInfo.queueCreateInfoCount = 1;
    deviceInfo.pQueueCreateInfos = &queueInfo;
    deviceInfo.enabledExtensionCount = static_cast<uint32_t>(std::size(deviceExtensions));
    deviceInfo.ppEnabledExtensionNames = deviceExtensions;
    XrVulkanDeviceCreateInfoKHR deviceCreate{XR_TYPE_VULKAN_DEVICE_CREATE_INFO_KHR};
    deviceCreate.systemId = systemId;
    deviceCreate.pfnGetInstanceProcAddr = &vkGetInstanceProcAddr;
    deviceCreate.vulkanPhysicalDevice = g.physicalDevice;
    deviceCreate.vulkanCreateInfo = &deviceInfo;
    if (XR_FAILED(createVkDevice(xrInstance, &deviceCreate, &g.vkDevice, &vkResult)) || vkResult != VK_SUCCESS) {
        mods::log::error("OpenXR could not create its Vulkan device ({})", static_cast<int>(vkResult));
        return false;
    }
    vkGetDeviceQueue(g.vkDevice, g.queueFamily, 0, &g.queue);

    g.getAhbProperties = device_proc<PFN_vkGetAndroidHardwareBufferPropertiesANDROID>(
        "vkGetAndroidHardwareBufferPropertiesANDROID");
    g.importSemaphoreFd = device_proc<PFN_vkImportSemaphoreFdKHR>("vkImportSemaphoreFdKHR");
    g.getSemaphoreFd = device_proc<PFN_vkGetSemaphoreFdKHR>("vkGetSemaphoreFdKHR");
    if (g.getAhbProperties == nullptr || g.importSemaphoreFd == nullptr) {
        mods::log::error("Vulkan device is missing the AHardwareBuffer / sync-fd entry points");
        return false;
    }

    VkCommandPoolCreateInfo poolInfo{VK_STRUCTURE_TYPE_COMMAND_POOL_CREATE_INFO};
    poolInfo.flags = VK_COMMAND_POOL_CREATE_RESET_COMMAND_BUFFER_BIT;
    poolInfo.queueFamilyIndex = g.queueFamily;
    if (!vk_ok(vkCreateCommandPool(g.vkDevice, &poolInfo, nullptr, &g.commandPool), "vkCreateCommandPool")) {
        return false;
    }
    VkCommandBufferAllocateInfo alloc{VK_STRUCTURE_TYPE_COMMAND_BUFFER_ALLOCATE_INFO};
    alloc.commandPool = g.commandPool;
    alloc.level = VK_COMMAND_BUFFER_LEVEL_PRIMARY;
    alloc.commandBufferCount = kCopyRing;
    if (!vk_ok(vkAllocateCommandBuffers(g.vkDevice, &alloc, g.commandBuffers.data()), "vkAllocateCommandBuffers")) {
        return false;
    }
    for (uint32_t i = 0; i < kCopyRing; ++i) {
        VkFenceCreateInfo fenceInfo{VK_STRUCTURE_TYPE_FENCE_CREATE_INFO};
        fenceInfo.flags = VK_FENCE_CREATE_SIGNALED_BIT;
        if (!vk_ok(vkCreateFence(g.vkDevice, &fenceInfo, nullptr, &g.fences[i]), "vkCreateFence")) {
            return false;
        }
    }
    // Exportable "copy done" semaphores (sync fd); fall back to the CPU wait if the driver can't.
    VkPhysicalDeviceExternalSemaphoreInfo extInfo{VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_EXTERNAL_SEMAPHORE_INFO};
    extInfo.handleType = VK_EXTERNAL_SEMAPHORE_HANDLE_TYPE_SYNC_FD_BIT;
    VkExternalSemaphoreProperties extProps{VK_STRUCTURE_TYPE_EXTERNAL_SEMAPHORE_PROPERTIES};
    vkGetPhysicalDeviceExternalSemaphoreProperties(g.physicalDevice, &extInfo, &extProps);
    g.exportDone = g.getSemaphoreFd != nullptr &&
                   (extProps.externalSemaphoreFeatures & VK_EXTERNAL_SEMAPHORE_FEATURE_EXPORTABLE_BIT) != 0;
    for (uint32_t i = 0; i < kCopyRing && g.exportDone; ++i) {
        VkExportSemaphoreCreateInfo exportInfo{VK_STRUCTURE_TYPE_EXPORT_SEMAPHORE_CREATE_INFO};
        exportInfo.handleTypes = VK_EXTERNAL_SEMAPHORE_HANDLE_TYPE_SYNC_FD_BIT;
        VkSemaphoreCreateInfo sci{VK_STRUCTURE_TYPE_SEMAPHORE_CREATE_INFO};
        sci.pNext = &exportInfo;
        g.exportDone = vk_ok(vkCreateSemaphore(g.vkDevice, &sci, nullptr, &g.done[i]), "vkCreateSemaphore (export)");
    }
    mods::log::info("Vulkan copy sync: {}", g.exportDone ? "semaphore handed to Dawn" : "CPU wait");

    g.binding = {XR_TYPE_GRAPHICS_BINDING_VULKAN2_KHR};
    g.binding.instance = g.instance;
    g.binding.physicalDevice = g.physicalDevice;
    g.binding.device = g.vkDevice;
    g.binding.queueFamilyIndex = g.queueFamily;
    g.binding.queueIndex = 0;
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
    mods::log::info("mark: create_target {} {}x{}", label, width, height);
    destroy_target(out);
    auto* native = new TargetNative();

    AHardwareBuffer_Desc desc{};
    desc.width = width;
    desc.height = height;
    desc.layers = 1;
    desc.format = AHARDWAREBUFFER_FORMAT_R8G8B8A8_UNORM;
    desc.usage = AHARDWAREBUFFER_USAGE_GPU_COLOR_OUTPUT | AHARDWAREBUFFER_USAGE_GPU_SAMPLED_IMAGE;
    if (AHardwareBuffer_allocate(&desc, &native->buffer) != 0 || native->buffer == nullptr) {
        mods::log::error("could not allocate an AHardwareBuffer for {}", label);
        delete native;
        return false;
    }

    // Dawn side: import as shared memory, then create the texture the render worker draws into.
    WGPUSharedTextureMemoryAHardwareBufferDescriptor ahb =
        WGPU_SHARED_TEXTURE_MEMORY_A_HARDWARE_BUFFER_DESCRIPTOR_INIT;
    ahb.handle = native->buffer;
    WGPUSharedTextureMemoryDescriptor memoryDesc = WGPU_SHARED_TEXTURE_MEMORY_DESCRIPTOR_INIT;
    memoryDesc.nextInChain = &ahb.chain;
    memoryDesc.label = {label, WGPU_STRLEN};
    native->memory = wgpuDeviceImportSharedTextureMemory(g.device, &memoryDesc);
    if (native->memory == nullptr) {
        mods::log::error("Dawn could not import the shared texture memory for {}", label);
        AHardwareBuffer_release(native->buffer);
        delete native;
        return false;
    }
    WGPUTextureDescriptor textureDesc = WGPU_TEXTURE_DESCRIPTOR_INIT;
    textureDesc.label = {label, WGPU_STRLEN};
    textureDesc.usage = WGPUTextureUsage_RenderAttachment | WGPUTextureUsage_TextureBinding | WGPUTextureUsage_CopySrc;
    textureDesc.dimension = WGPUTextureDimension_2D;
    textureDesc.size = {width, height, 1};
    textureDesc.format = format;
    textureDesc.mipLevelCount = 1;
    textureDesc.sampleCount = 1;
    out.texture = wgpuSharedTextureMemoryCreateTexture(native->memory, &textureDesc);
    if (out.texture == nullptr) {
        mods::log::error("Dawn could not create a texture from the shared memory for {}", label);
        wgpuSharedTextureMemoryRelease(native->memory);
        AHardwareBuffer_release(native->buffer);
        delete native;
        return false;
    }
    out.view = wgpuTextureCreateView(out.texture, nullptr);

    // Vulkan side: the same buffer as a VkImage on the OpenXR device.
    VkAndroidHardwareBufferPropertiesANDROID props{VK_STRUCTURE_TYPE_ANDROID_HARDWARE_BUFFER_PROPERTIES_ANDROID};
    if (!vk_ok(g.getAhbProperties(g.vkDevice, native->buffer, &props), "vkGetAndroidHardwareBufferPropertiesANDROID")) {
        destroy_target(out);
        return false;
    }
    VkExternalMemoryImageCreateInfo external{VK_STRUCTURE_TYPE_EXTERNAL_MEMORY_IMAGE_CREATE_INFO};
    external.handleTypes = VK_EXTERNAL_MEMORY_HANDLE_TYPE_ANDROID_HARDWARE_BUFFER_BIT_ANDROID;
    VkImageCreateInfo imageInfo{VK_STRUCTURE_TYPE_IMAGE_CREATE_INFO};
    imageInfo.pNext = &external;
    imageInfo.imageType = VK_IMAGE_TYPE_2D;
    imageInfo.format = VK_FORMAT_R8G8B8A8_UNORM;
    imageInfo.extent = {width, height, 1};
    imageInfo.mipLevels = 1;
    imageInfo.arrayLayers = 1;
    imageInfo.samples = VK_SAMPLE_COUNT_1_BIT;
    imageInfo.tiling = VK_IMAGE_TILING_OPTIMAL;
    imageInfo.usage = VK_IMAGE_USAGE_TRANSFER_SRC_BIT | VK_IMAGE_USAGE_COLOR_ATTACHMENT_BIT;
    imageInfo.sharingMode = VK_SHARING_MODE_EXCLUSIVE;
    imageInfo.initialLayout = VK_IMAGE_LAYOUT_UNDEFINED;
    if (!vk_ok(vkCreateImage(g.vkDevice, &imageInfo, nullptr, &native->image), "vkCreateImage")) {
        destroy_target(out);
        return false;
    }
    VkImportAndroidHardwareBufferInfoANDROID importInfo{
        VK_STRUCTURE_TYPE_IMPORT_ANDROID_HARDWARE_BUFFER_INFO_ANDROID};
    importInfo.buffer = native->buffer;
    VkMemoryDedicatedAllocateInfo dedicated{VK_STRUCTURE_TYPE_MEMORY_DEDICATED_ALLOCATE_INFO};
    dedicated.pNext = &importInfo;
    dedicated.image = native->image;
    VkMemoryAllocateInfo allocInfo{VK_STRUCTURE_TYPE_MEMORY_ALLOCATE_INFO};
    allocInfo.pNext = &dedicated;
    allocInfo.allocationSize = props.allocationSize;
    VkPhysicalDeviceMemoryProperties memoryProps{};
    vkGetPhysicalDeviceMemoryProperties(g.physicalDevice, &memoryProps);
    allocInfo.memoryTypeIndex = UINT32_MAX;
    for (uint32_t i = 0; i < memoryProps.memoryTypeCount; ++i) {
        if ((props.memoryTypeBits & (1u << i)) != 0) {
            allocInfo.memoryTypeIndex = i;
            break;
        }
    }
    if (allocInfo.memoryTypeIndex == UINT32_MAX ||
        !vk_ok(vkAllocateMemory(g.vkDevice, &allocInfo, nullptr, &native->imageMemory), "vkAllocateMemory") ||
        !vk_ok(vkBindImageMemory(g.vkDevice, native->image, native->imageMemory, 0), "vkBindImageMemory"))
    {
        destroy_target(out);
        return false;
    }

    out.width = width;
    out.height = height;
    out.format = format;
    out.native = native;
    // Dawn owns the texture until the first copy.
    begin_access_texture(*native, out.texture);
    return true;
}

void destroy_target(Target& target) {
    if (target.native != nullptr) {
        auto* native = native_of(target);
        if (g.vkDevice != VK_NULL_HANDLE) {
            if (native->image != VK_NULL_HANDLE) {
                vkDestroyImage(g.vkDevice, native->image, nullptr);
            }
            if (native->imageMemory != VK_NULL_HANDLE) {
                vkFreeMemory(g.vkDevice, native->imageMemory, nullptr);
            }
        }
        if (native->memory != nullptr) {
            wgpuSharedTextureMemoryRelease(native->memory);
        }
        if (native->buffer != nullptr) {
            AHardwareBuffer_release(native->buffer);
        }
        delete native;
    }
    if (target.view != nullptr) {
        wgpuTextureViewRelease(target.view);
    }
    if (target.texture != nullptr) {
        wgpuTextureRelease(target.texture);
    }
    target = {};
}

bool copy_to_swapchains(const std::vector<CopyJob>& jobs) {
    static bool _first = true;
    if (_first) {
        _first = false;
        mods::log::info("mark: first copy_to_swapchains ({} jobs)", jobs.size());
    }
    if (jobs.empty() || g.vkDevice == VK_NULL_HANDLE) {
        return false;
    }

    // Take the targets back from Dawn; its fences tell us when the composition is done.
    std::vector<VkSemaphore> waits;
    for (const auto& job : jobs) {
        auto* native = native_of(*job.source);
        if (!native->accessOpen) {
            continue;
        }
        WGPUSharedTextureMemoryVkImageLayoutEndState endLayout =
            WGPU_SHARED_TEXTURE_MEMORY_VK_IMAGE_LAYOUT_END_STATE_INIT;
        WGPUSharedTextureMemoryEndAccessState end = WGPU_SHARED_TEXTURE_MEMORY_END_ACCESS_STATE_INIT;
        end.nextInChain = &endLayout.chain;
        if (wgpuSharedTextureMemoryEndAccess(native->memory, job.source->texture, &end) != WGPUStatus_Success) {
            mods::log::error("wgpuSharedTextureMemoryEndAccess failed");
            return false;
        }
        native->accessOpen = false;
        native->everUsed = true;
        native->layout = static_cast<VkImageLayout>(endLayout.newLayout);
        import_dawn_fences(end, waits);
        wgpuSharedTextureMemoryEndAccessStateFreeMembers(end);
    }

    const uint32_t slot = g.slot;
    g.slot = (slot + 1) % kCopyRing;
    VkFence fence = g.fences[slot];
    vkWaitForFences(g.vkDevice, 1, &fence, VK_TRUE, UINT64_MAX);
    vkResetFences(g.vkDevice, 1, &fence);
    for (VkSemaphore s : g.slotWaits[slot]) { // that copy has finished waiting on them
        vkDestroySemaphore(g.vkDevice, s, nullptr);
    }
    g.slotWaits[slot].clear();
    VkCommandBuffer cmd = g.commandBuffers[slot];
    vkResetCommandBuffer(cmd, 0);
    VkCommandBufferBeginInfo begin{VK_STRUCTURE_TYPE_COMMAND_BUFFER_BEGIN_INFO};
    begin.flags = VK_COMMAND_BUFFER_USAGE_ONE_TIME_SUBMIT_BIT;
    vkBeginCommandBuffer(cmd, &begin);
    for (const auto& job : jobs) {
        auto* native = native_of(*job.source);
        auto destination = reinterpret_cast<VkImage>(job.destination);
        barrier(cmd, native->image, native->layout, VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL,
            VK_ACCESS_COLOR_ATTACHMENT_WRITE_BIT, VK_ACCESS_TRANSFER_READ_BIT);
        barrier(cmd, destination, VK_IMAGE_LAYOUT_UNDEFINED, VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL, 0,
            VK_ACCESS_TRANSFER_WRITE_BIT);
        VkImageCopy region{};
        region.srcSubresource = {VK_IMAGE_ASPECT_COLOR_BIT, 0, 0, 1};
        region.dstSubresource = {VK_IMAGE_ASPECT_COLOR_BIT, 0, 0, 1};
        region.extent = {job.source->width, job.source->height, 1};
        vkCmdCopyImage(cmd, native->image, VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL, destination,
            VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL, 1, &region);
        // The runtime expects its images back as colour attachments.
        barrier(cmd, destination, VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL, VK_IMAGE_LAYOUT_COLOR_ATTACHMENT_OPTIMAL,
            VK_ACCESS_TRANSFER_WRITE_BIT, VK_ACCESS_COLOR_ATTACHMENT_WRITE_BIT);
    }
    vkEndCommandBuffer(cmd);

    std::vector<VkPipelineStageFlags> waitStages(waits.size(), VK_PIPELINE_STAGE_TRANSFER_BIT);
    VkSubmitInfo submit{VK_STRUCTURE_TYPE_SUBMIT_INFO};
    submit.waitSemaphoreCount = static_cast<uint32_t>(waits.size());
    submit.pWaitSemaphores = waits.empty() ? nullptr : waits.data();
    submit.pWaitDstStageMask = waitStages.empty() ? nullptr : waitStages.data();
    submit.commandBufferCount = 1;
    submit.pCommandBuffers = &cmd;
    if (g.exportDone) {
        submit.signalSemaphoreCount = 1;
        submit.pSignalSemaphores = &g.done[slot];
    }
    const bool submitted = vk_ok(vkQueueSubmit(g.queue, 1, &submit, fence), "vkQueueSubmit");
    // The waits stay alive until this slot's fence says the copy is done.
    g.slotWaits[slot] = std::move(waits);

    // Dawn may render into the targets again once the copy has read them: hand it our semaphore as
    // a sync-fd fence, or (no export support / export failed) wait for the copy here.
    WGPUSharedFence copyDone = nullptr;
    if (submitted && g.exportDone) {
        VkSemaphoreGetFdInfoKHR get{VK_STRUCTURE_TYPE_SEMAPHORE_GET_FD_INFO_KHR};
        get.semaphore = g.done[slot];
        get.handleType = VK_EXTERNAL_SEMAPHORE_HANDLE_TYPE_SYNC_FD_BIT;
        int fd = -1;
        if (!vk_ok(g.getSemaphoreFd(g.vkDevice, &get, &fd), "vkGetSemaphoreFdKHR")) {
            // The semaphore is left signalled and can't be reused: stay on the CPU wait from now on.
            g.exportDone = false;
            mods::log::warn("Vulkan copy sync: falling back to a CPU wait");
        } else if (fd >= 0) { // -1: the copy has already completed
            WGPUSharedFenceSyncFDDescriptor syncFd = WGPU_SHARED_FENCE_SYNC_FD_DESCRIPTOR_INIT;
            syncFd.handle = fd;
            WGPUSharedFenceDescriptor desc{};
            desc.nextInChain = &syncFd.chain;
            copyDone = wgpuDeviceImportSharedFence(g.device, &desc); // Dawn keeps a duplicate
            close(fd);
        }
    }
    if (submitted && copyDone == nullptr) {
        vkWaitForFences(g.vkDevice, 1, &fence, VK_TRUE, UINT64_MAX);
    }

    // Give the targets back to Dawn for the next frame.
    for (const auto& job : jobs) {
        begin_access_texture(*native_of(*job.source), job.source->texture, copyDone);
    }
    if (copyDone != nullptr) {
        wgpuSharedFenceRelease(copyDone);
    }
    return submitted;
}

void wait_idle() {
    if (g.vkDevice != VK_NULL_HANDLE) {
        vkDeviceWaitIdle(g.vkDevice);
    }
}

bool read_center_pixel(const Target& target, uint8_t rgba[4]) {
    // Debug helper for the desktop simulation only; not needed on Android.
    (void)target;
    (void)rgba;
    return false;
}

} // namespace vr::interop
