#include "xr_runtime.hpp"

#include "interop.hpp"
#include "vr_config.hpp"

#include "mods/svc/log.hpp"

#ifdef _WIN32
#define XR_USE_PLATFORM_WIN32
#else
#define XR_USE_PLATFORM_ANDROID
#endif

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



struct Swapchain {
    XrSwapchain handle = XR_NULL_HANDLE;
    uint32_t width = 0;
    uint32_t height = 0;
    std::vector<interop::SwapchainImage> images; // runtime-owned
};

// The quad (HUD / virtual screen) resources are recreated when the game's framebuffer size
// changes. Frames already in flight keep a raw pointer to the set they were rendered with, so
// retired sets are only destroyed once those frames have ended.
struct QuadResources {
    Swapchain swapchain;
    interop::Target target;
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
    bool seeThrough = false;
    std::array<QuadLayer, kQuadSlots> quad{};
    std::array<QuadResources*, kQuadSlots> quadRes{};
};

constexpr size_t kRecordRing = 8;

struct State {
    // Graphics
    WGPUDevice device = nullptr;
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
    std::array<interop::Target, 2> eyeTargets;
    std::array<QuadResources*, kQuadSlots> quads{};
    std::vector<std::pair<uint64_t, QuadResources*>> retiredQuads;
    // Frame bookkeeping (guarded by frameMutex).
    std::mutex frameMutex;
    std::condition_variable frameCv;
    std::array<FrameRecord, kRecordRing> records{};
    uint64_t nextId = 1;
    uint64_t lastWaited = 0;
    Armed armed;

    // See-through (tabletop): XR_FB_passthrough layer if the runtime has it, else the ALPHA_BLEND
    // environment blend mode, else opaque.
    bool hasFbPassthrough = false; // extension enabled on the instance
    bool hasAlphaBlend = false;    // session's system lists ALPHA_BLEND
    bool wantSeeThrough = false;   // game thread
    bool passthroughFailed = false;
    PFN_xrCreatePassthroughFB createPassthrough = nullptr;
    PFN_xrDestroyPassthroughFB destroyPassthrough = nullptr;
    PFN_xrCreatePassthroughLayerFB createPassthroughLayer = nullptr;
    PFN_xrDestroyPassthroughLayerFB destroyPassthroughLayer = nullptr;
    XrPassthroughFB passthrough = XR_NULL_HANDLE;
    XrPassthroughLayerFB passthroughLayer = XR_NULL_HANDLE; // guarded by frameMutex

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

// --- Swapchain copies ----------------------------------------------------------------------------

struct CopyJob {
    Swapchain* swapchain;
    const interop::Target* source;
    uint32_t imageIndex = 0;
};

// Acquire each destination image, copy our targets into them (the interop layer orders this after
// Dawn's frame work), release. Never blocks the CPU on the GPU except when a copy slot is reused.
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

