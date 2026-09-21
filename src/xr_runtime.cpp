#include "xr_runtime.hpp"

#include "vr_config.hpp"

#include "mods/svc/log.hpp"

#define XR_USE_GRAPHICS_API_D3D12
#define XR_USE_PLATFORM_WIN32
#include <openxr/openxr.h>
#include <openxr/openxr_platform.h>

#include <algorithm>
#include <array>
#include <chrono>
#include <condition_variable>
#include <cstdio>
#include <cstring>
#include <mutex>
#include <vector>

namespace vr::xr {
namespace {

using d3d::ComPtr;

struct Swapchain {
    XrSwapchain handle = XR_NULL_HANDLE;
    uint32_t width = 0;
    uint32_t height = 0;
    std::vector<ID3D12Resource*> images; // runtime-owned
};

// The quad (HUD / virtual screen) resources are recreated when the game's framebuffer size
// changes. Frames already in flight keep a raw pointer to the set they were rendered with, so
// retired sets are only destroyed once those frames have ended.
struct QuadResources {
    Swapchain swapchain;
    d3d::CapturedTexture target;
};

enum class FrameState : uint8_t { Free, Waited, Begun, Ended };

struct FrameRecord {
    uint64_t id = 0;
    FrameState state = FrameState::Free;
    XrTime displayTime = 0;
    bool shouldRender = false;
    bool viewsValid = false;
    std::array<XrView, 2> views{};
};

struct Armed {
    uint64_t id = 0;
    bool stereo = false;
    QuadLayer quad;
    QuadResources* quadRes = nullptr;
};

constexpr uint32_t kCopyRing = 3;
constexpr size_t kRecordRing = 8;

struct State {
    // Graphics
    WGPUDevice device = nullptr;
    ComPtr<ID3D12Device> d3dDevice;
    ComPtr<ID3D12CommandQueue> d3dQueue;
    WGPUTextureFormat targetFormat = WGPUTextureFormat_RGBA8Unorm;
    int64_t swapchainFormat = 0;

    // OpenXR
    XrInstance instance = XR_NULL_HANDLE;
    XrSystemId system = XR_NULL_SYSTEM_ID;
    XrSession session = XR_NULL_HANDLE;
    XrSpace appSpace = XR_NULL_HANDLE;
    XrSpace viewSpace = XR_NULL_HANDLE;
    XrSpace localSpace = XR_NULL_HANDLE;
    std::vector<XrSpace> retiredSpaces;
    XrSessionState sessionState = XR_SESSION_STATE_UNKNOWN;
    bool sessionRunning = false; // between xrBeginSession and xrEndSession
    bool recenterRequested = false;
    bool needRecenterOnStart = true;
    std::chrono::steady_clock::time_point nextSessionAttempt{};
    bool hmdWaitLogged = false;
    std::string runtimeName;
    std::string systemName;

    std::array<Swapchain, 2> eyeSwapchains;
    std::array<d3d::CapturedTexture, 2> eyeTargets;
    QuadResources* quad = nullptr;
    std::vector<std::pair<uint64_t, QuadResources*>> retiredQuads;

    // D3D12 copy infrastructure (runs on Dawn's queue so it is ordered after Dawn's frame work).
    std::array<ComPtr<ID3D12CommandAllocator>, kCopyRing> copyAlloc;
    std::array<ComPtr<ID3D12GraphicsCommandList>, kCopyRing> copyList;
    std::array<uint64_t, kCopyRing> copyFenceAt{};
    uint32_t copySlot = 0;
    ComPtr<ID3D12Fence> fence;
    uint64_t fenceValue = 0;
    HANDLE fenceEvent = nullptr;

    // Frame bookkeeping (guarded by frameMutex).
    std::mutex frameMutex;
    std::condition_variable frameCv;
    std::array<FrameRecord, kRecordRing> records{};
    uint64_t nextId = 1;
    uint64_t lastWaited = 0;
    Armed armed;

