#include "interop.hpp"

#include "dawn_d3d12.hpp"

#include "mods/svc/log.hpp"

#define XR_USE_GRAPHICS_API_D3D12
#define XR_USE_PLATFORM_WIN32
#include <openxr/openxr.h>
#include <openxr/openxr_platform.h>

#include <array>

namespace vr::interop {
namespace {

using d3d::ComPtr;

constexpr uint32_t kCopyRing = 3;

struct State {
    WGPUDevice device = nullptr;
    ComPtr<ID3D12Device> d3dDevice;
    ComPtr<ID3D12CommandQueue> d3dQueue;

    // Copy infrastructure (runs on Dawn's queue so it is ordered after Dawn's frame work).
    std::array<ComPtr<ID3D12CommandAllocator>, kCopyRing> copyAlloc;
    std::array<ComPtr<ID3D12GraphicsCommandList>, kCopyRing> copyList;
    std::array<uint64_t, kCopyRing> copyFenceAt{};
    uint32_t copySlot = 0;
    ComPtr<ID3D12Fence> fence;
    uint64_t fenceValue = 0;
    HANDLE fenceEvent = nullptr;

    XrGraphicsBindingD3D12KHR binding{XR_TYPE_GRAPHICS_BINDING_D3D12_KHR};
    std::vector<FormatChoice> formats;
};
State g;

bool create_copy_infra() {
    if (g.fence) {
        return true;
    }
    if (FAILED(g.d3dDevice->CreateFence(0, D3D12_FENCE_FLAG_NONE, IID_PPV_ARGS(&g.fence)))) {
        return false;
    }
    for (uint32_t i = 0; i < kCopyRing; ++i) {
        if (FAILED(g.d3dDevice->CreateCommandAllocator(D3D12_COMMAND_LIST_TYPE_DIRECT, IID_PPV_ARGS(&g.copyAlloc[i]))) ||
            FAILED(g.d3dDevice->CreateCommandList(
                0, D3D12_COMMAND_LIST_TYPE_DIRECT, g.copyAlloc[i].Get(), nullptr, IID_PPV_ARGS(&g.copyList[i]))))
        {
            return false;
        }
        g.copyList[i]->Close();
    }
    g.fenceEvent = CreateEventW(nullptr, FALSE, FALSE, nullptr);
    g.copyFenceAt = {};
    return true;
}

// Dawn leaves the target in RENDER_TARGET after our composition pass (and still believes it is
// there), so we round-trip it through COPY_SOURCE.
void transition_source(ID3D12GraphicsCommandList* list, const d3d::CapturedTexture& t, bool toCopy) {
    if (t.barriers == d3d::BarrierModel::Enhanced) {
        ComPtr<ID3D12GraphicsCommandList7> list7;
        if (FAILED(list->QueryInterface(IID_PPV_ARGS(&list7)))) {
            return;
        }
        D3D12_TEXTURE_BARRIER b{};
        b.SyncBefore = toCopy ? D3D12_BARRIER_SYNC_ALL : D3D12_BARRIER_SYNC_COPY;
        b.SyncAfter = toCopy ? D3D12_BARRIER_SYNC_COPY : D3D12_BARRIER_SYNC_ALL;
        b.AccessBefore = toCopy ? D3D12_BARRIER_ACCESS_RENDER_TARGET : D3D12_BARRIER_ACCESS_COPY_SOURCE;
        b.AccessAfter = toCopy ? D3D12_BARRIER_ACCESS_COPY_SOURCE : D3D12_BARRIER_ACCESS_RENDER_TARGET;
        b.LayoutBefore = toCopy ? D3D12_BARRIER_LAYOUT_RENDER_TARGET : D3D12_BARRIER_LAYOUT_COPY_SOURCE;
        b.LayoutAfter = toCopy ? D3D12_BARRIER_LAYOUT_COPY_SOURCE : D3D12_BARRIER_LAYOUT_RENDER_TARGET;
        b.pResource = t.resource.Get();
        b.Subresources.IndexOrFirstMipLevel = 0xffffffff;
        D3D12_BARRIER_GROUP group{};
        group.Type = D3D12_BARRIER_TYPE_TEXTURE;
        group.NumBarriers = 1;
        group.pTextureBarriers = &b;
        list7->Barrier(1, &group);
        return;
    }
    D3D12_RESOURCE_BARRIER b{};
    b.Type = D3D12_RESOURCE_BARRIER_TYPE_TRANSITION;
    b.Transition.pResource = t.resource.Get();
    b.Transition.Subresource = D3D12_RESOURCE_BARRIER_ALL_SUBRESOURCES;
    b.Transition.StateBefore = toCopy ? D3D12_RESOURCE_STATE_RENDER_TARGET : D3D12_RESOURCE_STATE_COPY_SOURCE;
    b.Transition.StateAfter = toCopy ? D3D12_RESOURCE_STATE_COPY_SOURCE : D3D12_RESOURCE_STATE_RENDER_TARGET;
    list->ResourceBarrier(1, &b);
}

void transition_image(ID3D12GraphicsCommandList* list, ID3D12Resource* image, bool toCopy) {
    D3D12_RESOURCE_BARRIER b{};
    b.Type = D3D12_RESOURCE_BARRIER_TYPE_TRANSITION;
    b.Transition.pResource = image;
    b.Transition.Subresource = D3D12_RESOURCE_BARRIER_ALL_SUBRESOURCES;
    b.Transition.StateBefore = toCopy ? D3D12_RESOURCE_STATE_RENDER_TARGET : D3D12_RESOURCE_STATE_COPY_DEST;
    b.Transition.StateAfter = toCopy ? D3D12_RESOURCE_STATE_COPY_DEST : D3D12_RESOURCE_STATE_RENDER_TARGET;
    list->ResourceBarrier(1, &b);
}

d3d::CapturedTexture* captured(const Target& target) { return static_cast<d3d::CapturedTexture*>(target.native); }

} // namespace

const char* const* required_extensions(uint32_t& count) {
    static const char* const kExtensions[] = {XR_KHR_D3D12_ENABLE_EXTENSION_NAME};
    count = 1;
    return kExtensions;
}

const char* backend_name() { return "D3D12"; }

bool initialize(WGPUDevice device, WGPUAdapter adapter) {
    if (!d3d::is_d3d12_backend(adapter)) {
        mods::log::error("VR needs Dusklight's D3D12 graphics backend (Settings > Graphics > Backend)");
        return false;
    }
    g.device = device;
    g.d3dDevice = d3d::device_of(device);
    g.d3dQueue = d3d::queue_of(device);
    if (!g.d3dDevice || !g.d3dQueue) {
        mods::log::error("could not reach Dawn's D3D12 device");
        return false;
    }
    // Dusklight renders gamma-encoded colour into UNORM targets, so an *_SRGB swapchain (fed the
    // same bytes) is what makes the runtime display it at the right brightness.
    g.formats = {
        {DXGI_FORMAT_R8G8B8A8_UNORM_SRGB, WGPUTextureFormat_RGBA8Unorm},
        {DXGI_FORMAT_B8G8R8A8_UNORM_SRGB, WGPUTextureFormat_BGRA8Unorm},
        {DXGI_FORMAT_R8G8B8A8_UNORM, WGPUTextureFormat_RGBA8Unorm},
        {DXGI_FORMAT_B8G8R8A8_UNORM, WGPUTextureFormat_BGRA8Unorm},
    };
    return true;
}

void shutdown() {
    wait_idle();
    for (auto& l : g.copyList) {
        l.Reset();
    }
    for (auto& a : g.copyAlloc) {
        a.Reset();
    }
    g.fence.Reset();
    if (g.fenceEvent != nullptr) {
        CloseHandle(g.fenceEvent);
        g.fenceEvent = nullptr;
    }
    g.d3dQueue.Reset();
    g.d3dDevice.Reset();
    g.device = nullptr;
}

bool prepare_system(uintptr_t instance, uint64_t systemId, const void*& graphicsBinding) {
    auto xrInstance = reinterpret_cast<XrInstance>(instance);
    PFN_xrGetD3D12GraphicsRequirementsKHR getReqs = nullptr;
    xrGetInstanceProcAddr(
        xrInstance, "xrGetD3D12GraphicsRequirementsKHR", reinterpret_cast<PFN_xrVoidFunction*>(&getReqs));
    XrGraphicsRequirementsD3D12KHR reqs{XR_TYPE_GRAPHICS_REQUIREMENTS_D3D12_KHR};
    if (getReqs == nullptr || XR_FAILED(getReqs(xrInstance, systemId, &reqs))) {
        mods::log::error("OpenXR xrGetD3D12GraphicsRequirementsKHR failed");
        return false;
    }
    const LUID luid = g.d3dDevice->GetAdapterLuid();
    if (luid.HighPart != reqs.adapterLuid.HighPart || luid.LowPart != reqs.adapterLuid.LowPart) {
        mods::log::warn("Dusklight renders on a different GPU than the headset expects; performance may suffer");
    }
    if (!create_copy_infra()) {
        mods::log::error("could not create the D3D12 copy resources");
        return false;
    }
    g.binding = {XR_TYPE_GRAPHICS_BINDING_D3D12_KHR};
    g.binding.device = g.d3dDevice.Get();
    g.binding.queue = g.d3dQueue.Get();
    graphicsBinding = &g.binding;
    return true;
}

const std::vector<FormatChoice>& candidate_formats() { return g.formats; }

bool enumerate_swapchain_images(uint64_t swapchain, std::vector<SwapchainImage>& out) {
    auto handle = reinterpret_cast<XrSwapchain>(swapchain);
    uint32_t count = 0;
    xrEnumerateSwapchainImages(handle, 0, &count, nullptr);
    std::vector<XrSwapchainImageD3D12KHR> images(count, {XR_TYPE_SWAPCHAIN_IMAGE_D3D12_KHR});
    if (XR_FAILED(xrEnumerateSwapchainImages(
            handle, count, &count, reinterpret_cast<XrSwapchainImageBaseHeader*>(images.data()))))
    {
        return false;
    }
    out.clear();
    for (const auto& img : images) {
        out.push_back(reinterpret_cast<SwapchainImage>(img.texture));
    }
    return true;
}

bool create_target(uint32_t width, uint32_t height, WGPUTextureFormat format, const char* label, Target& out) {
    destroy_target(out);
    auto* captured = new d3d::CapturedTexture();
    if (!d3d::create_captured_texture(g.device, g.d3dDevice.Get(), width, height, format, label, *captured)) {
        delete captured;
        return false;
    }
    out.texture = captured->texture;
    out.view = captured->view;
    out.width = width;
    out.height = height;
    out.format = format;
    out.native = captured;
    return true;
}

void destroy_target(Target& target) {
    if (target.native != nullptr) {
        auto* c = captured(target);
        c->reset();
        delete c;
    }
    target = {};
}

bool copy_to_swapchains(const std::vector<CopyJob>& jobs) {
    if (jobs.empty() || !g.fence) {
        return false;
    }
    const uint32_t slot = g.copySlot;
    g.copySlot = (slot + 1) % kCopyRing;
    if (g.fence->GetCompletedValue() < g.copyFenceAt[slot]) {
        g.fence->SetEventOnCompletion(g.copyFenceAt[slot], g.fenceEvent);
        WaitForSingleObject(g.fenceEvent, 1000);
    }
    ID3D12CommandAllocator* alloc = g.copyAlloc[slot].Get();
    ID3D12GraphicsCommandList* list = g.copyList[slot].Get();
    alloc->Reset();
    list->Reset(alloc, nullptr);
    for (const auto& job : jobs) {
        auto* src = captured(*job.source);
        auto* dst = reinterpret_cast<ID3D12Resource*>(job.destination);
        transition_source(list, *src, true);
        transition_image(list, dst, true);
        list->CopyResource(dst, src->resource.Get());
        transition_image(list, dst, false);
        transition_source(list, *src, false);
    }
    list->Close();
    ID3D12CommandList* lists[] = {list};
    g.d3dQueue->ExecuteCommandLists(1, lists);
    const uint64_t v = ++g.fenceValue;
    g.d3dQueue->Signal(g.fence.Get(), v);
    g.copyFenceAt[slot] = v;
    return true;
}

void wait_idle() {
    if (!g.d3dQueue || !g.fence) {
        return;
    }
    const uint64_t v = ++g.fenceValue;
    g.d3dQueue->Signal(g.fence.Get(), v);
    if (g.fence->GetCompletedValue() < v) {
        g.fence->SetEventOnCompletion(v, g.fenceEvent);
        WaitForSingleObject(g.fenceEvent, 2000);
    }
}

bool read_center_pixel(const Target& target, uint8_t rgba[4]) {
    if (!target || !g.d3dDevice || !create_copy_infra()) {
        return false;
    }
    // Same path as a real frame: barrier the captured Dawn resource to COPY_SOURCE on Dawn's queue
    // right after Dawn's submit, copy, and barrier back. The destination is a readback buffer here.
    const auto& src = *captured(target);
    D3D12_RESOURCE_DESC desc = src.resource->GetDesc();
    D3D12_PLACED_SUBRESOURCE_FOOTPRINT fp{};
    UINT64 total = 0;
    g.d3dDevice->GetCopyableFootprints(&desc, 0, 1, 0, &fp, nullptr, nullptr, &total);
    D3D12_HEAP_PROPERTIES heap{D3D12_HEAP_TYPE_READBACK};
    D3D12_RESOURCE_DESC bd{};
    bd.Dimension = D3D12_RESOURCE_DIMENSION_BUFFER;
    bd.Width = total;
    bd.Height = 1;
    bd.DepthOrArraySize = 1;
    bd.MipLevels = 1;
    bd.SampleDesc.Count = 1;
    bd.Layout = D3D12_TEXTURE_LAYOUT_ROW_MAJOR;
    ComPtr<ID3D12Resource> buffer;
    if (FAILED(g.d3dDevice->CreateCommittedResource(
            &heap, D3D12_HEAP_FLAG_NONE, &bd, D3D12_RESOURCE_STATE_COPY_DEST, nullptr, IID_PPV_ARGS(&buffer))))
    {
        return false;
    }
    ID3D12CommandAllocator* alloc = g.copyAlloc[0].Get();
    ID3D12GraphicsCommandList* list = g.copyList[0].Get();
    alloc->Reset();
    list->Reset(alloc, nullptr);
    transition_source(list, src, true);
    D3D12_TEXTURE_COPY_LOCATION dst{buffer.Get(), D3D12_TEXTURE_COPY_TYPE_PLACED_FOOTPRINT};
    dst.PlacedFootprint = fp;
    D3D12_TEXTURE_COPY_LOCATION srcLoc{src.resource.Get(), D3D12_TEXTURE_COPY_TYPE_SUBRESOURCE_INDEX};
    srcLoc.SubresourceIndex = 0;
    list->CopyTextureRegion(&dst, 0, 0, 0, &srcLoc, nullptr);
    transition_source(list, src, false);
    list->Close();
    ID3D12CommandList* lists[] = {list};
    g.d3dQueue->ExecuteCommandLists(1, lists);
    wait_idle();
    uint8_t* data = nullptr;
    D3D12_RANGE range{0, static_cast<SIZE_T>(total)};
    bool ok = false;
    if (SUCCEEDED(buffer->Map(0, &range, reinterpret_cast<void**>(&data)))) {
        const uint8_t* px = data + fp.Offset + (target.height / 2) * fp.Footprint.RowPitch + (target.width / 2) * 4;
        for (int i = 0; i < 4; ++i) {
            rgba[i] = px[i];
        }
        D3D12_RANGE none{0, 0};
        buffer->Unmap(0, &none);
        ok = true;
    }
    return ok;
}

} // namespace vr::interop