    std::vector<interop::CopyJob> copies;
    copies.reserve(jobs.size());
    for (const auto& job : jobs) {
        copies.push_back({job.source, job.swapchain->images[job.imageIndex]});
    }
    bool ok = interop::copy_to_swapchains(copies);

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
    // Runtime order is its preference; take the first one the interop layer can feed.
    for (int64_t f : formats) {
        for (const auto& c : interop::candidate_formats()) {
            if (c.native == f) {
                g.swapchainFormat = f;
                g.targetFormat = c.wgpu;
                mods::log::info("OpenXR swapchain format: {} ({})", f, interop::backend_name());
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
    if (!interop::enumerate_swapchain_images(reinterpret_cast<uint64_t>(sc.handle), sc.images)) {
        mods::log::error("OpenXR xrEnumerateSwapchainImages failed");
        return false;
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
    interop::destroy_target(q->target);
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

// --- Passthrough -------------------------------------------------------------------------------

void destroy_passthrough() {
    XrPassthroughLayerFB layer = XR_NULL_HANDLE;
    {
        std::lock_guard lock{g.frameMutex};
        layer = g.passthroughLayer;
        g.passthroughLayer = XR_NULL_HANDLE;
    }
    if (layer != XR_NULL_HANDLE) {
        g.destroyPassthroughLayer(layer);
    }
    if (g.passthrough != XR_NULL_HANDLE) {
        g.destroyPassthrough(g.passthrough);
        g.passthrough = XR_NULL_HANDLE;
        mods::log::info("Passthrough stopped");
    }
}

bool create_passthrough() {
    XrPassthroughCreateInfoFB ci{XR_TYPE_PASSTHROUGH_CREATE_INFO_FB};
    ci.flags = XR_PASSTHROUGH_IS_RUNNING_AT_CREATION_BIT_FB;
    if (!xr_ok(g.createPassthrough(g.session, &ci, &g.passthrough), "xrCreatePassthroughFB")) {
        g.passthrough = XR_NULL_HANDLE;
        return false;
    }
    XrPassthroughLayerCreateInfoFB li{XR_TYPE_PASSTHROUGH_LAYER_CREATE_INFO_FB};
    li.passthrough = g.passthrough;
    li.flags = XR_PASSTHROUGH_IS_RUNNING_AT_CREATION_BIT_FB;
    li.purpose = XR_PASSTHROUGH_LAYER_PURPOSE_RECONSTRUCTION_FB;
    XrPassthroughLayerFB layer = XR_NULL_HANDLE;
    if (!xr_ok(g.createPassthroughLayer(g.session, &li, &layer), "xrCreatePassthroughLayerFB")) {
        g.destroyPassthrough(g.passthrough);
        g.passthrough = XR_NULL_HANDLE;
        return false;
    }
    std::lock_guard lock{g.frameMutex};
    g.passthroughLayer = layer;
    mods::log::info("Passthrough started (XR_FB_passthrough)");
    return true;
}

// Game thread: bring the passthrough layer up or down to match the tabletop setting.
void update_passthrough() {
    const bool want = g.wantSeeThrough && g.hasFbPassthrough && g.sessionRunning && !g.passthroughFailed;
    if (want && g.passthrough == XR_NULL_HANDLE) {
        if (!create_passthrough()) {
            g.passthroughFailed = true; // fall back to ALPHA_BLEND / opaque for this session
        }
    } else if (!want && g.passthrough != XR_NULL_HANDLE) {
        destroy_passthrough();
    }
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
    interop::wait_idle();
    destroy_passthrough();
    g.passthroughFailed = false;
    g.hasAlphaBlend = false;
    for (auto& sc : g.eyeSwapchains) {
        destroy_swapchain(sc);
    }
    for (auto& t : g.eyeTargets) {
        interop::destroy_target(t);
    }
    for (auto*& q : g.quads) {
        destroy_quad(q);
        q = nullptr;
    }
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

    const void* graphicsBinding = nullptr;
    if (!interop::prepare_system(reinterpret_cast<uintptr_t>(g.instance), g.system, graphicsBinding)) {
        return false;
    }
    XrSessionCreateInfo ci{XR_TYPE_SESSION_CREATE_INFO};
    ci.next = graphicsBinding;
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
    if (viewCount < 2 || !pick_swapchain_format()) {
        teardown_session();
        return false;
    }

    uint32_t blendCount = 0;
    xrEnumerateEnvironmentBlendModes(
        g.instance, g.system, XR_VIEW_CONFIGURATION_TYPE_PRIMARY_STEREO, 0, &blendCount, nullptr);
    std::vector<XrEnvironmentBlendMode> blends(blendCount);
    xrEnumerateEnvironmentBlendModes(
        g.instance, g.system, XR_VIEW_CONFIGURATION_TYPE_PRIMARY_STEREO, blendCount, &blendCount, blends.data());
    g.hasAlphaBlend = std::ranges::find(blends, XR_ENVIRONMENT_BLEND_MODE_ALPHA_BLEND) != blends.end();
    mods::log::info("See-through support: XR_FB_passthrough {}, ALPHA_BLEND {}", g.hasFbPassthrough, g.hasAlphaBlend);

    const float scale = config().renderScale;
    for (int eye = 0; eye < 2; ++eye) {
        const auto& v = views[eye];
        const uint32_t w = std::clamp(static_cast<uint32_t>(v.recommendedImageRectWidth * scale), 64u,
            v.maxImageRectWidth);
        const uint32_t h = std::clamp(static_cast<uint32_t>(v.recommendedImageRectHeight * scale), 64u,
            v.maxImageRectHeight);
        if (!create_swapchain(g.eyeSwapchains[eye], w, h) ||
            !interop::create_target(w, h, g.targetFormat, eye == 0 ? "VR eye L" : "VR eye R", g.eyeTargets[eye]))
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
            interop::wait_idle();
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

bool initialize(WGPUDevice device, WGPUAdapter adapter, bool createInstance) {
    if (g.instance != XR_NULL_HANDLE) {
        return true;
    }
    if (!interop::initialize(device, adapter)) {
        return false;
    }
    g.device = device;

    if (!createInstance) {
        mods::log::info("OpenXR disabled (DUSKLIGHT_VR_NO_XR); simulation only");
        return true;
    }
    uint32_t extCount = 0;
    if (XR_FAILED(xrEnumerateInstanceExtensionProperties(nullptr, 0, &extCount, nullptr))) {
        mods::log::warn("No OpenXR runtime installed; VR unavailable");
        return false;
    }
    std::vector<XrExtensionProperties> exts(extCount, {XR_TYPE_EXTENSION_PROPERTIES});
    xrEnumerateInstanceExtensionProperties(nullptr, extCount, &extCount, exts.data());
    const auto offered = [&exts](const char* name) {
        return std::ranges::any_of(
            exts, [name](const XrExtensionProperties& e) { return std::strcmp(e.extensionName, name) == 0; });
    };
    uint32_t requiredCount = 0;
    const char* const* required = interop::required_extensions(requiredCount);
    std::vector<const char*> enabled;
    for (uint32_t i = 0; i < requiredCount; ++i) {
        if (!offered(required[i])) {
            mods::log::error("The active OpenXR runtime does not support {} ({} backend)", required[i],
                interop::backend_name());
            return false;
        }
        enabled.push_back(required[i]);
    }
    const bool hasPassthrough = offered(XR_FB_PASSTHROUGH_EXTENSION_NAME);
    if (hasPassthrough) {
        enabled.push_back(XR_FB_PASSTHROUGH_EXTENSION_NAME);
    }
    XrInstanceCreateInfo ci{XR_TYPE_INSTANCE_CREATE_INFO};
    std::strncpy(ci.applicationInfo.applicationName, "Dusklight VR", XR_MAX_APPLICATION_NAME_SIZE - 1);
    std::strncpy(ci.applicationInfo.engineName, "Dusklight", XR_MAX_ENGINE_NAME_SIZE - 1);
    ci.applicationInfo.apiVersion = XR_API_VERSION_1_0;
    ci.enabledExtensionCount = static_cast<uint32_t>(enabled.size());
    ci.enabledExtensionNames = enabled.data();
    if (!xr_ok(xrCreateInstance(&ci, &g.instance), "xrCreateInstance")) {
        return false;
    }
    if (hasPassthrough) {
        auto load = [](const char* name, auto& fn) {
            xrGetInstanceProcAddr(g.instance, name, reinterpret_cast<PFN_xrVoidFunction*>(&fn));
        };
        load("xrCreatePassthroughFB", g.createPassthrough);
        load("xrDestroyPassthroughFB", g.destroyPassthrough);
        load("xrCreatePassthroughLayerFB", g.createPassthroughLayer);
        load("xrDestroyPassthroughLayerFB", g.destroyPassthroughLayer);
        g.hasFbPassthrough = g.createPassthrough && g.destroyPassthrough && g.createPassthroughLayer &&
                             g.destroyPassthroughLayer;
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
    interop::shutdown();
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

    update_passthrough();
    retire_old_resources();
}

void set_see_through(bool want) { g.wantSeeThrough = want; }

bool see_through_available() { return g.passthroughLayer != XR_NULL_HANDLE || g.hasAlphaBlend; }

const char* see_through_mode() {
    if (g.passthroughLayer != XR_NULL_HANDLE) {
        return "passthrough";
    }
    if (g.hasAlphaBlend) {
        return "alpha blend";
    }
    return "none (opaque)";
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
           std::to_string(g.framesDiscarded) + " dropped" +
           (g.wantSeeThrough ? std::string(", see-through: ") + see_through_mode() : std::string());
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

void arm_submit(uint64_t id, bool stereo, bool seeThrough, const QuadLayer (&quads)[kQuadSlots],
    void* const (&quadTokens)[kQuadSlots]) {
    std::lock_guard lock{g.frameMutex};
    g.armed = {id, stereo, seeThrough, {}, {}};
    for (int i = 0; i < kQuadSlots; ++i) {
        g.armed.quad[i] = quads[i];
        g.armed.quadRes[i] = quads[i].enabled ? static_cast<QuadResources*>(quadTokens[i]) : nullptr;
    }
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
    std::array<bool, kQuadSlots> withQuad{};
    for (int i = 0; i < kQuadSlots; ++i) {
        QuadResources* quad = armed.quadRes[i];
        withQuad[i] = r->shouldRender && armed.quad[i].enabled && quad != nullptr && quad->target;
        if (withQuad[i]) {
            jobs.push_back({&quad->swapchain, &quad->target});
        }
    }
    const bool copied = !jobs.empty() && copy_into_swapchains(jobs);

    std::array<XrCompositionLayerProjectionView, 2> projViews{};
    XrCompositionLayerProjection proj{XR_TYPE_COMPOSITION_LAYER_PROJECTION};
    std::array<XrCompositionLayerQuad, kQuadSlots> quadLayers{};
    XrCompositionLayerPassthroughFB ptLayer{XR_TYPE_COMPOSITION_LAYER_PASSTHROUGH_FB};
    std::vector<const XrCompositionLayerBaseHeader*> layers;
    XrEnvironmentBlendMode blendMode = XR_ENVIRONMENT_BLEND_MODE_OPAQUE;
    // The tabletop eye images carry premultiplied alpha (0 around the diorama).
    const bool seeThrough = copied && stereo && armed.seeThrough;
    if (seeThrough) {
        if (g.passthroughLayer != XR_NULL_HANDLE) {
            ptLayer.flags = XR_COMPOSITION_LAYER_BLEND_TEXTURE_SOURCE_ALPHA_BIT;
            ptLayer.layerHandle = g.passthroughLayer;
            layers.push_back(reinterpret_cast<const XrCompositionLayerBaseHeader*>(&ptLayer));
        } else if (g.hasAlphaBlend) {
            blendMode = XR_ENVIRONMENT_BLEND_MODE_ALPHA_BLEND;
        }
        proj.layerFlags = XR_COMPOSITION_LAYER_BLEND_TEXTURE_SOURCE_ALPHA_BIT;
    }
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
    for (int i = 0; i < kQuadSlots && copied; ++i) {
        if (!withQuad[i]) {
            continue;
        }
        const QuadLayer& q = armed.quad[i];
        const QuadResources* res = armed.quadRes[i];
        XrCompositionLayerQuad& layer = quadLayers[i];
        layer = {XR_TYPE_COMPOSITION_LAYER_QUAD};
        layer.layerFlags = q.premultipliedAlpha ? XR_COMPOSITION_LAYER_BLEND_TEXTURE_SOURCE_ALPHA_BIT : 0;
        layer.space = q.space == QuadSpace::View ? g.viewSpace : g.appSpace;
        layer.eyeVisibility = XR_EYE_VISIBILITY_BOTH;
        layer.subImage.swapchain = res->swapchain.handle;
        layer.subImage.imageRect = {
            {0, 0}, {static_cast<int32_t>(res->swapchain.width), static_cast<int32_t>(res->swapchain.height)}};
        layer.pose = to_xr(q.pose);
        layer.size = {q.width, q.height};
        layers.push_back(reinterpret_cast<const XrCompositionLayerBaseHeader*>(&layer));
    }

    XrFrameEndInfo end{XR_TYPE_FRAME_END_INFO};
    end.displayTime = r->displayTime;
    end.environmentBlendMode = blendMode;
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

WGPUTextureView ensure_quad_target(int slot, uint32_t width, uint32_t height) {
    if (g.session == XR_NULL_HANDLE || width == 0 || height == 0 || slot < 0 || slot >= kQuadSlots) {
        return nullptr;
    }
    QuadResources*& cur = g.quads[slot];
    if (cur != nullptr && cur->swapchain.width == width && cur->swapchain.height == height) {
        return cur->target.view;
    }
    if (cur != nullptr) {
        std::lock_guard lock{g.frameMutex};
        g.retiredQuads.emplace_back(g.nextId, cur);
        cur = nullptr;
    }
    auto* q = new QuadResources();
    if (!create_swapchain(q->swapchain, width, height) ||
        !interop::create_target(width, height, g.targetFormat,
            slot == kQuadUi ? "VR UI quad" : "VR quad", q->target))
    {
        destroy_quad(q);
        return nullptr;
    }
    std::lock_guard lock{g.frameMutex};
    cur = q;
    return q->target.view;
}

void* quad_token(int slot) { return slot >= 0 && slot < kQuadSlots ? g.quads[slot] : nullptr; }

void simulation_readback_once() {
    // Sample well after the scene has faded in (the first stereo frames are a black load screen).
    static int frames = 0;
    if (++frames != 1200 || !g.eyeTargets[0]) {
        return;
    }
    uint8_t px[4] = {};
    if (interop::read_center_pixel(g.eyeTargets[0], px)) {
        mods::log::info("Copy self-test: centre pixel of left eye = {},{},{},{}", int{px[0]}, int{px[1]}, int{px[2]},
            int{px[3]});
        std::fflush(stdout);
    }
}

bool ensure_simulation_targets(uint32_t width, uint32_t height) {
    if (g.session != XR_NULL_HANDLE || g.device == nullptr) {
        return false;
    }
    if (g.eyeTargets[0] && g.eyeTargets[0].width == width && g.eyeTargets[0].height == height) {
        return true;
    }
    for (int eye = 0; eye < 2; ++eye) {
        if (!interop::create_target(width, height, g.targetFormat,
                eye == 0 ? "VR sim eye L" : "VR sim eye R", g.eyeTargets[eye]))
        {
            return false;
        }
    }
    mods::log::info("Simulation eye targets {}x{} ready ({} interop)", width, height, interop::backend_name());
    return true;
}

void simulated_views(FrameInfo& out, float pitchDeg) {
    out = {};
    out.id = 0;
    out.shouldRender = true;
    out.viewsValid = true;
    const float halfIpd = 0.032f;
    const float pitch = pitchDeg * kPi / 180.0f;
    const Quat tilt{std::sin(pitch * 0.5f), 0.0f, 0.0f, std::cos(pitch * 0.5f)};
    for (int eye = 0; eye < 2; ++eye) {
        out.views[eye].pose.orientation = tilt;
        out.views[eye].pose.position = {eye == 0 ? -halfIpd : halfIpd, 0.0f, 0.0f};
        out.views[eye].fov = {std::tan(-50.0f * kPi / 180.0f), std::tan(50.0f * kPi / 180.0f),
            std::tan(45.0f * kPi / 180.0f), std::tan(-45.0f * kPi / 180.0f)};
    }
    // Slightly inward-canted frusta like real headsets.
    out.views[0].fov.tanRight = std::tan(40.0f * kPi / 180.0f);
    out.views[1].fov.tanLeft = std::tan(-40.0f * kPi / 180.0f);
}

} // namespace vr::xr
