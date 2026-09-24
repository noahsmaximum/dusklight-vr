// Game headers first: windows.h (pulled in below) defines macros such as IN that collide with game enums.
#include "JSystem/J3DGraphAnimator/J3DModel.h"
#include "JSystem/J3DGraphBase/J3DSys.h"
#include "d/d_com_inf_game.h"
#include "f_op/f_op_camera_mng.h"
#include "f_op/f_op_view.h"

#include "render_hooks.hpp"

#include "gpu.hpp"
#ifdef _WIN32
#include "iat_hook.hpp"
#endif
#include "vr_config.hpp"
#include "vr_math.hpp"
#include "xr_runtime.hpp"
#include "xray.hpp"

#include "mods/svc/gfx.h"
#include "mods/svc/hook.hpp"
#include "mods/svc/log.hpp"

#include <aurora/aurora.h>
#include <dolphin/gx/GXAurora.h>

#include <algorithm>
#include <array>
#include <atomic>
#include <chrono>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <memory>
#include <mutex>
#include <string_view>
#include <type_traits>
#include <vector>

// --- Hook targets ------------------------------------------------------------------------------------

DEFINE_HOOK_SYMBOL("aurora_begin_frame", bool(), AuroraBeginFrame);
DEFINE_HOOK_SYMBOL("aurora_end_frame", void(), AuroraEndFrame);
// Render worker, right after the frame is submitted (and presented). One-line wrapper; its callee is
// the fallback in case the wrapper is inlined.
DEFINE_HOOK_SYMBOL("aurora::gfx::after_submit", void(), AfterSubmit);
DEFINE_HOOK_SYMBOL("aurora::gfx::depth_peek::after_submit", void(), DepthPeekAfterSubmit);
DEFINE_HOOK_SYMBOL("mDoGph_Painter", int(), Painter);
DEFINE_HOOK_SYMBOL("dusk::mods::gfx_run_stage", void(int, const void*, const void*), RunStage);
DEFINE_HOOK_SYMBOL("dusk::game_clock::original_frames", float(), OriginalFrames);
DEFINE_HOOK_SYMBOL("dusk::ImGuiConsole::PreDraw", void(void*), ImguiPreDraw);
DEFINE_HOOK_SYMBOL("dusk::ImGuiConsole::PostDraw", void(void*), ImguiPostDraw);
DEFINE_HOOK_SYMBOL("src/m_Do/m_Do_graphic.cpp#motionBlure", void(view_class*), MotionBlur);
DEFINE_HOOK_SYMBOL("src/m_Do/m_Do_graphic.cpp#trimming", void(view_class*, view_port_class*), Trimming);
DEFINE_HOOK_SYMBOL("mDoLib_clipper::setup", void(f32, f32, f32, f32), ClipperSetup);
DEFINE_HOOK_SYMBOL("aurora::window::get_window_size", AuroraWindowSize(), GetWindowSize);
// Overloaded members are named by their decorated names, which differ per ABI (MSVC / Itanium).
#ifdef _WIN32
#define VR_SYM_CLIP_SPHERE "?clip@J3DUClipper@@QEBAHPEAY03$$CBMUVec@@M@Z"
#define VR_SYM_CLIP_BOX "?clip@J3DUClipper@@QEBAHPEAY03$$CBMPEAUVec@@1@Z"
#define VR_SYM_CALC_TEX_MTX "?calcTexMtx@J3DTexMtx@@QEAAXQEAY03$$CBM@Z"
#define VR_SYM_CALC_POST_TEX_MTX "?calcPostTexMtx@J3DTexMtx@@QEAAXQEAY03$$CBM@Z"
#else
#define VR_SYM_CLIP_SPHERE "_ZNK11J3DUClipper4clipEPA4_Kf3Vecf"
#define VR_SYM_CLIP_BOX "_ZNK11J3DUClipper4clipEPA4_KfP3VecS4_"
#define VR_SYM_CALC_TEX_MTX "_ZN9J3DTexMtx10calcTexMtxEPA4_Kf"
#define VR_SYM_CALC_POST_TEX_MTX "_ZN9J3DTexMtx14calcPostTexMtxEPA4_Kf"
#endif
// Tabletop: the two J3DUClipper::clip overloads (sphere, box), the sky lists, fog.
DEFINE_HOOK_SYMBOL(VR_SYM_CLIP_SPHERE, int(const void*, const f32 (*)[4], Vec, f32),
    ClipSphere);
DEFINE_HOOK_SYMBOL(VR_SYM_CLIP_BOX, int(const void*, const f32 (*)[4], Vec*, Vec*),
    ClipBox);
DEFINE_HOOK_SYMBOL("dComIfGd_drawOpaListSky", void(), DrawOpaSky);
DEFINE_HOOK_SYMBOL("dComIfGd_drawXluListSky", void(), DrawXluSky);
DEFINE_HOOK_SYMBOL("GXSetFog", void(GXFogType, f32, f32, f32, f32, GXColor), SetFog);
// Screen-space projective texturing (water reflection/refraction) builds its texgen from the view's fovy/aspect.
DEFINE_HOOK_SYMBOL("C_MTXLightPerspective", void(f32 (*)[4], f32, f32, f32, f32, f32, f32), LightPerspective);
// Re-run per eye (see run_interp_callbacks); the camera's callback must not touch the eye view.
DEFINE_HOOK_SYMBOL("src/d/d_camera.cpp#widezoom_correction", void(void*, f32), WidezoomCorrection);
// Map/BG water materials: projection texgen computed from the game view in the actors' draw.
DEFINE_HOOK_SYMBOL("dKy_bg_MAxx_proc", void(void*), BgMaterialProc);
// Frame interpolation's callback list, mirrored where callbacks_run itself has no symbol (clang
// inlines its only call site). The overloads need their decorated names.
using InterpCallback = void (*)(void*);
DEFINE_HOOK_SYMBOL("dusk::interp::begin_sim_tick", void(), InterpBeginSimTick);
#ifndef _WIN32
DEFINE_HOOK_SYMBOL("_ZN4dusk6interp26add_interpolation_callbackEPFvPvES1_", void(InterpCallback, void*),
    InterpAddCallback);
DEFINE_HOOK_SYMBOL("_ZN4dusk6interp26add_interpolation_callbackEPFvPvES1_NSt6__ndk110shared_ptrIvEE",
    void(InterpCallback, void*, std::shared_ptr<void>), InterpAddOwnedCallback);
#endif
// View-projection texture matrices (J3D modes 3/9): effect matrix x view x model.
DEFINE_HOOK_SYMBOL(VR_SYM_CALC_TEX_MTX, void(J3DTexMtx*, const f32 (*)[4]), CalcTexMtx);
// The same for models drawn with post-transform texture matrices (model flag 0x20), where modes
// 3/9 are effect x SRT alone, applied to view-space positions.
DEFINE_HOOK_SYMBOL(VR_SYM_CALC_POST_TEX_MTX, void(J3DTexMtx*, const f32 (*)[4]), CalcPostTexMtx);

// Dusklight's own UI (RmlUi) renders into aurora::rmlui::s_renderTarget during aurora_end_frame.
// record_frame returns {bind group (null when nothing was drawn), overlay}; mirrored layouts below.
struct RmlRecordedFrame {
    void* bindGroup;
    bool overlay;
    // Aurora's RecordedFrame holds a wgpu::BindGroup, so it is non-trivial for the purposes of calls
    // and comes back through the hidden return pointer. The mirror must be returned the same way: as
    // a trivial 16-byte struct arm64 returns it in x0/x1, the trampoline never passes x8 on, and the
    // real function writes its result through a stale pointer (the first-frame crash on Quest).
    // Windows x64 returns both in memory, which is why it only broke on Android.
    RmlRecordedFrame() = default;
    RmlRecordedFrame(const RmlRecordedFrame&) = default;
    RmlRecordedFrame& operator=(const RmlRecordedFrame&) = default;
    ~RmlRecordedFrame() {}
};
static_assert(!std::is_trivially_destructible_v<RmlRecordedFrame>, "must be returned in memory like aurora's");
struct RmlRenderTarget { // aurora::webgpu::TextureWithSampler
    WGPUTexture texture;
    WGPUTextureView view;
    WGPUExtent3D size;
    WGPUTextureFormat format;
    WGPUSampler sampler;
};
DEFINE_HOOK_SYMBOL("aurora::rmlui::record_frame", RmlRecordedFrame(const void*), RmlRecordFrame);
// With a visible backdrop filter (glass blur) RmlUi renders the whole scene as its base layer
// (BaseLayerContent::Scene = 1) and record_frame reports overlay = false.
DEFINE_HOOK_SYMBOL("aurora::rmlui::WebGPURenderInterface::BeginFrame", void(void*, const void*, const void*, int),
    RmlBeginFrame);

