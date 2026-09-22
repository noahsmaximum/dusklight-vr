#pragma once

#include <webgpu/webgpu.h>

#include <cstdint>
#include <vector>

// Everything about handing Dawn's textures to OpenXR that depends on the graphics API.
//
// Windows uses Dawn's D3D12 backend: the ID3D12Resource behind a Dawn texture is captured while the
// texture is created, and the copy into the XR swapchain runs on Dawn's own D3D12 queue
// (interop_d3d12.cpp).
//
// Android uses Dawn's Vulkan backend through Dawn's public shared-texture interface: the render
// targets live in AHardwareBuffers shared between Dawn and the Vulkan device OpenXR created, and the
// copy runs on that device (interop_vulkan.cpp). It needs a Dusklight build that enables Dawn's
// shared-texture features (see docs/android.md).
namespace vr::interop {

// Opaque per-platform handle of an OpenXR swapchain image (ID3D12Resource* / VkImage).
using SwapchainImage = uint64_t;

// A render target the mod composites into and then copies to the runtime.
struct Target {
    WGPUTexture texture = nullptr;  // Dawn side, used by the render worker
    WGPUTextureView view = nullptr;
    uint32_t width = 0;
    uint32_t height = 0;
    WGPUTextureFormat format = WGPUTextureFormat_Undefined;
    void* native = nullptr; // platform payload (owned by the interop layer)

    explicit operator bool() const { return view != nullptr; }
};

struct CopyJob {
    const Target* source = nullptr;
    SwapchainImage destination = 0;
};

// The OpenXR extensions the graphics binding needs (XR_KHR_D3D12_enable / XR_KHR_vulkan_enable2).
const char* const* required_extensions(uint32_t& count);

// Sets up the graphics side. Fails (after logging) when Dusklight runs on another backend, so the
// mod can tell the user which backend to select.
bool initialize(WGPUDevice device, WGPUAdapter adapter);
void shutdown();

// Human-readable backend name for the status line ("D3D12", "Vulkan").
const char* backend_name();

// After xrGetSystem: checks the runtime's graphics requirements against Dusklight's device (and on
// Vulkan creates the device OpenXR wants). Returns the binding to chain into xrCreateSession.
bool prepare_system(uintptr_t instance, uint64_t systemId, const void*& graphicsBinding);

// Swapchain formats the mod can feed, most preferred first, paired with the Dawn format written
// into them. Values are the platform's (DXGI_FORMAT / VkFormat).
struct FormatChoice {
    int64_t native;
    WGPUTextureFormat wgpu;
};
const std::vector<FormatChoice>& candidate_formats();

// Swapchain images as platform handles (XrSwapchainImageD3D12KHR / XrSwapchainImageVulkanKHR).
bool enumerate_swapchain_images(uint64_t swapchain, std::vector<SwapchainImage>& out);

// A Dawn render-attachment texture the interop layer can copy from.
bool create_target(uint32_t width, uint32_t height, WGPUTextureFormat format, const char* label, Target& out);
void destroy_target(Target& target);

// Copies composited targets into the acquired swapchain images, ordered after Dawn's frame work.
// Runs on the render worker right after the frame was submitted.
bool copy_to_swapchains(const std::vector<CopyJob>& jobs);

// Blocks until the interop layer's own GPU work is done (teardown / resource retirement).
void wait_idle();

// Debug: reads the centre pixel of a target (simulation self-test).
bool read_center_pixel(const Target& target, uint8_t rgba[4]);

} // namespace vr::interop
