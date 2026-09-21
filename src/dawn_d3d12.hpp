#pragma once

#include <webgpu/webgpu.h>

#ifndef WIN32_LEAN_AND_MEAN
#define WIN32_LEAN_AND_MEAN
#endif
#ifndef NOMINMAX
#define NOMINMAX
#endif
#include <d3d12.h>
#include <wrl/client.h>

// Bridges Dawn (webgpu_dawn.dll, the renderer Dusklight uses) to raw D3D12 so the mod can hand
// frames to OpenXR without host support for shared textures:
//   - Dawn's D3D12 device and queue come from dawn::native::d3d12 functions webgpu_dawn.dll exports.
//   - The ID3D12Resource behind a Dawn texture is captured by briefly intercepting the device's
//     resource-creation methods while the mod creates that texture.
namespace vr::d3d {

using Microsoft::WRL::ComPtr;

// True if the device runs on Dawn's D3D12 backend (the only one supported).
bool is_d3d12_backend(WGPUAdapter adapter);

ComPtr<ID3D12Device> device_of(WGPUDevice device);
ComPtr<ID3D12CommandQueue> queue_of(WGPUDevice device);

// Which barrier model the captured resource was created with.
enum class BarrierModel {
    Legacy,   // resource states (CreateCommittedResource/CreatePlacedResource[1]/CreateCommittedResource[1|2])
    Enhanced, // layouts (CreateCommittedResource3/CreatePlacedResource2)
};

struct CapturedTexture {
    WGPUTexture texture = nullptr;     // owned by the caller (wgpuTextureRelease)
    WGPUTextureView view = nullptr;    // owned by the caller
    ComPtr<ID3D12Resource> resource;   // the D3D12 resource Dawn allocated for `texture`
    BarrierModel barriers = BarrierModel::Legacy;
    uint32_t width = 0;
    uint32_t height = 0;
    WGPUTextureFormat format = WGPUTextureFormat_Undefined;

    explicit operator bool() const { return texture != nullptr && resource != nullptr; }
    void reset();
};

// Creates a 2D render-attachment texture through Dawn and captures its D3D12 resource.
bool create_captured_texture(WGPUDevice device, ID3D12Device* d3dDevice, uint32_t width, uint32_t height,
    WGPUTextureFormat format, const char* label, CapturedTexture& out);

} // namespace vr::d3d