namespace vr::render {
namespace {

using ConfigOverrideFn = void (*)(std::string_view, std::string_view);

// Everything the game thread learns about the frame it is recording.
struct Frame {
    Mode mode = Mode::Off;
    bool active = false;   // VR handles this frame
    uint64_t xrId = 0;     // 0 when there is no XR frame (simulation)
    xr::FrameInfo info;    // eye views used to render
    int eye = -1;          // eye currently being painted, -1 outside the stereo loop
    bool hudCapturing = false; // the 2D phase is drawing over our black/white clear
    WGPUTextureView sceneNoHud = nullptr; // 3D scene saved before the 2D phase
    bool hasCamera = false;
    std::array<WGPUTextureView, 2> scene{};
    std::array<WGPUTextureView, 2> hud{}; // [0] over black, [1] over white
    WGPUTextureView mono = nullptr;
    xr::QuadLayer quad;
    bool tabletop = false; // diorama camera + cut applied this frame
    std::array<gpu::CutParams, 2> cut{};
    bool xrayReady = false; // this eye's x-ray data is uploaded (xrayFog points at it)
    bool xrayOn = false;    // the 3D phase of a tabletop eye draws with the x-ray fog
    xray::FogArgs xrayFog{};
    // The current eye's projection and the symmetric fovy/aspect written into its view_class.
    Mtx44f eyeProj;
    f32 eyeFovy = 0.0f, eyeAspect = 0.0f;
};
Frame f;

// Handed to the render worker through a compute payload (the payload itself carries only the slot).
struct Packet {
    uint64_t seq = 0;
    uint64_t xrId = 0;
    bool stereo = false;
    bool tabletop = false;   // eye images carry alpha
    bool seeThrough = false; // ...and the runtime should show the room behind them
    gpu::BlitMode eyeBlit = gpu::BlitMode::Opaque;
    uint32_t keyColor = 0;
    std::array<WGPUTextureView, 2> scene{};
    std::array<WGPUTextureView, 2> hud{};
    WGPUTextureView mono = nullptr;
    std::array<WGPUTextureView, 2> eyeTargets{};
    std::array<WGPUTextureView, xr::kQuadSlots> quadTargets{};
    void* quadTokens[xr::kQuadSlots] = {};
    WGPUTextureFormat targetFormat = WGPUTextureFormat_Undefined;
    xr::QuadLayer quads[xr::kQuadSlots] = {};
    WGPUTextureView ui = nullptr; // Dusklight UI snapshot (owned reference, released by the worker)
};
void prepare_ui_quad(Packet& p);

struct PacketRef {
    uint32_t slot;
    uint64_t seq;
};
constexpr size_t kPackets = 8;
std::mutex g_packetMutex;
std::array<Packet, kPackets> g_packets;
uint64_t g_packetSeq = 0;

GfxComputeTypeHandle g_beginCompute = 0;
GfxComputeTypeHandle g_finishCompute = 0;

std::atomic<bool> g_simulationArmed{false};

#ifdef _WIN32
std::atomic<DWORD> g_workerThread{0};
using QueueSubmitFn = void (*)(WGPUQueue, size_t, const WGPUCommandBuffer*);
QueueSubmitFn g_origQueueSubmit = nullptr;
#endif

ConfigOverrideFn g_configOverride = nullptr;
bool g_overridesApplied = false;

// Frame interpolation's per-presentation callbacks (J3DModel::calcMaterial for every recorded model).
using CallbacksRunFn = void (*)();
CallbacksRunFn g_callbacksRun = nullptr;
bool g_rerunningCallbacks = false;
// Models passed to dKy_bg_MAxx_proc by this frame's actor draws (cleared after the painter).
std::vector<void*> g_bgModels;

// Our copy of frame interpolation's callback list, for builds where callbacks_run can't be called.
// It follows the game's rules: add_interpolation_callback accepts only while capturing a sim tick
// outside a presentation, begin_sim_tick clears (when interpolating), and the whole history is
// dropped when interpolation stops or the presentation epoch moves.
namespace interp_mirror {
struct Work {
    InterpCallback fn;
    void* work;
    std::shared_ptr<void> owner; // keeps the work alive exactly as the game's entry does
};
struct FrameTiming { // dusk::game_clock::FrameTiming
    float dt;
    bool interpolating;
    bool separatePresentation;
    int numSimTicks;
    uint64_t presentationEpoch;
};
using BoolFn = bool (*)();

std::vector<Work> g_list;
uint64_t g_epoch = 0;
BoolFn g_enabled = nullptr;
BoolFn g_shouldCapture = nullptr;
BoolFn g_presentationActive = nullptr;
const FrameTiming* g_timing = nullptr;

void sync_history() {
    if (!g_timing->interpolating || g_timing->presentationEpoch != g_epoch) {
        g_list.clear();
        g_epoch = g_timing->presentationEpoch;
    }
}

bool accepts(InterpCallback fn) {
    return fn != nullptr && g_shouldCapture() && !g_presentationActive();
}

[[maybe_unused]] HookAction begin_sim_tick_pre(ModContext*, void*, void*, void*) {
    if (g_enabled()) {
        g_list.clear();
    }
    return HOOK_CONTINUE;
}

[[maybe_unused]] HookAction add_pre(ModContext*, void* args, void*, void*) {
    const auto fn = mods::arg<InterpCallback>(args, 0);
    if (accepts(fn)) {
        sync_history();
        g_list.push_back({fn, mods::arg<void*>(args, 1), {}});
    }
    return HOOK_CONTINUE;
}

[[maybe_unused]] HookAction add_owned_pre(ModContext*, void* args, void*, void*) {
    const auto fn = mods::arg<InterpCallback>(args, 0);
    if (accepts(fn)) {
        sync_history();
        g_list.push_back({fn, mods::arg<void*>(args, 1), mods::arg_ref<std::shared_ptr<void>>(args, 2)});
    }
    return HOOK_CONTINUE;
}

void run() {
    sync_history();
    for (const Work& w : g_list) {
        w.fn(w.work);
    }
}

bool install() {
#ifdef _WIN32
    return false; // callbacks_run resolves on Windows
#else
    void* enabled = nullptr;
    void* shouldCapture = nullptr;
    void* presentationActive = nullptr;
    void* timing = nullptr;
    if (svc_hook->resolve(mod_ctx, "dusk::interp::is_enabled", &enabled, nullptr) != MOD_OK ||
        svc_hook->resolve(mod_ctx, "dusk::interp::should_capture", &shouldCapture, nullptr) != MOD_OK ||
        svc_hook->resolve(mod_ctx, "dusk::interp::is_presentation_active", &presentationActive, nullptr) != MOD_OK ||
        svc_hook->resolve(mod_ctx, "dusk::game_clock::g_frameTiming", &timing, nullptr) != MOD_OK)
    {
        return false;
    }
    g_enabled = reinterpret_cast<BoolFn>(enabled);
    g_shouldCapture = reinterpret_cast<BoolFn>(shouldCapture);
    g_presentationActive = reinterpret_cast<BoolFn>(presentationActive);
    g_timing = static_cast<const FrameTiming*>(timing);
    return mods::hook::add_pre<InterpBeginSimTick>(begin_sim_tick_pre) == MOD_OK &&
           mods::hook::add_pre<InterpAddCallback>(add_pre) == MOD_OK &&
           mods::hook::add_pre<InterpAddOwnedCallback>(add_owned_pre) == MOD_OK;
#endif
}
} // namespace interp_mirror

// Dusklight UI state from the last aurora_end_frame (game thread).
const RmlRenderTarget* g_rmlTarget = nullptr;
bool g_rmlDrew = false;

// Placement state for the head-following HUD and world-locked menus.
struct Placement {
    bool init = false;
    float hudYaw = 0.0f;
    Vec3 hudAnchor;
    bool hudTurning = false;
    bool menuWasOpen = false;
    Pose menuPose;
    bool uiWasOpen = false;
    Pose uiPose;
};
Placement g_place;

// Where the diorama is looking in the game world (smoothed so the table glides under the player).
struct Table {
    bool init = false;
    Vec3 anchor; // world units
    float align = 0.0f; // world yaw that maps the game camera's heading to "away from the player"
    int64_t lastTicks = 0;
};
Table g_table;

uint64_t g_stereoFrames = 0;
uint64_t g_monoFrames = 0;

// Internal framebuffer override while a headset session runs (0 = follow the window).
std::atomic<uint32_t> g_efbWidth{0};
std::atomic<uint32_t> g_efbHeight{0};
using RefreshSurfaceFn = bool (*)(bool);
RefreshSurfaceFn g_refreshSurface = nullptr;

// Frame timing (game thread), exponentially smoothed milliseconds.
struct Timing {
    double frameMs = 0, waitMs = 0, painterMs = 0;
    int64_t lastFrameStart = 0;
};
Timing g_timing;

int64_t now_ticks() {
    return std::chrono::steady_clock::now().time_since_epoch().count();
}

double ticks_to_ms(int64_t ticks) {
    using Period = std::chrono::steady_clock::period;
    return static_cast<double>(ticks) * 1000.0 * Period::num / Period::den;
}

void smooth(double& avg, double sample) { avg = avg == 0 ? sample : avg + (sample - avg) * 0.05; }

// --- Camera -------------------------------------------------------------------------------------

Mtx34 load(const Mtx m) {
    Mtx34 r;
    std::memcpy(r.m, m, sizeof(r.m));
    return r;
}

void store(const Mtx34& a, Mtx m) { std::memcpy(m, a.m, sizeof(a.m)); }

// Removes pitch and roll from a view matrix, keeping the camera position and heading. The player's
// head then supplies all pitch/roll, which keeps the horizon still.
Mtx34 level_view(const Mtx34& view) {
    const Mtx34 world = inverse_affine(view);
    const Vec3 pos{world.m[0][3], world.m[1][3], world.m[2][3]};
    // Row 2 of the view rotation is the camera's backward axis in world space.
    const Vec3 fwd = normalize(Vec3{-view.m[2][0], -view.m[2][1], -view.m[2][2]});
    const Vec3 flat = normalize(Vec3{fwd.x, 0.0f, fwd.z});
    if (length(flat) < 0.5f) {
        return view; // looking straight up/down: keep the game camera
    }
    const Vec3 up{0.0f, 1.0f, 0.0f};
    const Vec3 right = normalize(cross(flat, up));
    const Vec3 back = flat * -1.0f;
    Mtx34 r;
    const Vec3 rows[3] = {right, up, back};
    for (int i = 0; i < 3; ++i) {
        r.m[i][0] = rows[i].x;
        r.m[i][1] = rows[i].y;
        r.m[i][2] = rows[i].z;
        r.m[i][3] = -dot(rows[i], pos);
    }
    return r;
}

struct ViewBackup {
    f32 near_, far_, fovy, aspect;
    lookat_class lookat;
    s16 bank;
    Mtx44 projMtx;
    Mtx viewMtx, invViewMtx;
    Mtx44 projViewMtx;
    Mtx viewMtxNoTrans;
};

void backup_view(const view_class& v, ViewBackup& b) {
    b.near_ = v.near_;
    b.far_ = v.far_;
    b.fovy = v.fovy;
    b.aspect = v.aspect;
    b.lookat = v.lookat;
    b.bank = v.bank;
    std::memcpy(b.projMtx, v.projMtx, sizeof(Mtx44));
    std::memcpy(b.viewMtx, v.viewMtx, sizeof(Mtx));
    std::memcpy(b.invViewMtx, v.invViewMtx, sizeof(Mtx));
    std::memcpy(b.projViewMtx, v.projViewMtx, sizeof(Mtx44));
    std::memcpy(b.viewMtxNoTrans, v.viewMtxNoTrans, sizeof(Mtx));
}

void restore_view(view_class& v, const ViewBackup& b) {
    v.near_ = b.near_;
    v.far_ = b.far_;
    v.fovy = b.fovy;
    v.aspect = b.aspect;
    v.lookat = b.lookat;
    v.bank = b.bank;
    std::memcpy(v.projMtx, b.projMtx, sizeof(Mtx44));
    std::memcpy(v.viewMtx, b.viewMtx, sizeof(Mtx));
    std::memcpy(v.invViewMtx, b.invViewMtx, sizeof(Mtx));
    std::memcpy(v.projViewMtx, b.projViewMtx, sizeof(Mtx44));
    std::memcpy(v.viewMtxNoTrans, b.viewMtxNoTrans, sizeof(Mtx));
}

// Eye pose with the stereo separation scaled around the head centre.
Pose eye_pose(const xr::FrameInfo& info, int eye) {
    const Vec3 c = (info.views[0].pose.position + info.views[1].pose.position) * 0.5f;
    Pose p = info.views[eye].pose;
    p.position = c + (p.position - c) * config().ipdScale;
    return p;
}

// Rewrites the camera's view_class for one eye. `trackingFromWorld` maps the game world into the
// tracking space (in game units): the game camera itself in stereo mode, the table placement in
// tabletop mode. The eye pose (metres -> game units via `unitsPerMeter`) is applied on top, and the
// projection becomes the headset's asymmetric frustum while keeping the game's depth mapping.
void apply_eye(view_class& v, const ViewBackup& base, const Mtx34& trackingFromWorld, float unitsPerMeter, int eye) {
    const Pose pose = eye_pose(f.info, eye);
    const Mtx34 eyeFromTracking = inverse_rigid(pose_to_mtx(pose, unitsPerMeter));
    const Mtx34 view = mul(eyeFromTracking, trackingFromWorld);

    const Fov& fov = f.info.views[eye].fov;
    const float w = fov.tanRight - fov.tanLeft;
    const float h = fov.tanUp - fov.tanDown;
    Mtx44f proj;
    proj.m[0][0] = 2.0f / w;
    proj.m[0][2] = (fov.tanRight + fov.tanLeft) / w;
    proj.m[1][1] = 2.0f / h;
    proj.m[1][2] = (fov.tanUp + fov.tanDown) / h;
    proj.m[2][2] = base.projMtx[2][2];
    proj.m[2][3] = base.projMtx[2][3];
    proj.m[3][2] = -1.0f;

    store(view, v.viewMtx);
    const Mtx34 inv = inverse_affine(view);
    store(inv, v.invViewMtx);
    Mtx34 noTrans = view;
    noTrans.m[0][3] = noTrans.m[1][3] = noTrans.m[2][3] = 0.0f;
    store(noTrans, v.viewMtxNoTrans);
    std::memcpy(v.projMtx, proj.m, sizeof(Mtx44));
    const Mtx44f projView = mul(proj, view);
    std::memcpy(v.projViewMtx, projView.m, sizeof(Mtx44));

    // fovy/aspect stay the game camera's: everything that builds a screen-space projection from
    // them (projected water, particles) then describes the game projection, and the hooks below
    // swap that for the eye's real frustum.
    v.fovy = base.fovy;
    v.aspect = base.aspect;
    f.eyeProj = proj;
    f.eyeFovy = v.fovy;
    f.eyeAspect = v.aspect;

    const Vec3 eyePos{inv.m[0][3], inv.m[1][3], inv.m[2][3]};
    const Vec3 fwd{-inv.m[0][2], -inv.m[1][2], -inv.m[2][2]};
    v.lookat.eye.set(eyePos.x, eyePos.y, eyePos.z);
    const Vec3 center = eyePos + fwd * 100.0f;
    v.lookat.center.set(center.x, center.y, center.z);
    v.lookat.up.set(inv.m[0][1], inv.m[1][1], inv.m[2][1]);
    v.bank = 0;
}

float wrap_angle(float a);

// --- Tabletop -----------------------------------------------------------------------------------

// Places the game world on the table: the (smoothed) player position sits at the table centre and
// the game camera's heading points away from the player, so stick directions still match what the
// player sees. Returns trackingFromWorld in game units (tracking metres x table scale).
Mtx34 table_transform(const Mtx34& gameView, const ViewBackup& base) {
    const auto& cfg = config();
    const float s = cfg.tableUnitsPerMeter;

    const int64_t now = now_ticks();
    const float dt = g_table.lastTicks != 0 ? std::clamp(static_cast<float>(ticks_to_ms(now - g_table.lastTicks)) / 1000.0f, 0.0f, 0.1f) : 0.0f;
    g_table.lastTicks = now;

    Vec3 target{base.lookat.center.x, base.lookat.center.y, base.lookat.center.z};
    if (fopAc_ac_c* player = dComIfGp_getPlayer(0)) {
        target = {player->current.pos.x, player->current.pos.y, player->current.pos.z};
    }
    const Vec3 fwd{-gameView.m[2][0], -gameView.m[2][1], -gameView.m[2][2]};
    const float camYaw = std::atan2(-fwd.x, -fwd.z);

    const float radius = cfg.tableRadius * s;
    const Vec3 delta = target - g_table.anchor;
    if (!g_table.init || length(delta) > radius * 4.0f) {
        // First frame, or a warp / room change: jump straight there.
        g_table.init = true;
        g_table.anchor = target;
        g_table.align = -camYaw;
    } else {
        // Horizontal: follow once the player leaves a small dead zone around the centre.
        const float deadZone = radius * 0.1f;
        const Vec3 flat{delta.x, 0.0f, delta.z};
        const float dist = length(flat);
        if (dist > deadZone) {
            const float k = 1.0f - std::exp(-dt * 5.0f);
            g_table.anchor = g_table.anchor + normalize(flat) * ((dist - deadZone) * k);
        }
        // Vertical: slower, so jumps and small steps don't bob the table.
        g_table.anchor.y += delta.y * (1.0f - std::exp(-dt * 2.0f));
        if (cfg.tableFollowYaw) {
            g_table.align = wrap_angle(g_table.align + wrap_angle(-camYaw - g_table.align) * (1.0f - std::exp(-dt * 4.0f)));
        }
    }

    const Vec3 tablePos{cfg.tableOffsetX * s, cfg.tableHeight * s, -cfg.tableDistance * s};
    const float yaw = g_table.align + cfg.tableYawDeg * kPi / 180.0f;
    return mul(translation34(tablePos), mul(rot_y34(yaw), translation34(g_table.anchor * -1.0f)));
}

// Uniforms for the cut shader of one eye, from the view/projection apply_eye just wrote.
gpu::CutParams cut_params(const view_class& v) {
    const auto& cfg = config();
    const float s = cfg.tableUnitsPerMeter;
    const bool reversed = gpu::reversed_z();

    // Clip space as Aurora feeds it to the GPU (see fill_uniform in aurora's shader_info.cpp).
    Mtx44f proj;
    std::memcpy(proj.m, v.projMtx, sizeof(proj.m));
    for (int c = 0; c < 4; ++c) {
        proj.m[2][c] = reversed ? -proj.m[2][c] : proj.m[2][c] + proj.m[3][c];
    }
    const Mtx44f clipFromWorld = mul(proj, load(v.viewMtx));
    Mtx44f worldFromClip;
    gpu::CutParams p;
    if (!inverse44(clipFromWorld, worldFromClip)) {
        return p;
    }
    for (int r = 0; r < 4; ++r) {
        for (int c = 0; c < 4; ++c) {
            p.worldFromClip[c * 4 + r] = worldFromClip.m[r][c];
        }
    }
    const float radius = cfg.tableRadius * s;
    p.anchor[0] = g_table.anchor.x;
    p.anchor[1] = g_table.anchor.y;
    p.anchor[2] = g_table.anchor.z;
    p.anchor[3] = radius;
    p.params[0] = radius * 0.08f;                               // soft rim
    p.params[1] = g_table.anchor.y - cfg.tableDepth * s;         // floor
    p.params[2] = 0.02f * s;                                     // floor fade (2 cm)
    p.params[3] = reversed ? 0.0f : 1.0f;                        // cleared depth
    return p;
}

void efb_size(uint32_t& w, uint32_t& h);

// Tabletop x-ray for the eye apply_eye just set up: uploads its data. The x-ray fog itself runs
// from SCENE_BEGIN (start_xray; offscreen renders before it, such as the minimap, stay untouched)
// to FRAME_BEFORE_HUD (end_xray); xray.cpp enforces it in between.
void begin_xray(const view_class& v, const gpu::CutParams& cut) {
    f.xrayOn = false;
    f.xrayReady = false;
    const auto& cfg = config();
    const fopAc_ac_c* player = dComIfGp_getPlayer(0);
    if (!xray::available() || SetFog::g_orig == nullptr || player == nullptr ||
        (!cfg.tableXray && cfg.tableFadeNear <= 0.0f))
    {
        return;
    }
    xray::EyeParams p{};
    std::memcpy(p.worldFromClip, cut.worldFromClip, sizeof(p.worldFromClip));
    const Mtx34 worldFromEye = inverse_affine(load(v.viewMtx));
    p.eye[0] = worldFromEye.m[0][3];
    p.eye[1] = worldFromEye.m[1][3];
    p.eye[2] = worldFromEye.m[2][3];
    p.target[0] = player->current.pos.x;
    p.target[1] = player->current.pos.y + 75.0f; // about half of Link's height
    p.target[2] = player->current.pos.z;
    uint32_t w, h;
    efb_size(w, h);
    p.targetWidth = static_cast<float>(w);
    p.targetHeight = static_cast<float>(h);
    p.radius = cfg.tableXray ? cfg.tableXrayRadius : 0.0f;
    p.taper = cfg.tableXrayRadius * 0.5f;
    p.margin = 30.0f;
    const float fade = cfg.tableFadeNear * cfg.tableUnitsPerMeter;
    p.fadeFar = fade > 0.0f ? fade : -1.0f;
    p.fadeNear = fade > 0.0f ? fade * 0.5f : -2.0f;
    f.xrayReady = xray::push_eye(p, f.xrayFog);
}

void start_xray() {
    if (f.xrayReady && !f.xrayOn) {
        f.xrayOn = true;
        SetFog::g_orig(static_cast<GXFogType>(xray::kFogType), f.xrayFog.startZ, f.xrayFog.endZ, f.xrayFog.nearZ,
            f.xrayFog.farZ, GXColor{0, 0, 0, 0});
    }
}

void end_xray() {
    f.xrayReady = false;
    if (f.xrayOn) {
        f.xrayOn = false;
        const auto& m = xray::kEndMarker;
        SetFog::g_orig(GX_FOG_NONE, m.startZ, m.endZ, m.nearZ, m.farZ, GXColor{0, 0, 0, 0});
    }
}

bool tabletop_culling_off() {
    return config().mode == Mode::Tabletop && (xr::session_running() || config().simulateHmd);
}

// --- Quad placement ---------------------------------------------------------------------------------

float wrap_angle(float a) {
    while (a > kPi) a -= 2.0f * kPi;
    while (a < -kPi) a += 2.0f * kPi;
    return a;
}

void efb_size(uint32_t& w, uint32_t& h) {
    AuroraGetRenderSize(&w, &h);
    if (w == 0 || h == 0) {
        w = 1920;
        h = 1080;
    }
}

xr::QuadLayer place_quad(bool menuOpen, bool screen) {
    const auto& cfg = config();
    uint32_t w, h;
    efb_size(w, h);
    const float aspect = static_cast<float>(h) / static_cast<float>(w);

    Pose head;
    head.position = (f.info.views[0].pose.position + f.info.views[1].pose.position) * 0.5f;
    head.orientation = f.info.views[0].pose.orientation;
    const float headYaw = yaw_of(head.orientation);
    if (!g_place.init) {
        g_place.init = true;
        g_place.hudYaw = headYaw;
        g_place.hudAnchor = head.position;
    }

    xr::QuadLayer q;
    q.enabled = true;
    q.premultipliedAlpha = true;

    if (screen) {
        q.space = xr::QuadSpace::App;
        q.pose.position = {0.0f, 0.0f, -cfg.screenDistance};
        q.width = cfg.screenWidth;
        q.height = cfg.screenWidth * aspect;
        return q;
    }

    if (menuOpen) {
        if (!g_place.menuWasOpen) {
            g_place.menuPose.orientation = quat_from_yaw(headYaw);
            g_place.menuPose.position =
                head.position + rotate(g_place.menuPose.orientation, Vec3{0.0f, -0.05f, -cfg.menuDistance});
        }
        g_place.menuWasOpen = true;
        q.space = xr::QuadSpace::App;
        q.pose = g_place.menuPose;
        q.width = cfg.menuWidth;
        q.height = cfg.menuWidth * aspect;
        return q;
    }
    g_place.menuWasOpen = false;

    q.width = cfg.hudWidth;
    q.height = cfg.hudWidth * aspect;
    switch (cfg.hudFollow) {
    case HudFollow::HeadLocked:
        q.space = xr::QuadSpace::View;
        q.pose.position = {0.0f, cfg.hudHeight, -cfg.hudDistance};
        break;
    case HudFollow::World:
        q.space = xr::QuadSpace::App;
        q.pose.position = {0.0f, cfg.hudHeight, -cfg.hudDistance};
        break;
    case HudFollow::Smooth: {
        // Stay put while the head looks around the panel; once it turns well away, glide back in
        // front of it.
        const float diff = wrap_angle(headYaw - g_place.hudYaw);
        if (std::fabs(diff) > 30.0f * kPi / 180.0f) {
            g_place.hudTurning = true;
        }
        if (g_place.hudTurning) {
            g_place.hudYaw = wrap_angle(g_place.hudYaw + diff * 0.08f);
            if (std::fabs(diff) < 2.0f * kPi / 180.0f) {
                g_place.hudTurning = false;
            }
        }
        g_place.hudAnchor = g_place.hudAnchor + (head.position - g_place.hudAnchor) * 0.08f;
        q.space = xr::QuadSpace::App;
        q.pose.orientation = quat_from_yaw(g_place.hudYaw);
        q.pose.position =
            g_place.hudAnchor + rotate(q.pose.orientation, Vec3{0.0f, cfg.hudHeight, -cfg.hudDistance});
        break;
    }
    }
    return q;
}

// --- Worker-side callbacks --------------------------------------------------------------------------

void begin_compute(ModContext*, const GfxComputeContext*, const void* payload, size_t size, void*) {
    if (size == sizeof(uint64_t)) {
        xr::begin_frame(*static_cast<const uint64_t*>(payload));
    }
}

void finish_compute(ModContext*, const GfxComputeContext* ctx, const void* payload, size_t size, void*) {
    if (size != sizeof(PacketRef)) {
        return;
    }
    const auto ref = *static_cast<const PacketRef*>(payload);
    Packet p;
    {
        std::lock_guard lock{g_packetMutex};
        p = g_packets[ref.slot];
        if (p.seq != ref.seq) {
            return;
        }
        g_packets[ref.slot].ui = nullptr; // this callback owns the UI view reference now
    }
    struct ReleaseUi {
        WGPUTextureView view;
        ~ReleaseUi() {
            if (view != nullptr) {
                wgpuTextureViewRelease(view);
            }
        }
    } releaseUi{p.ui};
    if (p.stereo) {
        for (int eye = 0; eye < 2; ++eye) {
            gpu::blit(ctx->encoder, p.scene[eye], p.eyeTargets[eye], p.targetFormat, p.eyeBlit, p.keyColor);
        }
        static bool logged = false;
        if (!logged) {
            logged = true;
            mods::log::info("First eye composition encoded on the render worker");
        }
    }
    const WGPUTextureView hudTarget = p.quadTargets[xr::kQuadHud];
    if (p.quads[xr::kQuadHud].enabled && hudTarget != nullptr) {
        if (p.hud[0] != nullptr && p.hud[1] != nullptr) {
            gpu::combine_hud(ctx->encoder, p.hud[0], p.hud[1], hudTarget, p.targetFormat);
        } else if (p.mono != nullptr) {
            gpu::blit(ctx->encoder, p.mono, hudTarget, p.targetFormat);
        }
    }
    const WGPUTextureView uiTarget = p.quadTargets[xr::kQuadUi];
    if (p.quads[xr::kQuadUi].enabled && uiTarget != nullptr && p.ui != nullptr) {
        // RmlUi renders premultiplied (transparent where there is no UI).
        gpu::blit(ctx->encoder, p.ui, uiTarget, p.targetFormat, gpu::BlitMode::KeepAlpha);
    }
#ifdef _WIN32
    g_workerThread.store(GetCurrentThreadId(), std::memory_order_release);
#endif
    if (p.xrId != 0) {
        xr::arm_submit(p.xrId, p.stereo, p.seeThrough, p.quads, p.quadTokens);
    } else if (p.stereo) {
        g_simulationArmed.store(true, std::memory_order_release);
    }
}

// Runs on the render worker right after the frame's command buffer was submitted: copy the eye/quad
// targets into the XR swapchains and end the XR frame.
void frame_submitted() {
    static bool logged = false;
    if (!logged) {
        logged = true;
        mods::log::info("First frame delivered after submit");
    }
    xr::on_queue_submitted();
    if (g_simulationArmed.exchange(false)) {
        xr::simulation_readback_once();
    }
}

void after_submit_post(ModContext*, void*, void*, void*) { frame_submitted(); }

// Fallback (Windows only): observe the exe's wgpuQueueSubmit import.
#ifdef _WIN32
void hk_queue_submit(WGPUQueue queue, size_t count, const WGPUCommandBuffer* buffers) {
    g_origQueueSubmit(queue, count, buffers);
    if (GetCurrentThreadId() == g_workerThread.load(std::memory_order_acquire)) {
        frame_submitted();
    }
}
#endif

// --- Game-thread hooks ------------------------------------------------------------------------------

void apply_game_overrides() {
    if (g_overridesApplied || g_configOverride == nullptr) {
        return;
    }
    g_overridesApplied = true;
    // In-memory overrides (never written to config.json): decouple rendering from the 30 Hz
    // simulation so every headset refresh gets a fresh frame, let xrWaitFrame (not the desktop
    // monitor) pace presentation, and drop effects that break in stereo.
    g_configOverride("game.enableFrameInterpolation", "2");
    g_configOverride("video.enableVsync", "false");
    g_configOverride("game.enableMirrorMode", "false");
    g_configOverride("game.disableLetterboxing", "1");
    // Depth of field through the game's own setting: drawDepth2 must still run, because it also
    // makes the framebuffer copy that water refraction samples (skipping it = "portal" water).
    if (config().disableDof) {
        g_configOverride("game.depthOfFieldMode", "0");
    }
    mods::log::info("Applied VR overrides: frame interpolation unlimited, vsync off, no letterbox/mirror");
}

// While a headset session runs, render the game at the headset's eye resolution with the game's
// native 4:3 aspect instead of following the desktop window (an ultrawide window would otherwise
// waste most of its pixels once squeezed into a near-square eye image, twice per frame).
void update_efb_override() {
    uint32_t w = 0;
    uint32_t h = 0;
    if ((f.mode == Mode::Stereo || f.mode == Mode::Tabletop) && xr::session_running() && xr::eye_height() != 0) {
        h = xr::eye_height() & ~1u;
        w = ((h * 4 + 2) / 3) & ~1u;
    }
#ifdef __ANDROID__
    // Cinema on a standalone headset: the app window is huge (4128x2208 on a Quest 3) but the
    // virtual screen covers only part of each eye, so render at about what the headset can resolve
    // there: eye pixels across the screen's angular width (typical per-eye tangent span ~2.4), with
    // some supersampling for the quad layer's filtering.
    else if (f.mode == Mode::Cinema && xr::session_running() && xr::eye_width() != 0) {
        constexpr float kEyeTanSpan = 2.4f;
        constexpr float kSupersample = 1.5f;
        const auto& cfg = config();
        const float px = static_cast<float>(xr::eye_width()) * (cfg.screenWidth / cfg.screenDistance) / kEyeTanSpan *
                         kSupersample;
        w = static_cast<uint32_t>(std::clamp(px, 960.0f, 2560.0f)) & ~1u;
        h = ((w * 9 + 8) / 16) & ~1u;
    }
#endif
    if (w == g_efbWidth.load() && h == g_efbHeight.load()) {
        return;
    }
    g_efbWidth.store(w);
    g_efbHeight.store(h);
    if (g_refreshSurface != nullptr) {
        g_refreshSurface(false);
    }
    if (w != 0) {
        mods::log::info("Rendering at {}x{} for the headset", w, h);
    } else {
        mods::log::info("Rendering at the window resolution again");
    }
}

void window_size_post(ModContext*, void*, void* retval, void*) {
    const uint32_t w = g_efbWidth.load(std::memory_order_relaxed);
    const uint32_t h = g_efbHeight.load(std::memory_order_relaxed);
    if (w != 0 && h != 0 && retval != nullptr) {
        auto* size = static_cast<AuroraWindowSize*>(retval);
        size->fb_width = w;
        size->fb_height = h;
#ifdef __ANDROID__
        // On a standalone headset nobody sees the app's 2D surface while the session runs, yet every
        // frame Aurora resamples, blits, draws ImGui and sizes RmlUi's target at the surface size
        // (~4K on a Quest 3): a fixed ~15 ms. Configure the surface at the render size instead.
        size->native_fb_width = w;
        size->native_fb_height = h;
#endif
    }
}

HookAction begin_frame_pre(ModContext*, void*, void*, void*) {
    const int64_t start = now_ticks();
    if (g_timing.lastFrameStart != 0) {
        smooth(g_timing.frameMs, ticks_to_ms(start - g_timing.lastFrameStart));
    }
    g_timing.lastFrameStart = start;

    refresh_config();
    f = {};
    const auto& cfg = config();
    f.mode = cfg.mode;
    xr::set_see_through(f.mode == Mode::Tabletop && cfg.tablePassthrough);
    if (f.mode != Mode::Tabletop) {
        g_table.init = false; // re-place the diorama next time
    }
    if (f.mode != Mode::Off) {
        xr::poll();
    }
    update_efb_override();
    if (f.mode == Mode::Off) {
        return HOOK_CONTINUE;
    }
    if (xr::session_running()) {
        apply_game_overrides();
        const int64_t waitStart = now_ticks();
        f.xrId = xr::wait_frame();
        smooth(g_timing.waitMs, ticks_to_ms(now_ticks() - waitStart));
    }
    f.active = f.xrId != 0 || (cfg.simulateHmd && (f.mode == Mode::Stereo || f.mode == Mode::Tabletop));
    return HOOK_CONTINUE;
}

void begin_frame_post(ModContext*, void*, void* retval, void*) {
    const bool ok = retval != nullptr && *static_cast<bool*>(retval);
    if (!ok) {
        if (f.xrId != 0) {
            xr::abandon_frame(f.xrId);
        }
        f = {};
        return;
    }
    if (f.xrId != 0) {
        if (const ModResult r = svc_gfx->push_compute(mod_ctx, g_beginCompute, &f.xrId, sizeof(f.xrId)); r != MOD_OK) {
            mods::log::error("push_compute(begin) failed ({})", static_cast<int>(r));
        }
    }
}

// Frame interpolation recomputes model materials once per presented frame, before the painter, with
// the game camera's view. Texture matrices that depend on the view (projected water/refraction,
// environment maps) and view-space lights would then be the game camera's in both eyes, so the
// screen copy the water samples lands in the wrong place ("portal" water). Recompute them with the
// eye's view before painting it.
// The same goes for map water (materials MA10/MA02), which dKy_bg_MAxx_proc sets up in the actors'
// draw from the game view (its C_MTXLightPerspective is corrected to the eye frustum by the hook).
void run_interp_callbacks(const view_class& v) {
    if (g_callbacksRun == nullptr && g_bgModels.empty()) {
        return;
    }
    // dKy_bg_MAxx_proc switches the current draw lists; nothing may be entered elsewhere afterwards.
    J3DDrawBuffer* opa = j3dSys.getDrawBuffer(J3DSysDrawBuf_Opa);
    J3DDrawBuffer* xlu = j3dSys.getDrawBuffer(J3DSysDrawBuf_Xlu);
    j3dSys.setViewMtx(v.viewMtx);
    g_rerunningCallbacks = true;
    for (void* model : g_bgModels) {
        BgMaterialProc::g_orig(model);
        // Recompute the texture matrices with the eye view and patch them into the display lists.
        auto* j3dModel = static_cast<J3DModel*>(model);
        j3dModel->calcMaterial();
        j3dModel->diff();
    }
    if (g_callbacksRun != nullptr) {
        g_callbacksRun();
    }
    g_rerunningCallbacks = false;
    j3dSys.setDrawBuffer(opa, J3DSysDrawBuf_Opa);
    j3dSys.setDrawBuffer(xlu, J3DSysDrawBuf_Xlu);
    j3dSys.setViewMtx(v.viewMtx);
}

HookAction bg_material_pre(ModContext*, void* args, void*, void*) {
    if (!g_rerunningCallbacks && config().mode != Mode::Off) {
        void* model = mods::arg<void*>(args, 0);
        if (model != nullptr && std::find(g_bgModels.begin(), g_bgModels.end(), model) == g_bgModels.end()) {
            g_bgModels.push_back(model);
        }
    }
    return HOOK_CONTINUE;
}

HookAction widezoom_pre(ModContext*, void*, void*, void*) {
    return g_rerunningCallbacks ? HOOK_SKIP_ORIGINAL : HOOK_CONTINUE;
}

WGPUTextureView resolve_color() {
    GfxResolveDesc desc = GFX_RESOLVE_DESC_INIT;
    desc.color = true;
    GfxResolvedTargets out = GFX_RESOLVED_TARGETS_INIT;
    if (svc_gfx->resolve_pass(mod_ctx, &desc, &out) != MOD_OK) {
        return nullptr;
    }
    return out.color;
}

// Ends the 2D capture: snapshot the HUD (drawn over black/white) and put the 3D scene back so the
// rest of the painter (screen fader) draws over the scene as usual.
void end_hud_capture() {
    if (!f.hudCapturing) {
        return;
    }
    f.hudCapturing = false;
    const WGPUTextureView view = resolve_color();
    if (f.eye == 0 || f.eye == 1) {
        f.hud[f.eye] = view;
    }
    gpu::push_restore(f.sceneNoHud, f.tabletop);
}

void painter_replace(ModContext*, void*, void* retval, void*) {
    // Recorded by this frame's actor draws; the next frame's draws record them again (and models
    // can be deleted by the simulation in between).
    struct ClearBgModels {
        ~ClearBgModels() { g_bgModels.clear(); }
    } clearBgModels;
    int result = 1;
    auto run = [&] { result = Painter::g_orig(); };

    if (!f.active) {
        run();
        if (retval) *static_cast<int*>(retval) = result;
        return;
    }

    bool stereo = false;
    if (f.mode == Mode::Stereo || f.mode == Mode::Tabletop) {
        if (f.xrId != 0) {
            stereo = xr::locate_views(f.xrId, f.info);
        } else if (config().simulateHmd) {
            xr::simulated_views(f.info, f.mode == Mode::Tabletop ? -40.0f : 0.0f);
            stereo = true;
        }
    }

    if (!stereo) {
        run();
        if (f.mode == Mode::Cinema) {
            f.mono = resolve_color();
            f.quad = place_quad(false, true);
            f.quad.premultipliedAlpha = false;
        }
        ++g_monoFrames;
        if (retval) *static_cast<int*>(retval) = result;
        return;
    }

    camera_process_class* cam = nullptr;
    if (dComIfGp_getWindowNum() != 0) {
        if (dDlst_window_c* window = dComIfGp_getWindow(0)) {
            cam = dComIfGp_getCamera(window->getCameraID());
        }
    }
    f.hasCamera = cam != nullptr;

    ViewBackup backup{};
    Mtx34 trackingFromWorld;
    float unitsPerMeter = config().unitsPerMeter;
    f.tabletop = cam != nullptr && f.mode == Mode::Tabletop;
    if (cam != nullptr) {
        backup_view(cam->view, backup);
        const Mtx34 gameView = load(backup.viewMtx);
        if (f.tabletop) {
            trackingFromWorld = table_transform(gameView, backup);
            unitsPerMeter = config().tableUnitsPerMeter;
        } else {
            trackingFromWorld = config().levelHorizon ? level_view(gameView) : gameView;
        }
    }

    const int64_t paintStart = now_ticks();
    for (int eye = 0; eye < 2; ++eye) {
        f.eye = eye;
        if (eye == 1) {
            // The painter never clears depth itself (the XFB copy does on hardware), so the second
            // eye would otherwise depth-test against the first eye's scene.
            gpu::push_clear();
        }
        if (cam != nullptr) {
            apply_eye(cam->view, backup, trackingFromWorld, unitsPerMeter, eye);
            if (f.tabletop) {
                f.cut[eye] = cut_params(cam->view);
                begin_xray(cam->view, f.cut[eye]);
            }
            run_interp_callbacks(cam->view);
        }
        run();
        end_xray();        // in case the painter skipped the HUD stage
        end_hud_capture();
        f.scene[eye] = resolve_color();
    }
    f.eye = -1;
    smooth(g_timing.painterMs, ticks_to_ms(now_ticks() - paintStart));
    if (cam != nullptr) {
        restore_view(cam->view, backup);
        j3dSys.setViewMtx(cam->view.viewMtx);
    }

    const auto& cfg = config();
    if (cfg.simulateHmd && f.xrId == 0) {
        gpu::push_mirror({f.scene[0], f.scene[1], f.hud[0], f.hud[1]});
    } else if (cfg.mirrorHud) {
        gpu::push_mirror({nullptr, nullptr, f.hud[0], f.hud[1]});
    }

    f.quad = place_quad(dComIfGp_isPauseFlag() != 0, !f.hasCamera);
    ++g_stereoFrames;
    if (retval) *static_cast<int*>(retval) = result;
}

HookAction run_stage_pre(ModContext*, void* args, void*, void*) {
    if (mods::arg<int>(args, 0) == GFX_STAGE_FRAME_AFTER_HUD) {
        end_hud_capture();
    }
    return HOOK_CONTINUE;
}

void run_stage_post(ModContext*, void* args, void*, void*) {
    if (mods::arg<int>(args, 0) == GFX_STAGE_SCENE_BEGIN && f.eye >= 0) {
        start_xray();
        return;
    }
    if (mods::arg<int>(args, 0) != GFX_STAGE_FRAME_BEFORE_HUD || f.eye < 0 || f.hudCapturing) {
        return;
    }
    // Everything the painter draws from here to FRAME_AFTER_HUD is 2D (HUD, text, menus). Save the
    // scene, then let the 2D phase draw into the main framebuffer over black (first eye) or white
    // (second eye); the pair gives the compositor exact alpha for the headset's quad layer. This
    // stays on the EFB (not an offscreen pass) so the game's 2D viewport scaling applies.
    end_xray(); // the 2D phase is never cut
    if (f.tabletop) {
        // Fade out everything beyond the table (and the empty background) using this eye's depth.
        GfxResolveDesc desc = GFX_RESOLVE_DESC_INIT;
        desc.color = false;
        desc.depth = true;
        GfxResolvedTargets out = GFX_RESOLVED_TARGETS_INIT;
        if (svc_gfx->resolve_pass(mod_ctx, &desc, &out) == MOD_OK && out.depth != nullptr) {
            gpu::push_cut(out.depth, f.cut[f.eye]);
        }
    }
    f.sceneNoHud = resolve_color();
    if (f.sceneNoHud == nullptr) {
        return;
    }
    gpu::push_clear(f.eye == 1);
    f.hudCapturing = true;
}

HookAction original_frames_pre(ModContext*, void*, void* retval, void*) {
    // UI animation timers advance by frame time inside the painter; the second eye must not
    // advance them again.
    if (f.eye == 1) {
        *static_cast<float*>(retval) = 0.0f;
        return HOOK_SKIP_ORIGINAL;
    }
    return HOOK_CONTINUE;
}

HookAction skip_on_second_eye(ModContext*, void*, void*, void*) {
    return f.eye == 1 ? HOOK_SKIP_ORIGINAL : HOOK_CONTINUE;
}

HookAction motion_blur_pre(ModContext*, void*, void*, void*) {
    // Motion blur blends in the previous frame's capture, which in stereo is the other eye.
    return f.eye >= 0 ? HOOK_SKIP_ORIGINAL : HOOK_CONTINUE;
}

HookAction trimming_pre(ModContext*, void*, void*, void*) {
    return f.active ? HOOK_SKIP_ORIGINAL : HOOK_CONTINUE;
}

HookAction clipper_setup_pre(ModContext*, void* args, void*, void*) {
    // Culling happens at simulation time against the game camera. In VR the player looks around
    // that camera, so widen the frustum or objects pop in at the edges of the headset view.
    if (config().mode == Mode::Stereo && (f.active || xr::session_running())) {
        f32& fovy = mods::arg_ref<f32>(args, 0);
        fovy = std::max(fovy, static_cast<f32>(config().cullFovDeg));
    }
    return HOOK_CONTINUE;
}

HookAction clip_pre(ModContext*, void*, void* retval, void*) {
    // The diorama shows everything around the player, including what is behind the game camera,
    // so nothing may be frustum-culled against it.
    if (tabletop_culling_off()) {
        *static_cast<int*>(retval) = 0;
        return HOOK_SKIP_ORIGINAL;
    }
    return HOOK_CONTINUE;
}

HookAction sky_pre(ModContext*, void*, void*, void*) {
    // No sky in tabletop: the empty background becomes transparent (the real room shows through).
    return f.eye >= 0 && f.tabletop ? HOOK_SKIP_ORIGINAL : HOOK_CONTINUE;
}

HookAction fog_pre(ModContext*, void* args, void*, void*) {
    // Fog is computed from the distance to the eye, which in tabletop is thousands of game units
    // away from everything (the whole diorama would fog over).
    // (During the x-ray phase the fog registers are replaced further down, see xray.hpp.)
    if (f.eye >= 0 && f.tabletop) {
        mods::arg_ref<GXFogType>(args, 0) = GX_FOG_NONE;
    }
    return HOOK_CONTINUE;
}

void light_perspective_post(ModContext*, void* args, void*, void*) {
    // Water samples a copy of the framebuffer through a texgen built from the camera's fovy/aspect.
    // In an eye that symmetric frustum doesn't match the asymmetric one the geometry is drawn with,
    // so the copy lands misregistered and repeats ("portal" water). Rebuild the matrix from the
    // eye's real projection: s*q = scaleS*(P00*x + P02*z) - transS*z, q = -z.
    // During the material re-run the result becomes a J3D effect matrix, which calc_tex_mtx_pre
    // corrects (rewriting it here as well would apply the eye frustum twice).
    if (f.eye < 0 || g_rerunningCallbacks || f.eyeFovy == 0.0f || mods::arg<f32>(args, 1) != f.eyeFovy ||
        mods::arg<f32>(args, 2) != f.eyeAspect)
    {
        return;
    }
    f32(*m)[4] = mods::arg<f32(*)[4]>(args, 0);
    const f32 scaleS = mods::arg<f32>(args, 3);
    const f32 scaleT = mods::arg<f32>(args, 4);
    const f32 transS = mods::arg<f32>(args, 5);
    const f32 transT = mods::arg<f32>(args, 6);
    const auto& p = f.eyeProj.m;
    m[0][0] = scaleS * p[0][0];
    m[0][1] = 0.0f;
    m[0][2] = scaleS * p[0][2] - transS;
    m[0][3] = 0.0f;
    m[1][0] = 0.0f;
    m[1][1] = scaleT * p[1][1];
    m[1][2] = scaleT * p[1][2] - transT;
    m[1][3] = 0.0f;
    static bool logged = false;
    if (!logged) {
        logged = true;
        mods::log::info("Corrected a screen-space (water) projection for the eye frustum");
    }
}

// J3D view-projection texgens: texMtx = SRT * effect * view * model. The effect matrix (baked into the
// model, or built from the view's fovy/aspect) projects like the game camera, Pg. For an eye it
// must project like the eye, Pe: effect' = effect * Pg^-1 * Pe (both acting on view-space xyz).
Mtx44 g_savedEffect;
J3DTexMtx* g_effectPatched = nullptr;

HookAction calc_tex_mtx_pre(ModContext*, void* args, void*, void*) {
    J3DTexMtx* texMtx = mods::arg<J3DTexMtx*>(args, 0);
    const u32 mode = texMtx->getTexMtxInfo().mInfo & 0x3f;
    if (f.eye < 0 || f.eyeFovy == 0.0f || (mode != J3DTexMtxMode_ViewProjmapBasic && mode != J3DTexMtxMode_ViewProjmap)) {
        return HOOK_CONTINUE;
    }
    // Pg from the game's fovy/aspect (what C_MTXLightPerspective and the game projection use).
    const float cot = 1.0f / std::tan(f.eyeFovy * 0.5f * kPi / 180.0f);
    const float ga = cot / f.eyeAspect, gc = cot;
    const auto& p = f.eyeProj.m;
    // C = Pg^-1 * Pe for Pg = [[ga,0,0],[0,gc,0],[0,0,-1]], Pe = [[p00,0,p02],[0,p11,p12],[0,0,-1]].
    const float c[3][3] = {{p[0][0] / ga, 0.0f, p[0][2] / ga}, {0.0f, p[1][1] / gc, p[1][2] / gc}, {0.0f, 0.0f, 1.0f}};
    auto& eff = texMtx->getTexMtxInfo().mEffectMtx;
    std::memcpy(g_savedEffect, eff, sizeof(Mtx44));
    for (int r = 0; r < 4; ++r) {
        for (int j = 0; j < 3; ++j) {
            eff[r][j] = g_savedEffect[r][0] * c[0][j] + g_savedEffect[r][1] * c[1][j] + g_savedEffect[r][2] * c[2][j];
        }
    }
    g_effectPatched = texMtx;
    return HOOK_CONTINUE;
}

void calc_tex_mtx_post(ModContext*, void* args, void*, void*) {
    J3DTexMtx* texMtx = mods::arg<J3DTexMtx*>(args, 0);
    if (g_effectPatched == texMtx) {
        std::memcpy(texMtx->getTexMtxInfo().mEffectMtx, g_savedEffect, sizeof(Mtx44));
        g_effectPatched = nullptr;
        static bool logged = false;
        if (!logged) {
            logged = true;
            mods::log::info("Corrected a view-projection texture matrix for the eye frustum");
        }
    }
}

HookAction calc_post_tex_mtx_pre(ModContext* ctx, void* args, void* retval, void* user) {
    const HookAction action = calc_tex_mtx_pre(ctx, args, retval, user);
    static bool logged = false;
    if (!logged && g_effectPatched != nullptr) {
        logged = true;
        mods::log::info("Corrected a post-transform view-projection texture matrix for the eye frustum");
    }
    return action;
}

HookAction end_frame_pre(ModContext*, void*, void*, void*) {
    if (!f.active) {
        return HOOK_CONTINUE;
    }
    const bool simulating = f.xrId == 0 && f.scene[0] != nullptr && f.scene[1] != nullptr &&
                            xr::ensure_simulation_targets(1024, 1024);
    Packet p;
    p.xrId = f.xrId;
    p.stereo = f.scene[0] != nullptr && f.scene[1] != nullptr && (f.xrId != 0 || simulating);
    p.tabletop = f.tabletop;
    p.seeThrough = f.tabletop && config().tablePassthrough;
    if (f.tabletop) {
        // Runtimes without passthrough / alpha blending show the transparent area opaque: black, or a
        // chroma-key colour for passthrough tools that key it out.
        const bool seeThrough = xr::see_through_available() && config().tablePassthrough;
        p.eyeBlit = seeThrough ? gpu::BlitMode::KeepAlpha : gpu::BlitMode::Key;
        p.keyColor = config().tableKeyColor;
    }
    p.scene = f.scene;
    p.hud = f.hud;
    p.mono = f.mono;
    p.quads[xr::kQuadHud] = f.quad;
    p.targetFormat = xr::target_format();
    p.eyeTargets = {xr::eye_target_view(0), xr::eye_target_view(1)};
    if (p.quads[xr::kQuadHud].enabled && f.xrId != 0) {
        uint32_t w, h;
        efb_size(w, h);
        p.quadTargets[xr::kQuadHud] = xr::ensure_quad_target(xr::kQuadHud, w, h);
        p.quadTokens[xr::kQuadHud] = xr::quad_token(xr::kQuadHud);
        if (p.quadTargets[xr::kQuadHud] == nullptr) {
            p.quads[xr::kQuadHud].enabled = false;
        }
    } else {
        p.quads[xr::kQuadHud].enabled = false;
    }
    if (p.xrId == 0 && !simulating) {
        g_place.uiWasOpen = false;
        return HOOK_CONTINUE;
    }
    if (f.xrId != 0) {
        prepare_ui_quad(p);
    }

    PacketRef ref{};
    {
        std::lock_guard lock{g_packetMutex};
        p.seq = ++g_packetSeq;
        ref.slot = static_cast<uint32_t>(p.seq % kPackets);
        ref.seq = p.seq;
        if (g_packets[ref.slot].ui != nullptr) {
            wgpuTextureViewRelease(g_packets[ref.slot].ui); // that packet was never picked up
        }
        g_packets[ref.slot] = p;
    }
    if (const ModResult r = svc_gfx->push_compute(mod_ctx, g_finishCompute, &ref, sizeof(ref)); r != MOD_OK) {
        mods::log::error("push_compute(finish) failed ({})", static_cast<int>(r));
    }
    return HOOK_CONTINUE;
}

bool g_rmlForcedOverlay = false; // this frame's RmlUi base layer was switched to transparent

void rml_record_post(ModContext*, void*, void* retval, void*) {
    auto* frame = static_cast<RmlRecordedFrame*>(retval);
    g_rmlDrew = frame != nullptr && frame->bindGroup != nullptr;
    if (frame != nullptr && g_rmlForcedOverlay) {
        // The target now holds only the UI, so the desktop must composite it over the scene too.
        frame->overlay = true;
    }
    g_rmlForcedOverlay = false;
    static bool logged = false;
    if (g_rmlDrew && !logged && g_rmlTarget != nullptr) {
        logged = true;
        mods::log::info("Dusklight UI target {}x{} format {} (view {})", g_rmlTarget->size.width,
            g_rmlTarget->size.height, static_cast<int>(g_rmlTarget->format), static_cast<void*>(g_rmlTarget->view));
    }
}

HookAction rml_begin_frame_pre(ModContext*, void* args, void*, void*) {
    // In the headset only the UI itself belongs on its panel. Menus with a backdrop blur make RmlUi
    // draw the whole scene underneath; render them as a transparent overlay instead (the blur then
    // has nothing behind it, the panels keep their tint).
    // The target is aurora::rmlui::s_renderTarget, which has internal linkage (no symbol to resolve
    // on Android); its address arrives here by reference.
    g_rmlTarget = static_cast<const RmlRenderTarget*>(mods::arg<const void*>(args, 1));
    int& baseLayer = mods::arg_ref<int>(args, 3);
    if (baseLayer != 0 && config().showDuskUi && config().mode != Mode::Off &&
        (xr::session_running() || config().simulateHmd)) {
        baseLayer = 0; // BaseLayerContent::Transparent
        g_rmlForcedOverlay = true;
    }
    return HOOK_CONTINUE;
}

// Puts Dusklight's UI (settings, mod manager, ...) on its own quad, locked in front of the head when
// it opens. The texture is last frame's (RmlUi renders after this frame's composition is queued).
void prepare_ui_quad(Packet& p) {
    const bool show = config().showDuskUi && g_rmlDrew && g_rmlTarget != nullptr && g_rmlTarget->view != nullptr &&
                      g_rmlTarget->size.width != 0 && g_rmlTarget->size.height != 0;
    if (!show) {
        g_place.uiWasOpen = false;
        return;
    }
    if (!f.info.viewsValid && !xr::locate_views(f.xrId, f.info)) {
        return;
    }
    const uint32_t w = g_rmlTarget->size.width;
    const uint32_t h = g_rmlTarget->size.height;
    WGPUTextureView target = xr::ensure_quad_target(xr::kQuadUi, w, h);
    if (target == nullptr) {
        return;
    }
    const auto& cfg = config();
    if (!g_place.uiWasOpen) {
        Pose head;
        head.position = (f.info.views[0].pose.position + f.info.views[1].pose.position) * 0.5f;
        g_place.uiPose.orientation = quat_from_yaw(yaw_of(f.info.views[0].pose.orientation));
        // A little in front of where game menus go, so it sits above them.
        g_place.uiPose.position = head.position +
                                  rotate(g_place.uiPose.orientation, Vec3{0.0f, -0.05f, -(cfg.menuDistance - 0.1f)});
        g_place.uiWasOpen = true;
    }
    xr::QuadLayer& q = p.quads[xr::kQuadUi];
    q.enabled = true;
    q.space = xr::QuadSpace::App;
    q.pose = g_place.uiPose;
    q.width = cfg.menuWidth;
    q.height = cfg.menuWidth * static_cast<float>(h) / static_cast<float>(w);
    q.premultipliedAlpha = true;
    p.quadTargets[xr::kQuadUi] = target;
    p.quadTokens[xr::kQuadUi] = xr::quad_token(xr::kQuadUi);
    // RmlUi may recreate the target (window resize) before the worker samples it; keep this view alive.
    wgpuTextureViewAddRef(g_rmlTarget->view);
    p.ui = g_rmlTarget->view;
}

template <class Entry>
bool check(ModResult r, const char* what, bool required = true) {
    if (r == MOD_OK) {
        return true;
    }
    if (required) {
        mods::log::error("failed to hook {} ({})", what, static_cast<int>(r));
    } else {
        mods::log::warn("optional hook {} unavailable ({})", what, static_cast<int>(r));
    }
    return !required;
}

} // namespace

// Bring-up switch (Android): installing every detour at once crashed the first frame on a Quest, so
// hookLevel narrows the set to bisect the culprit on device.
//   0 nothing   1 Aurora frame hooks   2 + frame delivery   3 + Dusklight UI   4 (default) everything
bool install() {
    bool ok = true;
    const int level = config().hookLevel;
    if (level < 4) {
        mods::log::warn("hookLevel {}: installing a reduced hook set (bring-up switch)", level);
    }
    if (level < 1) {
        return true;
    }
    ok &= check<AuroraBeginFrame>(mods::hook::add_pre<AuroraBeginFrame>(begin_frame_pre), "aurora_begin_frame");
    ok &= check<AuroraBeginFrame>(mods::hook::add_post<AuroraBeginFrame>(begin_frame_post), "aurora_begin_frame");
    ok &= check<AuroraEndFrame>(mods::hook::add_pre<AuroraEndFrame>(end_frame_pre), "aurora_end_frame");
    if (level >= 4) {
        ok &= check<Painter>(mods::hook::replace<Painter>(painter_replace), "mDoGph_Painter");
        ok &= check<RunStage>(mods::hook::add_pre<RunStage>(run_stage_pre), "gfx_run_stage");
        ok &= check<RunStage>(mods::hook::add_post<RunStage>(run_stage_post), "gfx_run_stage");
        ok &= check<OriginalFrames>(mods::hook::add_pre<OriginalFrames>(original_frames_pre), "original_frames");
        check<ImguiPreDraw>(mods::hook::add_pre<ImguiPreDraw>(skip_on_second_eye), "ImGuiConsole::PreDraw", false);
        check<ImguiPostDraw>(mods::hook::add_pre<ImguiPostDraw>(skip_on_second_eye), "ImGuiConsole::PostDraw", false);
        check<MotionBlur>(mods::hook::add_pre<MotionBlur>(motion_blur_pre), "motionBlure", false);
        check<Trimming>(mods::hook::add_pre<Trimming>(trimming_pre), "trimming", false);
        check<ClipperSetup>(mods::hook::add_pre<ClipperSetup>(clipper_setup_pre), "mDoLib_clipper::setup", false);
        check<GetWindowSize>(mods::hook::add_post<GetWindowSize>(window_size_post), "get_window_size", false);
        check<ClipSphere>(mods::hook::add_pre<ClipSphere>(clip_pre), "J3DUClipper::clip (sphere)", false);
        check<ClipBox>(mods::hook::add_pre<ClipBox>(clip_pre), "J3DUClipper::clip (box)", false);
        check<DrawOpaSky>(mods::hook::add_pre<DrawOpaSky>(sky_pre), "dComIfGd_drawOpaListSky", false);
        check<DrawXluSky>(mods::hook::add_pre<DrawXluSky>(sky_pre), "dComIfGd_drawXluListSky", false);
        const ModResult fog = mods::hook::add_pre<SetFog>(fog_pre);
        check<SetFog>(fog, "GXSetFog", false);
        if (fog == MOD_OK) {
            xray::install(); // needs the fog hook to select its shaders
        }
        check<LightPerspective>(
            mods::hook::add_post<LightPerspective>(light_perspective_post), "C_MTXLightPerspective", false);
    }
    if (!ok) {
        return false;
    }

    if (level >= 2) {
        GfxComputeTypeDesc begin = GFX_COMPUTE_TYPE_DESC_INIT;
        begin.label = "Dusklight VR frame begin";
        begin.callback = begin_compute;
        GfxComputeTypeDesc finish = GFX_COMPUTE_TYPE_DESC_INIT;
        finish.label = "Dusklight VR composite";
        finish.callback = finish_compute;
        if (svc_gfx->register_compute_type(mod_ctx, &begin, &g_beginCompute) != MOD_OK ||
            svc_gfx->register_compute_type(mod_ctx, &finish, &g_finishCompute) != MOD_OK)
        {
            mods::log::error("failed to register VR compute callbacks");
            return false;
        }

        // Frame delivery: Aurora's own post-submit step (works on every platform); the import-table
        // hook on wgpuQueueSubmit is only a Windows fallback.
        if (mods::hook::add_post<AfterSubmit>(after_submit_post) == MOD_OK) {
            mods::log::info("Frame delivery: aurora::gfx::after_submit");
        } else if (mods::hook::add_post<DepthPeekAfterSubmit>(after_submit_post) == MOD_OK) {
            mods::log::info("Frame delivery: aurora::gfx::depth_peek::after_submit");
        } else {
#ifdef _WIN32
            void* orig = nullptr;
            if (!patch_import(GetModuleHandleW(nullptr), "webgpu_dawn.dll", "wgpuQueueSubmit",
                    reinterpret_cast<void*>(&hk_queue_submit), &orig))
            {
                mods::log::error("could not observe frame submission; VR frames cannot be delivered");
                return false;
            }
            g_origQueueSubmit = reinterpret_cast<QueueSubmitFn>(orig);
            mods::log::info("Frame delivery: wgpuQueueSubmit import hook");
#else
            mods::log::error("could not observe frame submission; VR frames cannot be delivered");
            return false;
#endif
        }
    }

    if (level >= 3) {
        if (mods::hook::add_pre<RmlBeginFrame>(rml_begin_frame_pre) != MOD_OK ||
            mods::hook::add_post<RmlRecordFrame>(rml_record_post) != MOD_OK)
        {
            mods::log::warn("Dusklight UI unavailable in the headset");
        }
    }

    if (level >= 4) {
        check<BgMaterialProc>(mods::hook::add_pre<BgMaterialProc>(bg_material_pre), "dKy_bg_MAxx_proc", false);
        check<CalcTexMtx>(mods::hook::add_pre<CalcTexMtx>(calc_tex_mtx_pre), "J3DTexMtx::calcTexMtx", false);
        check<CalcTexMtx>(mods::hook::add_post<CalcTexMtx>(calc_tex_mtx_post), "J3DTexMtx::calcTexMtx", false);
        check<CalcPostTexMtx>(
            mods::hook::add_pre<CalcPostTexMtx>(calc_post_tex_mtx_pre), "J3DTexMtx::calcPostTexMtx", false);
        check<CalcPostTexMtx>(
            mods::hook::add_post<CalcPostTexMtx>(calc_tex_mtx_post), "J3DTexMtx::calcPostTexMtx", false);
        // Without the widezoom guard the camera's callback would rewrite the eye view, so only use
        // the per-eye material refresh when both are available.
        if (mods::hook::add_pre<WidezoomCorrection>(widezoom_pre) == MOD_OK) {
            void* callbacksFn = nullptr;
            if (svc_hook->resolve(mod_ctx, "src/dusk/interp/frame_interpolation.cpp#callbacks_run", &callbacksFn,
                    nullptr) == MOD_OK)
            {
                g_callbacksRun = reinterpret_cast<CallbacksRunFn>(callbacksFn);
            } else if (interp_mirror::install()) {
                g_callbacksRun = interp_mirror::run;
                mods::log::info("Per-eye material refresh: mirrored interpolation callbacks");
            }
        }
        if (g_callbacksRun == nullptr) {
            mods::log::warn("per-eye material refresh unavailable; water may look wrong in stereo");
        }
    }

    void* refreshFn = nullptr;
    if (svc_hook->resolve(mod_ctx, "aurora::webgpu::refresh_surface", &refreshFn, nullptr) == MOD_OK) {
        g_refreshSurface = reinterpret_cast<RefreshSurfaceFn>(refreshFn);
    } else {
        mods::log::warn("could not resolve refresh_surface; headset render resolution follows the window");
    }

    void* overrideFn = nullptr;
    if (svc_hook->resolve(mod_ctx, "dusk::config::load_arg_override", &overrideFn, nullptr) == MOD_OK) {
        g_configOverride = reinterpret_cast<ConfigOverrideFn>(overrideFn);
    } else {
        mods::log::warn("could not resolve config overrides; enable Frame Interpolation (Unlimited) manually");
    }
    return true;
}

void uninstall() {
    xray::uninstall();
    if (g_efbWidth.load() != 0) {
        g_efbWidth.store(0);
        g_efbHeight.store(0);
        if (g_refreshSurface != nullptr) {
            g_refreshSurface(false);
        }
    }
#ifdef _WIN32
    if (g_origQueueSubmit != nullptr) {
        restore_import(GetModuleHandleW(nullptr), "webgpu_dawn.dll", "wgpuQueueSubmit",
            reinterpret_cast<void*>(g_origQueueSubmit));
        g_origQueueSubmit = nullptr;
    }
#endif
    if (g_beginCompute != 0) {
        svc_gfx->unregister_compute_type(mod_ctx, g_beginCompute);
        g_beginCompute = 0;
    }
    if (g_finishCompute != 0) {
        svc_gfx->unregister_compute_type(mod_ctx, g_finishCompute);
        g_finishCompute = 0;
    }
}

std::string status() {
    uint32_t w = 0, h = 0;
    AuroraGetRenderSize(&w, &h);
    char buf[256];
    std::snprintf(buf, sizeof(buf),
        "%.0f fps (%.1f ms) | xrWaitFrame %.1f ms | both eyes %.1f ms | render %ux%u | stereo %llu, mono %llu",
        g_timing.frameMs > 0 ? 1000.0 / g_timing.frameMs : 0.0, g_timing.frameMs, g_timing.waitMs,
        g_timing.painterMs, w, h, static_cast<unsigned long long>(g_stereoFrames),
        static_cast<unsigned long long>(g_monoFrames));
    return buf;
}

} // namespace vr::render