    // Stats
    uint64_t framesSubmitted = 0;
    uint64_t framesDiscarded = 0;
};

State g;

bool xr_ok(XrResult r, const char* what) {
    if (XR_SUCCEEDED(r)) {
        return true;
    }
    char buf[XR_MAX_RESULT_STRING_SIZE] = {};
    if (g.instance != XR_NULL_HANDLE) {
        xrResultToString(g.instance, r, buf);
    } else {
        std::snprintf(buf, sizeof(buf), "%d", static_cast<int>(r));
    }
    mods::log::error("OpenXR {} failed: {}", what, buf);
    return false;
}

XrPosef to_xr(const Pose& p) {
    XrPosef r;
    r.orientation = {p.orientation.x, p.orientation.y, p.orientation.z, p.orientation.w};
    r.position = {p.position.x, p.position.y, p.position.z};
    return r;
}

Pose from_xr(const XrPosef& p) {
    return {{p.orientation.x, p.orientation.y, p.orientation.z, p.orientation.w},
        {p.position.x, p.position.y, p.position.z}};
}

FrameRecord* find_record(uint64_t id) {
    if (id == 0) {
        return nullptr;
    }
    FrameRecord& r = g.records[id % kRecordRing];
    return r.id == id ? &r : nullptr;
}

// --- GPU helpers ---------------------------------------------------------------------------------

void gpu_wait_idle() {
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

bool create_copy_infra() {
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

void destroy_copy_infra() {
    gpu_wait_idle();
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
}

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
    // Dawn leaves the target in RENDER_TARGET after our composition pass (and still believes it is
    // there), so we round-trip it through COPY_SOURCE.
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

struct CopyJob {
    Swapchain* swapchain;
    const d3d::CapturedTexture* source;
    uint32_t imageIndex = 0;
};

// Acquire each destination image, copy our targets into them on Dawn's queue, release. Never
// blocks the CPU on the GPU except when reusing a ring slot that is still in flight.
bool copy_into_swapchains(std::vector<CopyJob>& jobs) {
    for (auto& job : jobs) {
        XrSwapchainImageAcquireInfo acq{XR_TYPE_SWAPCHAIN_IMAGE_ACQUIRE_INFO};
        if (!xr_ok(xrAcquireSwapchainImage(job.swapchain->handle, &acq, &job.imageIndex), "xrAcquireSwapchainImage")) {
            return false;
        }
        XrSwapchainImageWaitInfo wait{XR_TYPE_SWAPCHAIN_IMAGE_WAIT_INFO};
        wait.timeout = XR_INFINITE_DURATION;
        if (!xr_ok(xrWaitSwapchainImage(job.swapchain->handle, &wait), "xrWaitSwapchainImage")) {
            return false;
        }
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
    for (auto& job : jobs) {
        ID3D12Resource* dst = job.swapchain->images[job.imageIndex];
        transition_source(list, *job.source, true);
        transition_image(list, dst, true);
        list->CopyResource(dst, job.source->resource.Get());
        transition_image(list, dst, false);
        transition_source(list, *job.source, false);
    }
    list->Close();
    ID3D12CommandList* lists[] = {list};
    g.d3dQueue->ExecuteCommandLists(1, lists);
    const uint64_t v = ++g.fenceValue;
    g.d3dQueue->Signal(g.fence.Get(), v);
    g.copyFenceAt[slot] = v;

    bool ok = true;
    for (auto& job : jobs) {
        XrSwapchainImageReleaseInfo rel{XR_TYPE_SWAPCHAIN_IMAGE_RELEASE_INFO};
        ok = xr_ok(xrReleaseSwapchainImage(job.swapchain->handle, &rel), "xrReleaseSwapchainImage") && ok;
    }
    return ok;
}

// --- Swapchains ----------------------------------------------------------------------------------

bool pick_swapchain_format() {
    uint32_t count = 0;
    if (!xr_ok(xrEnumerateSwapchainFormats(g.session, 0, &count, nullptr), "xrEnumerateSwapchainFormats")) {
        return false;
    }
    std::vector<int64_t> formats(count);
    xrEnumerateSwapchainFormats(g.session, count, &count, formats.data());
    // Dusklight renders gamma-encoded colour into UNORM targets, so an *_SRGB swapchain (fed the
    // same bytes) is what makes the runtime display it at the right brightness.
    struct Candidate {
        int64_t dxgi;
        WGPUTextureFormat wgpu;
    };
    constexpr Candidate kCandidates[] = {
        {DXGI_FORMAT_R8G8B8A8_UNORM_SRGB, WGPUTextureFormat_RGBA8Unorm},
        {DXGI_FORMAT_B8G8R8A8_UNORM_SRGB, WGPUTextureFormat_BGRA8Unorm},
        {DXGI_FORMAT_R8G8B8A8_UNORM, WGPUTextureFormat_RGBA8Unorm},
        {DXGI_FORMAT_B8G8R8A8_UNORM, WGPUTextureFormat_BGRA8Unorm},
    };
    // Runtime order is its preference; take the first one we can feed.
    for (int64_t f : formats) {
        for (const auto& c : kCandidates) {
            if (c.dxgi == f) {
                g.swapchainFormat = f;
                g.targetFormat = c.wgpu;
                mods::log::info("OpenXR swapchain format: DXGI {}", f);
                return true;
            }
        }
    }
    mods::log::error("OpenXR runtime offers no 8-bit RGBA swapchain format");
    return false;
}

bool create_swapchain(Swapchain& sc, uint32_t width, uint32_t height) {
    XrSwapchainCreateInfo ci{XR_TYPE_SWAPCHAIN_CREATE_INFO};
    ci.usageFlags = XR_SWAPCHAIN_USAGE_COLOR_ATTACHMENT_BIT | XR_SWAPCHAIN_USAGE_TRANSFER_DST_BIT;
    ci.format = g.swapchainFormat;
    ci.sampleCount = 1;
    ci.width = width;
    ci.height = height;
    ci.faceCount = 1;
    ci.arraySize = 1;
    ci.mipCount = 1;
    if (!xr_ok(xrCreateSwapchain(g.session, &ci, &sc.handle), "xrCreateSwapchain")) {
        return false;
    }
    uint32_t count = 0;
    xrEnumerateSwapchainImages(sc.handle, 0, &count, nullptr);
    std::vector<XrSwapchainImageD3D12KHR> images(count, {XR_TYPE_SWAPCHAIN_IMAGE_D3D12_KHR});
    if (!xr_ok(xrEnumerateSwapchainImages(
                   sc.handle, count, &count, reinterpret_cast<XrSwapchainImageBaseHeader*>(images.data())),
            "xrEnumerateSwapchainImages"))
    {
        return false;
    }
    sc.images.clear();
    for (auto& img : images) {
        sc.images.push_back(img.texture);
    }
    sc.width = width;
    sc.height = height;
    return true;
}

void destroy_swapchain(Swapchain& sc) {
    if (sc.handle != XR_NULL_HANDLE) {
        xrDestroySwapchain(sc.handle);
    }
    sc = {};
}

void destroy_quad(QuadResources* q) {
    if (q == nullptr) {
        return;
    }
    destroy_swapchain(q->swapchain);
    q->target.reset();
    delete q;
}

// --- Spaces / recentre -------------------------------------------------------------------------

XrSpace create_space(XrReferenceSpaceType type, const XrPosef& pose) {
    XrReferenceSpaceCreateInfo ci{XR_TYPE_REFERENCE_SPACE_CREATE_INFO};
    ci.referenceSpaceType = type;
    ci.poseInReferenceSpace = pose;
    XrSpace space = XR_NULL_HANDLE;
    xr_ok(xrCreateReferenceSpace(g.session, &ci, &space), "xrCreateReferenceSpace");
    return space;
}

// Re-anchors the app space at the head's current position, facing its current yaw.
void do_recenter(XrTime time) {
    XrSpaceLocation loc{XR_TYPE_SPACE_LOCATION};
    if (!XR_SUCCEEDED(xrLocateSpace(g.viewSpace, g.localSpace, time, &loc)) ||
        !(loc.locationFlags & XR_SPACE_LOCATION_ORIENTATION_VALID_BIT) ||
        !(loc.locationFlags & XR_SPACE_LOCATION_POSITION_VALID_BIT))
    {
        return;
    }
    const Pose head = from_xr(loc.pose);
    Pose anchor;
    anchor.orientation = quat_from_yaw(yaw_of(head.orientation));
    anchor.position = head.position;
    XrSpace space = create_space(XR_REFERENCE_SPACE_TYPE_LOCAL, to_xr(anchor));
    if (space == XR_NULL_HANDLE) {
        return;
    }
    std::lock_guard lock{g.frameMutex};
    if (g.appSpace != XR_NULL_HANDLE) {
        g.retiredSpaces.push_back(g.appSpace);
    }
    g.appSpace = space;
    mods::log::info("Recentred view");
}

// --- Session -------------------------------------------------------------------------------------

bool frames_idle_locked() {
    for (const auto& r : g.records) {
        if (r.state == FrameState::Waited || r.state == FrameState::Begun) {
            return false;
        }
    }
    return true;
}

// Ends any frame that is still open so the session can stop. Requires frameMutex.
void flush_frames_locked() {
    for (auto& r : g.records) {
        if (r.state == FrameState::Begun) {
            XrFrameEndInfo end{XR_TYPE_FRAME_END_INFO};
            end.displayTime = r.displayTime;
            end.environmentBlendMode = XR_ENVIRONMENT_BLEND_MODE_OPAQUE;
            xrEndFrame(g.session, &end);
        }
        if (r.state != FrameState::Free) {
            r.state = FrameState::Ended;
        }
    }
    g.armed = {};
    g.frameCv.notify_all();
}

void teardown_session() {
    {
        std::lock_guard lock{g.frameMutex};
        if (g.session != XR_NULL_HANDLE) {
            flush_frames_locked();
        }
        if (g.sessionRunning) {
            xrEndSession(g.session);
            g.sessionRunning = false;
        }
    }
    gpu_wait_idle();
    for (auto& sc : g.eyeSwapchains) {
        destroy_swapchain(sc);
    }
    for (auto& t : g.eyeTargets) {
        t.reset();
    }
    destroy_quad(g.quad);
    g.quad = nullptr;
    for (auto& [id, q] : g.retiredQuads) {
        destroy_quad(q);
    }
    g.retiredQuads.clear();
    for (XrSpace s : g.retiredSpaces) {
        xrDestroySpace(s);
    }
    g.retiredSpaces.clear();
    for (XrSpace* s : {&g.appSpace, &g.viewSpace, &g.localSpace}) {
        if (*s != XR_NULL_HANDLE) {
            xrDestroySpace(*s);
            *s = XR_NULL_HANDLE;
        }
    }
    destroy_copy_infra();
    if (g.session != XR_NULL_HANDLE) {
        xrDestroySession(g.session);
        g.session = XR_NULL_HANDLE;
    }
    g.system = XR_NULL_SYSTEM_ID;
    g.sessionState = XR_SESSION_STATE_UNKNOWN;
    g.needRecenterOnStart = true;
}

bool create_session() {
    XrSystemGetInfo sysInfo{XR_TYPE_SYSTEM_GET_INFO};
    sysInfo.formFactor = XR_FORM_FACTOR_HEAD_MOUNTED_DISPLAY;
    const XrResult sysRes = xrGetSystem(g.instance, &sysInfo, &g.system);
    if (XR_FAILED(sysRes)) {
        if (!g.hmdWaitLogged) {
            g.hmdWaitLogged = true;
            mods::log::info("OpenXR: no headset available yet; will keep checking");
        }
        return false;
    }
    g.hmdWaitLogged = false;
    XrSystemProperties props{XR_TYPE_SYSTEM_PROPERTIES};
    if (XR_SUCCEEDED(xrGetSystemProperties(g.instance, g.system, &props))) {
        g.systemName = props.systemName;
    }

    PFN_xrGetD3D12GraphicsRequirementsKHR getReqs = nullptr;
    xrGetInstanceProcAddr(
        g.instance, "xrGetD3D12GraphicsRequirementsKHR", reinterpret_cast<PFN_xrVoidFunction*>(&getReqs));
    XrGraphicsRequirementsD3D12KHR reqs{XR_TYPE_GRAPHICS_REQUIREMENTS_D3D12_KHR};
    if (getReqs == nullptr || !xr_ok(getReqs(g.instance, g.system, &reqs), "xrGetD3D12GraphicsRequirementsKHR")) {
        return false;
    }
    const LUID luid = g.d3dDevice->GetAdapterLuid();
    if (luid.HighPart != reqs.adapterLuid.HighPart || luid.LowPart != reqs.adapterLuid.LowPart) {
        mods::log::warn("Dusklight renders on a different GPU than the headset expects; performance may suffer");
    }

    XrGraphicsBindingD3D12KHR binding{XR_TYPE_GRAPHICS_BINDING_D3D12_KHR};
    binding.device = g.d3dDevice.Get();
    binding.queue = g.d3dQueue.Get();
    XrSessionCreateInfo ci{XR_TYPE_SESSION_CREATE_INFO};
    ci.next = &binding;
    ci.systemId = g.system;
    if (!xr_ok(xrCreateSession(g.instance, &ci, &g.session), "xrCreateSession")) {
        return false;
    }

    XrPosef identity{};
    identity.orientation.w = 1.0f;
    g.localSpace = create_space(XR_REFERENCE_SPACE_TYPE_LOCAL, identity);
    g.appSpace = create_space(XR_REFERENCE_SPACE_TYPE_LOCAL, identity);
    g.viewSpace = create_space(XR_REFERENCE_SPACE_TYPE_VIEW, identity);
    if (g.localSpace == XR_NULL_HANDLE || g.appSpace == XR_NULL_HANDLE || g.viewSpace == XR_NULL_HANDLE) {
        teardown_session();
        return false;
    }

    uint32_t viewCount = 0;
    xrEnumerateViewConfigurationViews(
        g.instance, g.system, XR_VIEW_CONFIGURATION_TYPE_PRIMARY_STEREO, 0, &viewCount, nullptr);
    std::vector<XrViewConfigurationView> views(viewCount, {XR_TYPE_VIEW_CONFIGURATION_VIEW});
    xrEnumerateViewConfigurationViews(
        g.instance, g.system, XR_VIEW_CONFIGURATION_TYPE_PRIMARY_STEREO, viewCount, &viewCount, views.data());
    if (viewCount < 2 || !pick_swapchain_format() || !create_copy_infra()) {
        teardown_session();
        return false;
    }

    const float scale = config().renderScale;
    for (int eye = 0; eye < 2; ++eye) {
        const auto& v = views[eye];
        const uint32_t w = std::clamp(static_cast<uint32_t>(v.recommendedImageRectWidth * scale), 64u,
            v.maxImageRectWidth);
        const uint32_t h = std::clamp(static_cast<uint32_t>(v.recommendedImageRectHeight * scale), 64u,
            v.maxImageRectHeight);
        if (!create_swapchain(g.eyeSwapchains[eye], w, h) ||
            !d3d::create_captured_texture(
                g.device, g.d3dDevice.Get(), w, h, g.targetFormat, eye == 0 ? "VR eye L" : "VR eye R",
                g.eyeTargets[eye]))
        {
            teardown_session();
            return false;
        }
    }
    mods::log::info("OpenXR session created on {} ({}x{} per eye, render scale {:.2f})", g.systemName,
        g.eyeSwapchains[0].width, g.eyeSwapchains[0].height, scale);
    return true;
}

void handle_session_state(XrSessionState state) {
    g.sessionState = state;
    switch (state) {
    case XR_SESSION_STATE_READY: {
        XrSessionBeginInfo bi{XR_TYPE_SESSION_BEGIN_INFO};
        bi.primaryViewConfigurationType = XR_VIEW_CONFIGURATION_TYPE_PRIMARY_STEREO;
        if (xr_ok(xrBeginSession(g.session, &bi), "xrBeginSession")) {
            std::lock_guard lock{g.frameMutex};
            g.sessionRunning = true;
            mods::log::info("OpenXR session started");
        }
        break;
    }
    case XR_SESSION_STATE_STOPPING: {
        std::lock_guard lock{g.frameMutex};
        flush_frames_locked();
        xrEndSession(g.session);
        g.sessionRunning = false;
        mods::log::info("OpenXR session stopped");
        break;
    }
    case XR_SESSION_STATE_EXITING:
    case XR_SESSION_STATE_LOSS_PENDING:
        mods::log::info("OpenXR session ending (state {})", static_cast<int>(state));
        teardown_session();
        g.nextSessionAttempt = std::chrono::steady_clock::now() + std::chrono::seconds(3);
        break;
    default:
        break;
    }
}

void retire_old_resources() {
    uint64_t oldestOpen = UINT64_MAX;
    {
        std::lock_guard lock{g.frameMutex};
        for (const auto& r : g.records) {
            if (r.state == FrameState::Waited || r.state == FrameState::Begun) {
                oldestOpen = std::min(oldestOpen, r.id);
            }
        }
        if (oldestOpen == UINT64_MAX) {
            oldestOpen = g.nextId;
        }
    }
    std::erase_if(g.retiredQuads, [&](const auto& entry) {
        if (entry.first < oldestOpen) {
            gpu_wait_idle();
            destroy_quad(entry.second);
            return true;
        }
        return false;
    });
    if (!g.retiredSpaces.empty() && oldestOpen == g.nextId) {
        for (XrSpace s : g.retiredSpaces) {
            xrDestroySpace(s);
        }
        g.retiredSpaces.clear();
    }
}

} // namespace

// --- Public API ------------------------------------------------------------------------------------

bool initialize(WGPUDevice device, WGPUAdapter adapter) {
    if (g.instance != XR_NULL_HANDLE) {
        return true;
    }
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

    uint32_t extCount = 0;
    if (XR_FAILED(xrEnumerateInstanceExtensionProperties(nullptr, 0, &extCount, nullptr))) {
        mods::log::warn("No OpenXR runtime installed; VR unavailable");
        return false;
    }
    std::vector<XrExtensionProperties> exts(extCount, {XR_TYPE_EXTENSION_PROPERTIES});
    xrEnumerateInstanceExtensionProperties(nullptr, extCount, &extCount, exts.data());
    const bool hasD3D12 = std::ranges::any_of(exts, [](const XrExtensionProperties& e) {
        return std::strcmp(e.extensionName, XR_KHR_D3D12_ENABLE_EXTENSION_NAME) == 0;
    });
    if (!hasD3D12) {
        mods::log::error("The active OpenXR runtime does not support D3D12");
        return false;
    }
    const char* enabled[] = {XR_KHR_D3D12_ENABLE_EXTENSION_NAME};
    XrInstanceCreateInfo ci{XR_TYPE_INSTANCE_CREATE_INFO};
    std::strncpy(ci.applicationInfo.applicationName, "Dusklight VR", XR_MAX_APPLICATION_NAME_SIZE - 1);
    std::strncpy(ci.applicationInfo.engineName, "Dusklight", XR_MAX_ENGINE_NAME_SIZE - 1);
    ci.applicationInfo.apiVersion = XR_API_VERSION_1_0;
    ci.enabledExtensionCount = 1;
    ci.enabledExtensionNames = enabled;
    if (!xr_ok(xrCreateInstance(&ci, &g.instance), "xrCreateInstance")) {
        return false;
    }
    XrInstanceProperties props{XR_TYPE_INSTANCE_PROPERTIES};
    if (XR_SUCCEEDED(xrGetInstanceProperties(g.instance, &props))) {
        g.runtimeName = props.runtimeName;
        mods::log::info("OpenXR runtime: {}", g.runtimeName);
    }
    return true;
}

void shutdown() {
    if (g.instance == XR_NULL_HANDLE) {
        return;
    }
    teardown_session();
    xrDestroyInstance(g.instance);
    g.instance = XR_NULL_HANDLE;
    g.d3dQueue.Reset();
    g.d3dDevice.Reset();
}

void poll() {
    if (g.instance == XR_NULL_HANDLE) {
        return;
    }
    if (g.session == XR_NULL_HANDLE) {
        const auto now = std::chrono::steady_clock::now();
        if (now < g.nextSessionAttempt) {
            return;
        }
        g.nextSessionAttempt = now + std::chrono::seconds(2);
        if (!create_session()) {
            return;
        }
    }

    XrEventDataBuffer ev{XR_TYPE_EVENT_DATA_BUFFER};
    while (xrPollEvent(g.instance, &ev) == XR_SUCCESS) {
        if (ev.type == XR_TYPE_EVENT_DATA_SESSION_STATE_CHANGED) {
            handle_session_state(reinterpret_cast<const XrEventDataSessionStateChanged*>(&ev)->state);
            if (g.session == XR_NULL_HANDLE) {
                return;
            }
        } else if (ev.type == XR_TYPE_EVENT_DATA_REFERENCE_SPACE_CHANGE_PENDING) {
            g.recenterRequested = true;
        } else if (ev.type == XR_TYPE_EVENT_DATA_INSTANCE_LOSS_PENDING) {
            teardown_session();
            return;
        }
        ev = {XR_TYPE_EVENT_DATA_BUFFER};
    }

    retire_old_resources();
}

bool instance_ready() { return g.instance != XR_NULL_HANDLE; }

bool session_running() { return g.sessionRunning; }

std::string status() {
    if (g.instance == XR_NULL_HANDLE) {
        return "OpenXR unavailable";
    }
    if (g.session == XR_NULL_HANDLE) {
        return g.runtimeName + ": waiting for headset";
    }
    static const char* kStates[] = {"unknown", "idle", "ready", "synchronized", "visible", "focused", "stopping",
        "loss pending", "exiting"};
    const int s = static_cast<int>(g.sessionState);
    return g.runtimeName + " / " + g.systemName + ": " + (s >= 0 && s <= 8 ? kStates[s] : "?") + ", " +
           std::to_string(g.eyeSwapchains[0].width) + "x" + std::to_string(g.eyeSwapchains[0].height) +
           " per eye, " + std::to_string(g.framesSubmitted) + " frames submitted, " +
           std::to_string(g.framesDiscarded) + " dropped";
}

void request_recenter() { g.recenterRequested = true; }

uint64_t wait_frame() {
    if (!g.sessionRunning) {
        return 0;
    }
    {
        // xrWaitFrame blocks until the previous frame has been begun. The render worker begins it;
        // if that never happened (the frame was dropped before encoding) end it here instead of
        // deadlocking.
        std::unique_lock lock{g.frameMutex};
        FrameRecord* prev = find_record(g.lastWaited);
        if (prev != nullptr && prev->state == FrameState::Waited) {
            const bool begun = g.frameCv.wait_for(lock, std::chrono::milliseconds(250),
                [&] { return prev->state != FrameState::Waited || !g.sessionRunning; });
            if (!begun && prev->state == FrameState::Waited && g.sessionRunning) {
                XrFrameBeginInfo bi{XR_TYPE_FRAME_BEGIN_INFO};
                if (XR_SUCCEEDED(xrBeginFrame(g.session, &bi))) {
                    XrFrameEndInfo end{XR_TYPE_FRAME_END_INFO};
                    end.displayTime = prev->displayTime;
                    end.environmentBlendMode = XR_ENVIRONMENT_BLEND_MODE_OPAQUE;
                    xrEndFrame(g.session, &end);
                }
                prev->state = FrameState::Ended;
                ++g.framesDiscarded;
            }
        }
        if (!g.sessionRunning) {
            return 0;
        }
    }

    XrFrameWaitInfo wi{XR_TYPE_FRAME_WAIT_INFO};
    XrFrameState fs{XR_TYPE_FRAME_STATE};
    if (!xr_ok(xrWaitFrame(g.session, &wi, &fs), "xrWaitFrame")) {
        return 0;
    }

    if (g.recenterRequested || g.needRecenterOnStart) {
        g.recenterRequested = false;
        g.needRecenterOnStart = false;
        do_recenter(fs.predictedDisplayTime);
    }

    std::lock_guard lock{g.frameMutex};
    const uint64_t id = g.nextId++;
    FrameRecord& r = g.records[id % kRecordRing];
    if (r.state == FrameState::Begun) {
        // Ring wrapped over a frame that never ended (should not happen); close it first.
        XrFrameEndInfo end{XR_TYPE_FRAME_END_INFO};
        end.displayTime = r.displayTime;
        end.environmentBlendMode = XR_ENVIRONMENT_BLEND_MODE_OPAQUE;
        xrEndFrame(g.session, &end);
    }
    r = {};
    r.id = id;
    r.state = FrameState::Waited;
    r.displayTime = fs.predictedDisplayTime;
    r.shouldRender = fs.shouldRender != XR_FALSE;
    g.lastWaited = id;
    return id;
}

bool locate_views(uint64_t id, FrameInfo& out) {
    out = {};
    XrTime time = 0;
    {
        std::lock_guard lock{g.frameMutex};
        FrameRecord* r = find_record(id);
        if (r == nullptr) {
            return false;
        }
        out.id = id;
        out.shouldRender = r->shouldRender;
        time = r->displayTime;
    }
    if (!out.shouldRender) {
        return false;
    }
    XrViewLocateInfo li{XR_TYPE_VIEW_LOCATE_INFO};
    li.viewConfigurationType = XR_VIEW_CONFIGURATION_TYPE_PRIMARY_STEREO;
    li.displayTime = time;
    li.space = g.appSpace;
    XrViewState vs{XR_TYPE_VIEW_STATE};
    std::array<XrView, 2> views{XrView{XR_TYPE_VIEW}, XrView{XR_TYPE_VIEW}};
    uint32_t count = 0;
    if (!XR_SUCCEEDED(xrLocateViews(g.session, &li, &vs, 2, &count, views.data())) || count < 2 ||
        !(vs.viewStateFlags & XR_VIEW_STATE_ORIENTATION_VALID_BIT))
    {
        return false;
    }
    for (int eye = 0; eye < 2; ++eye) {
        out.views[eye].pose = from_xr(views[eye].pose);
        const XrFovf& f = views[eye].fov;
        out.views[eye].fov = {std::tan(f.angleLeft), std::tan(f.angleRight), std::tan(f.angleUp), std::tan(f.angleDown)};
    }
    out.viewsValid = true;
    std::lock_guard lock{g.frameMutex};
    if (FrameRecord* r = find_record(id)) {
        r->views = views;
        r->viewsValid = true;
    }
    return true;
}

void abandon_frame(uint64_t id) {
    // Leave the record in Waited: the next wait_frame() ends it after its short timeout. Ending it
    // here could race a worker that is still ending the previous frame.
    (void)id;
}

void begin_frame(uint64_t id) {
    std::lock_guard lock{g.frameMutex};
    FrameRecord* r = find_record(id);
    if (r == nullptr || r->state != FrameState::Waited || !g.sessionRunning) {
        return;
    }
    // A previous frame that was begun but never armed/submitted must end first, or beginning this
    // one would discard it anyway.
    for (auto& other : g.records) {
        if (other.state == FrameState::Begun && other.id < id) {
            XrFrameEndInfo end{XR_TYPE_FRAME_END_INFO};
            end.displayTime = other.displayTime;
            end.environmentBlendMode = XR_ENVIRONMENT_BLEND_MODE_OPAQUE;
            xrEndFrame(g.session, &end);
            other.state = FrameState::Ended;
            ++g.framesDiscarded;
        }
    }
    XrFrameBeginInfo bi{XR_TYPE_FRAME_BEGIN_INFO};
    const XrResult res = xrBeginFrame(g.session, &bi);
    r->state = XR_SUCCEEDED(res) ? FrameState::Begun : FrameState::Ended;
    g.frameCv.notify_all();
}

void arm_submit(uint64_t id, bool stereo, const QuadLayer& quad, void* quadToken) {
    std::lock_guard lock{g.frameMutex};
    g.armed = {id, stereo, quad, quad.enabled ? static_cast<QuadResources*>(quadToken) : nullptr};
}

void on_queue_submitted() {
    std::lock_guard lock{g.frameMutex};
    if (g.armed.id == 0) {
        return;
    }
    const Armed armed = g.armed;
    g.armed = {};
    FrameRecord* r = find_record(armed.id);
    if (r == nullptr || r->state != FrameState::Begun || !g.sessionRunning) {
        return;
    }

    std::vector<CopyJob> jobs;
    const bool stereo = armed.stereo && r->shouldRender && r->viewsValid && g.eyeTargets[0] && g.eyeTargets[1];
    if (stereo) {
        jobs.push_back({&g.eyeSwapchains[0], &g.eyeTargets[0]});
        jobs.push_back({&g.eyeSwapchains[1], &g.eyeTargets[1]});
    }
    QuadResources* quad = armed.quadRes;
    const bool withQuad = r->shouldRender && armed.quad.enabled && quad != nullptr && quad->target;
    if (withQuad) {
        jobs.push_back({&quad->swapchain, &quad->target});
    }
    const bool copied = !jobs.empty() && copy_into_swapchains(jobs);

    std::array<XrCompositionLayerProjectionView, 2> projViews{};
    XrCompositionLayerProjection proj{XR_TYPE_COMPOSITION_LAYER_PROJECTION};
    XrCompositionLayerQuad quadLayer{XR_TYPE_COMPOSITION_LAYER_QUAD};
    std::vector<const XrCompositionLayerBaseHeader*> layers;
    if (copied && stereo) {
        for (int eye = 0; eye < 2; ++eye) {
            projViews[eye] = {XR_TYPE_COMPOSITION_LAYER_PROJECTION_VIEW};
            projViews[eye].pose = r->views[eye].pose;
            projViews[eye].fov = r->views[eye].fov;
            projViews[eye].subImage.swapchain = g.eyeSwapchains[eye].handle;
            projViews[eye].subImage.imageRect = {{0, 0},
                {static_cast<int32_t>(g.eyeSwapchains[eye].width), static_cast<int32_t>(g.eyeSwapchains[eye].height)}};
        }
        proj.space = g.appSpace;
        proj.viewCount = 2;
        proj.views = projViews.data();
        layers.push_back(reinterpret_cast<const XrCompositionLayerBaseHeader*>(&proj));
    }
    if (copied && withQuad) {
        quadLayer.layerFlags = armed.quad.premultipliedAlpha ? XR_COMPOSITION_LAYER_BLEND_TEXTURE_SOURCE_ALPHA_BIT : 0;
        quadLayer.space = armed.quad.space == QuadSpace::View ? g.viewSpace : g.appSpace;
        quadLayer.eyeVisibility = XR_EYE_VISIBILITY_BOTH;
        quadLayer.subImage.swapchain = quad->swapchain.handle;
        quadLayer.subImage.imageRect = {
            {0, 0}, {static_cast<int32_t>(quad->swapchain.width), static_cast<int32_t>(quad->swapchain.height)}};
        quadLayer.pose = to_xr(armed.quad.pose);
        quadLayer.size = {armed.quad.width, armed.quad.height};
        layers.push_back(reinterpret_cast<const XrCompositionLayerBaseHeader*>(&quadLayer));
    }

    XrFrameEndInfo end{XR_TYPE_FRAME_END_INFO};
    end.displayTime = r->displayTime;
    end.environmentBlendMode = XR_ENVIRONMENT_BLEND_MODE_OPAQUE;
    end.layerCount = static_cast<uint32_t>(layers.size());
    end.layers = layers.data();
    xr_ok(xrEndFrame(g.session, &end), "xrEndFrame");
    r->state = FrameState::Ended;
    ++g.framesSubmitted;
    g.frameCv.notify_all();
}

uint32_t eye_width() { return g.eyeSwapchains[0].width; }
uint32_t eye_height() { return g.eyeSwapchains[0].height; }
WGPUTextureFormat target_format() { return g.targetFormat; }

WGPUTextureView eye_target_view(int eye) {
    return (eye == 0 || eye == 1) && g.eyeTargets[eye] ? g.eyeTargets[eye].view : nullptr;
}

WGPUTextureView ensure_quad_target(uint32_t width, uint32_t height) {
    if (g.session == XR_NULL_HANDLE || width == 0 || height == 0) {
        return nullptr;
    }
    if (g.quad != nullptr && g.quad->swapchain.width == width && g.quad->swapchain.height == height) {
        return g.quad->target.view;
    }
    if (g.quad != nullptr) {
        std::lock_guard lock{g.frameMutex};
        g.retiredQuads.emplace_back(g.nextId, g.quad);
        g.quad = nullptr;
    }
    auto* q = new QuadResources();
    if (!create_swapchain(q->swapchain, width, height) ||
        !d3d::create_captured_texture(g.device, g.d3dDevice.Get(), width, height, g.targetFormat, "VR quad", q->target))
    {
        destroy_quad(q);
        return nullptr;
    }
    std::lock_guard lock{g.frameMutex};
    g.quad = q;
    return q->target.view;
}

void* quad_token() { return g.quad; }

void simulation_readback_once() {
    // Sample well after the scene has faded in (the first stereo frames are a black load screen).
    static int frames = 0;
    if (++frames != 1200 || !g.eyeTargets[0] || !g.d3dDevice) {
        return;
    }
    if (!g.fence && !create_copy_infra()) {
        return;
    }
    // Same path as a real frame: barrier the captured Dawn resource to COPY_SOURCE on Dawn's queue
    // right after Dawn's submit, copy, and barrier back. The destination is a readback buffer here.
    const auto& src = g.eyeTargets[0];
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
        return;
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
    gpu_wait_idle();
    uint8_t* data = nullptr;
    D3D12_RANGE range{0, static_cast<SIZE_T>(total)};
    if (SUCCEEDED(buffer->Map(0, &range, reinterpret_cast<void**>(&data)))) {
        const uint8_t* px = data + fp.Offset + (src.height / 2) * fp.Footprint.RowPitch + (src.width / 2) * 4;
        mods::log::info("D3D12 copy self-test: centre pixel of left eye = {},{},{},{}", int{px[0]}, int{px[1]},
            int{px[2]}, int{px[3]});
        std::fflush(stdout);
        D3D12_RANGE none{0, 0};
        buffer->Unmap(0, &none);
    }
}

bool ensure_simulation_targets(uint32_t width, uint32_t height) {
    if (g.session != XR_NULL_HANDLE || !g.d3dDevice) {
        return false;
    }
    if (g.eyeTargets[0] && g.eyeTargets[0].width == width && g.eyeTargets[0].height == height) {
        return true;
    }
    for (int eye = 0; eye < 2; ++eye) {
        if (!d3d::create_captured_texture(g.device, g.d3dDevice.Get(), width, height, g.targetFormat,
                eye == 0 ? "VR sim eye L" : "VR sim eye R", g.eyeTargets[eye]))
        {
            return false;
        }
    }
    mods::log::info("Simulation eye targets {}x{} captured (D3D12 resource {}, barrier model {})", width, height,
        static_cast<void*>(g.eyeTargets[0].resource.Get()),
        g.eyeTargets[0].barriers == d3d::BarrierModel::Enhanced ? "enhanced" : "legacy");
    return true;
}

void simulated_views(FrameInfo& out) {
    out = {};
    out.id = 0;
    out.shouldRender = true;
    out.viewsValid = true;
    const float halfIpd = 0.032f;
    for (int eye = 0; eye < 2; ++eye) {
        out.views[eye].pose.position = {eye == 0 ? -halfIpd : halfIpd, 0.0f, 0.0f};
        out.views[eye].fov = {std::tan(-50.0f * kPi / 180.0f), std::tan(50.0f * kPi / 180.0f),
            std::tan(45.0f * kPi / 180.0f), std::tan(-45.0f * kPi / 180.0f)};
    }
    // Slightly inward-canted frusta like real headsets.
    out.views[0].fov.tanRight = std::tan(40.0f * kPi / 180.0f);
    out.views[1].fov.tanLeft = std::tan(-40.0f * kPi / 180.0f);
}

} // namespace vr::xr
