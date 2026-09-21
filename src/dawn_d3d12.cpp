#include "dawn_d3d12.hpp"

#include "mods/svc/log.hpp"

#include <atomic>
#include <mutex>

extern "C" {
extern const size_t vr_slot_CreateCommittedResource;
extern const size_t vr_slot_CreatePlacedResource;
extern const size_t vr_slot_CreateCommittedResource1;
extern const size_t vr_slot_CreateCommittedResource2;
extern const size_t vr_slot_CreatePlacedResource1;
extern const size_t vr_slot_CreateCommittedResource3;
extern const size_t vr_slot_CreatePlacedResource2;
}

namespace vr::d3d {
namespace {

HMODULE dawn_module() {
    static HMODULE mod = GetModuleHandleA("webgpu_dawn.dll");
    return mod;
}

// dawn::native::d3d12::GetD3D12Device / GetD3D12CommandQueue (D3D12Backend.h). Both return a
// ComPtr by value; declaring the same signature lets the compiler reproduce the MSVC ABI.
using GetDeviceFn = ComPtr<ID3D12Device> (*)(WGPUDevice);
using GetQueueFn = ComPtr<ID3D12CommandQueue> (*)(WGPUDevice);

constexpr const char* kGetDeviceSym =
    "?GetD3D12Device@d3d12@native@dawn@@YA?AV?$ComPtr@UID3D12Device@@@WRL@Microsoft@@PEAUWGPUDeviceImpl@@@Z";
constexpr const char* kGetQueueSym =
    "?GetD3D12CommandQueue@d3d12@native@dawn@@YA?AV?$ComPtr@UID3D12CommandQueue@@@WRL@Microsoft@@PEAUWGPUDeviceImpl@@@Z";

// --- Resource capture -------------------------------------------------------------------------

struct CaptureRequest {
    std::atomic<DWORD> threadId{0};
    uint32_t width = 0;
    uint32_t height = 0;
    ComPtr<ID3D12Resource> resource;
    BarrierModel barriers = BarrierModel::Legacy;
};
CaptureRequest g_capture;

void** g_vtbl = nullptr;
void* g_orig[7] = {};
size_t g_slots[7] = {};
int g_slotCount = 0;

void* orig_for(size_t slot) {
    for (int i = 0; i < g_slotCount; ++i) {
        if (g_slots[i] == slot) {
            return g_orig[i];
        }
    }
    return nullptr;
}

bool capture_matches(const void* descPtr) {
    if (g_capture.threadId.load(std::memory_order_acquire) != GetCurrentThreadId()) {
        return false;
    }
    // D3D12_RESOURCE_DESC and D3D12_RESOURCE_DESC1 share the leading fields we check.
    const auto* desc = static_cast<const D3D12_RESOURCE_DESC*>(descPtr);
    return desc != nullptr && desc->Dimension == D3D12_RESOURCE_DIMENSION_TEXTURE2D &&
           desc->Width == g_capture.width && desc->Height == g_capture.height;
}

void record_capture(HRESULT hr, REFIID riid, void** ppv, BarrierModel model) {
    if (FAILED(hr) || ppv == nullptr || *ppv == nullptr) {
        return;
    }
    ComPtr<ID3D12Resource> res;
    if (SUCCEEDED(static_cast<IUnknown*>(*ppv)->QueryInterface(IID_PPV_ARGS(&res)))) {
        g_capture.resource = res;
        g_capture.barriers = model;
    }
    (void)riid;
}

HRESULT STDMETHODCALLTYPE hk_CreateCommittedResource(ID3D12Device* self, const D3D12_HEAP_PROPERTIES* heap,
    D3D12_HEAP_FLAGS flags, const D3D12_RESOURCE_DESC* desc, D3D12_RESOURCE_STATES state,
    const D3D12_CLEAR_VALUE* clear, REFIID riid, void** ppv) {
    using Fn = HRESULT(STDMETHODCALLTYPE*)(ID3D12Device*, const D3D12_HEAP_PROPERTIES*, D3D12_HEAP_FLAGS,
        const D3D12_RESOURCE_DESC*, D3D12_RESOURCE_STATES, const D3D12_CLEAR_VALUE*, REFIID, void**);
    const HRESULT hr = reinterpret_cast<Fn>(orig_for(vr_slot_CreateCommittedResource))(
        self, heap, flags, desc, state, clear, riid, ppv);
    if (capture_matches(desc)) {
        record_capture(hr, riid, ppv, BarrierModel::Legacy);
    }
    return hr;
}

HRESULT STDMETHODCALLTYPE hk_CreatePlacedResource(ID3D12Device* self, ID3D12Heap* heap, UINT64 offset,
    const D3D12_RESOURCE_DESC* desc, D3D12_RESOURCE_STATES state, const D3D12_CLEAR_VALUE* clear, REFIID riid,
    void** ppv) {
    using Fn = HRESULT(STDMETHODCALLTYPE*)(ID3D12Device*, ID3D12Heap*, UINT64, const D3D12_RESOURCE_DESC*,
        D3D12_RESOURCE_STATES, const D3D12_CLEAR_VALUE*, REFIID, void**);
    const HRESULT hr = reinterpret_cast<Fn>(orig_for(vr_slot_CreatePlacedResource))(
        self, heap, offset, desc, state, clear, riid, ppv);
    if (capture_matches(desc)) {
        record_capture(hr, riid, ppv, BarrierModel::Legacy);
    }
    return hr;
}

HRESULT STDMETHODCALLTYPE hk_CreateCommittedResource1(ID3D12Device4* self, const D3D12_HEAP_PROPERTIES* heap,
    D3D12_HEAP_FLAGS flags, const D3D12_RESOURCE_DESC* desc, D3D12_RESOURCE_STATES state,
    const D3D12_CLEAR_VALUE* clear, ID3D12ProtectedResourceSession* prs, REFIID riid, void** ppv) {
    using Fn = HRESULT(STDMETHODCALLTYPE*)(ID3D12Device4*, const D3D12_HEAP_PROPERTIES*, D3D12_HEAP_FLAGS,
        const D3D12_RESOURCE_DESC*, D3D12_RESOURCE_STATES, const D3D12_CLEAR_VALUE*,
        ID3D12ProtectedResourceSession*, REFIID, void**);
    const HRESULT hr = reinterpret_cast<Fn>(orig_for(vr_slot_CreateCommittedResource1))(
        self, heap, flags, desc, state, clear, prs, riid, ppv);
    if (capture_matches(desc)) {
        record_capture(hr, riid, ppv, BarrierModel::Legacy);
    }
    return hr;
}

HRESULT STDMETHODCALLTYPE hk_CreateCommittedResource2(ID3D12Device8* self, const D3D12_HEAP_PROPERTIES* heap,
    D3D12_HEAP_FLAGS flags, const D3D12_RESOURCE_DESC1* desc, D3D12_RESOURCE_STATES state,
    const D3D12_CLEAR_VALUE* clear, ID3D12ProtectedResourceSession* prs, REFIID riid, void** ppv) {
    using Fn = HRESULT(STDMETHODCALLTYPE*)(ID3D12Device8*, const D3D12_HEAP_PROPERTIES*, D3D12_HEAP_FLAGS,
        const D3D12_RESOURCE_DESC1*, D3D12_RESOURCE_STATES, const D3D12_CLEAR_VALUE*,
        ID3D12ProtectedResourceSession*, REFIID, void**);
    const HRESULT hr = reinterpret_cast<Fn>(orig_for(vr_slot_CreateCommittedResource2))(
        self, heap, flags, desc, state, clear, prs, riid, ppv);
    if (capture_matches(desc)) {
        record_capture(hr, riid, ppv, BarrierModel::Legacy);
    }
    return hr;
}

HRESULT STDMETHODCALLTYPE hk_CreatePlacedResource1(ID3D12Device8* self, ID3D12Heap* heap, UINT64 offset,
    const D3D12_RESOURCE_DESC1* desc, D3D12_RESOURCE_STATES state, const D3D12_CLEAR_VALUE* clear, REFIID riid,
    void** ppv) {
    using Fn = HRESULT(STDMETHODCALLTYPE*)(ID3D12Device8*, ID3D12Heap*, UINT64, const D3D12_RESOURCE_DESC1*,
        D3D12_RESOURCE_STATES, const D3D12_CLEAR_VALUE*, REFIID, void**);
    const HRESULT hr = reinterpret_cast<Fn>(orig_for(vr_slot_CreatePlacedResource1))(
        self, heap, offset, desc, state, clear, riid, ppv);
    if (capture_matches(desc)) {
        record_capture(hr, riid, ppv, BarrierModel::Legacy);
    }
    return hr;
}

HRESULT STDMETHODCALLTYPE hk_CreateCommittedResource3(ID3D12Device10* self, const D3D12_HEAP_PROPERTIES* heap,
    D3D12_HEAP_FLAGS flags, const D3D12_RESOURCE_DESC1* desc, D3D12_BARRIER_LAYOUT layout,
    const D3D12_CLEAR_VALUE* clear, ID3D12ProtectedResourceSession* prs, UINT32 numCastable,
    const DXGI_FORMAT* castable, REFIID riid, void** ppv) {
    using Fn = HRESULT(STDMETHODCALLTYPE*)(ID3D12Device10*, const D3D12_HEAP_PROPERTIES*, D3D12_HEAP_FLAGS,
        const D3D12_RESOURCE_DESC1*, D3D12_BARRIER_LAYOUT, const D3D12_CLEAR_VALUE*,
        ID3D12ProtectedResourceSession*, UINT32, const DXGI_FORMAT*, REFIID, void**);
    const HRESULT hr = reinterpret_cast<Fn>(orig_for(vr_slot_CreateCommittedResource3))(
        self, heap, flags, desc, layout, clear, prs, numCastable, castable, riid, ppv);
    if (capture_matches(desc)) {
        record_capture(hr, riid, ppv, BarrierModel::Enhanced);
    }
    return hr;
}

HRESULT STDMETHODCALLTYPE hk_CreatePlacedResource2(ID3D12Device10* self, ID3D12Heap* heap, UINT64 offset,
    const D3D12_RESOURCE_DESC1* desc, D3D12_BARRIER_LAYOUT layout, const D3D12_CLEAR_VALUE* clear,
    UINT32 numCastable, const DXGI_FORMAT* castable, REFIID riid, void** ppv) {
    using Fn = HRESULT(STDMETHODCALLTYPE*)(ID3D12Device10*, ID3D12Heap*, UINT64, const D3D12_RESOURCE_DESC1*,
        D3D12_BARRIER_LAYOUT, const D3D12_CLEAR_VALUE*, UINT32, const DXGI_FORMAT*, REFIID, void**);
    const HRESULT hr = reinterpret_cast<Fn>(orig_for(vr_slot_CreatePlacedResource2))(
        self, heap, offset, desc, layout, clear, numCastable, castable, riid, ppv);
    if (capture_matches(desc)) {
        record_capture(hr, riid, ppv, BarrierModel::Enhanced);
    }
    return hr;
}

// The originals table is append-only and never cleared: another thread may still be inside one of
// our hooks (reading its original) after the vtable has been restored.
void patch_slot(size_t slot, void* hook) {
    void** entry = &g_vtbl[slot];
    if (orig_for(slot) == nullptr) {
        g_orig[g_slotCount] = *entry;
        g_slots[g_slotCount] = slot;
        ++g_slotCount;
    }
    DWORD old = 0;
    VirtualProtect(entry, sizeof(void*), PAGE_EXECUTE_READWRITE, &old);
    *entry = hook;
    VirtualProtect(entry, sizeof(void*), old, &old);
}

void unpatch_all() {
    for (int i = 0; i < g_slotCount; ++i) {
        void** entry = &g_vtbl[g_slots[i]];
        DWORD old = 0;
        VirtualProtect(entry, sizeof(void*), PAGE_EXECUTE_READWRITE, &old);
        *entry = g_orig[i];
        VirtualProtect(entry, sizeof(void*), old, &old);
    }
}

// Patch every resource-creation entry point the device's interface level exposes. All the
// ID3D12DeviceN interfaces share one object and one vtable, so patching the slots covers calls
// made through any of them.
void patch_device(ID3D12Device* device) {
    g_vtbl = *reinterpret_cast<void***>(device);
    patch_slot(vr_slot_CreateCommittedResource, reinterpret_cast<void*>(&hk_CreateCommittedResource));
    patch_slot(vr_slot_CreatePlacedResource, reinterpret_cast<void*>(&hk_CreatePlacedResource));
    ComPtr<ID3D12Device4> d4;
    if (SUCCEEDED(device->QueryInterface(IID_PPV_ARGS(&d4)))) {
        patch_slot(vr_slot_CreateCommittedResource1, reinterpret_cast<void*>(&hk_CreateCommittedResource1));
    }
    ComPtr<ID3D12Device8> d8;
    if (SUCCEEDED(device->QueryInterface(IID_PPV_ARGS(&d8)))) {
        patch_slot(vr_slot_CreateCommittedResource2, reinterpret_cast<void*>(&hk_CreateCommittedResource2));
        patch_slot(vr_slot_CreatePlacedResource1, reinterpret_cast<void*>(&hk_CreatePlacedResource1));
    }
    ComPtr<ID3D12Device10> d10;
    if (SUCCEEDED(device->QueryInterface(IID_PPV_ARGS(&d10)))) {
        patch_slot(vr_slot_CreateCommittedResource3, reinterpret_cast<void*>(&hk_CreateCommittedResource3));
        patch_slot(vr_slot_CreatePlacedResource2, reinterpret_cast<void*>(&hk_CreatePlacedResource2));
    }
}

std::mutex g_captureMutex;

} // namespace

bool is_d3d12_backend(WGPUAdapter adapter) {
    if (adapter == nullptr) {
        return false;
    }
    WGPUAdapterInfo info = WGPU_ADAPTER_INFO_INIT;
    if (wgpuAdapterGetInfo(adapter, &info) != WGPUStatus_Success) {
        return false;
    }
    const bool d3d12 = info.backendType == WGPUBackendType_D3D12;
    wgpuAdapterInfoFreeMembers(info);
    return d3d12;
}

ComPtr<ID3D12Device> device_of(WGPUDevice device) {
    auto fn = reinterpret_cast<GetDeviceFn>(GetProcAddress(dawn_module(), kGetDeviceSym));
    if (fn == nullptr || device == nullptr) {
        return nullptr;
    }
    return fn(device);
}

ComPtr<ID3D12CommandQueue> queue_of(WGPUDevice device) {
    auto fn = reinterpret_cast<GetQueueFn>(GetProcAddress(dawn_module(), kGetQueueSym));
    if (fn == nullptr || device == nullptr) {
        return nullptr;
    }
    return fn(device);
}

void CapturedTexture::reset() {
    if (view != nullptr) {
        wgpuTextureViewRelease(view);
        view = nullptr;
    }
    if (texture != nullptr) {
        wgpuTextureRelease(texture);
        texture = nullptr;
    }
    resource.Reset();
    width = height = 0;
}

bool create_captured_texture(WGPUDevice device, ID3D12Device* d3dDevice, uint32_t width, uint32_t height,
    WGPUTextureFormat format, const char* label, CapturedTexture& out) {
    out.reset();
    std::lock_guard lock{g_captureMutex};

    g_capture.width = width;
    g_capture.height = height;
    g_capture.resource.Reset();
    g_capture.threadId.store(GetCurrentThreadId(), std::memory_order_release);
    patch_device(d3dDevice);

    WGPUTextureDescriptor desc = WGPU_TEXTURE_DESCRIPTOR_INIT;
    desc.label = WGPUStringView{label, WGPU_STRLEN};
    desc.usage = WGPUTextureUsage_RenderAttachment | WGPUTextureUsage_TextureBinding | WGPUTextureUsage_CopySrc;
    desc.dimension = WGPUTextureDimension_2D;
    desc.size = {width, height, 1};
    desc.format = format;
    desc.mipLevelCount = 1;
    desc.sampleCount = 1;
    WGPUTexture tex = wgpuDeviceCreateTexture(device, &desc);

    unpatch_all();
    g_capture.threadId.store(0, std::memory_order_release);

    if (tex == nullptr) {
        mods::log::error("create_captured_texture: wgpuDeviceCreateTexture failed ({}x{})", width, height);
        return false;
    }
    if (!g_capture.resource) {
        mods::log::error("create_captured_texture: Dawn did not allocate a resource we could observe ({}x{})",
            width, height);
        wgpuTextureRelease(tex);
        return false;
    }
    out.texture = tex;
    out.view = wgpuTextureCreateView(tex, nullptr);
    out.resource = g_capture.resource;
    out.barriers = g_capture.barriers;
    out.width = width;
    out.height = height;
    out.format = format;
    g_capture.resource.Reset();
    return true;
}

} // namespace vr::d3d
