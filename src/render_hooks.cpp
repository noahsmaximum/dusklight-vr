// Game headers first: windows.h (pulled in below) defines macros such as IN that collide with game enums.
#include "JSystem/J3DGraphBase/J3DSys.h"
#include "d/d_com_inf_game.h"
#include "f_op/f_op_camera_mng.h"
#include "f_op/f_op_view.h"

#include "render_hooks.hpp"

#include "gpu.hpp"
#include "iat_hook.hpp"
#include "vr_config.hpp"
#include "vr_math.hpp"
#include "xr_runtime.hpp"

#include "mods/svc/gfx.h"
#include "mods/svc/hook.hpp"
#include "mods/svc/log.hpp"

#include <dolphin/gx/GXAurora.h>

#include <array>
#include <atomic>
#include <cstring>
#include <mutex>
#include <string_view>

// --- Hook targets ------------------------------------------------------------------------------------

DEFINE_HOOK_SYMBOL("aurora_begin_frame", bool(), AuroraBeginFrame);
DEFINE_HOOK_SYMBOL("aurora_end_frame", void(), AuroraEndFrame);
DEFINE_HOOK_SYMBOL("mDoGph_Painter", int(), Painter);
DEFINE_HOOK_SYMBOL("dusk::mods::gfx_run_stage", void(int, const void*, const void*), RunStage);
DEFINE_HOOK_SYMBOL("dusk::game_clock::original_frames", float(), OriginalFrames);
DEFINE_HOOK_SYMBOL("dusk::ImGuiConsole::PreDraw", void(void*), ImguiPreDraw);
DEFINE_HOOK_SYMBOL("dusk::ImGuiConsole::PostDraw", void(void*), ImguiPostDraw);
DEFINE_HOOK_SYMBOL("src/m_Do/m_Do_graphic.cpp#motionBlure", void(view_class*), MotionBlur);
DEFINE_HOOK_SYMBOL("src/m_Do/m_Do_graphic.cpp#drawDepth2", void(view_class*, view_port_class*, int), DrawDepth);
DEFINE_HOOK_SYMBOL("src/m_Do/m_Do_graphic.cpp#trimming", void(view_class*, view_port_class*), Trimming);
DEFINE_HOOK_SYMBOL("mDoLib_clipper::setup", void(f32, f32, f32, f32), ClipperSetup);

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
    bool hudOpen = false;  // our offscreen pass for the 2D layer is recording
    bool hasCamera = false;
    std::array<WGPUTextureView, 2> scene{};
    std::array<WGPUTextureView, 2> hud{}; // [0] over black, [1] over white
    WGPUTextureView mono = nullptr;
    xr::QuadLayer quad;
};
Frame f;

// Handed to the render worker through a compute payload (the payload itself carries only the slot).
struct Packet {
    uint64_t seq = 0;
    uint64_t xrId = 0;
    bool stereo = false;
    std::array<WGPUTextureView, 2> scene{};
    std::array<WGPUTextureView, 2> hud{};
    WGPUTextureView mono = nullptr;
    std::array<WGPUTextureView, 2> eyeTargets{};
    WGPUTextureView quadTarget = nullptr;
    void* quadToken = nullptr;
    WGPUTextureFormat targetFormat = WGPUTextureFormat_Undefined;
    xr::QuadLayer quad;
};
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

std::atomic<DWORD> g_workerThread{0};
std::atomic<bool> g_simulationArmed{false};
using QueueSubmitFn = void (*)(WGPUQueue, size_t, const WGPUCommandBuffer*);
QueueSubmitFn g_origQueueSubmit = nullptr;

ConfigOverrideFn g_configOverride = nullptr;
bool g_overridesApplied = false;

// Placement state for the head-following HUD and world-locked menus.
struct Placement {
    bool init = false;
    float hudYaw = 0.0f;
    Vec3 hudAnchor;
    bool hudTurning = false;
    bool menuWasOpen = false;
    Pose menuPose;
};
Placement g_place;

uint64_t g_stereoFrames = 0;
uint64_t g_monoFrames = 0;

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

// Rewrites the camera's view_class for one eye: the game camera becomes the tracking-space origin,
// the eye pose (metres -> game units) is applied on top, and the projection becomes the headset's
// asymmetric frustum while keeping the game's depth mapping.
void apply_eye(view_class& v, const ViewBackup& base, const Mtx34& gameView, int eye) {
    const auto& cfg = config();
    const Pose pose = eye_pose(f.info, eye);
    const Mtx34 eyeFromTracking = inverse_rigid(pose_to_mtx(pose, cfg.unitsPerMeter));
    const Mtx34 view = mul(eyeFromTracking, gameView);

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

    // Symmetric bounds of the frustum for systems that only understand fovy/aspect (particles).
    const float halfV = std::max(fov.tanUp, -fov.tanDown);
    const float halfH = std::max(fov.tanRight, -fov.tanLeft);
    v.fovy = 2.0f * std::atan(halfV) * 180.0f / kPi;
    v.aspect = halfH / halfV;

    const Vec3 eyePos{inv.m[0][3], inv.m[1][3], inv.m[2][3]};
    const Vec3 fwd{-inv.m[0][2], -inv.m[1][2], -inv.m[2][2]};
    v.lookat.eye.set(eyePos.x, eyePos.y, eyePos.z);
    const Vec3 center = eyePos + fwd * 100.0f;
    v.lookat.center.set(center.x, center.y, center.z);
    v.lookat.up.set(inv.m[0][1], inv.m[1][1], inv.m[2][1]);
    v.bank = 0;
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
    }
    if (p.seq != ref.seq) {
        return;
    }
    if (p.stereo) {
        for (int eye = 0; eye < 2; ++eye) {
            gpu::blit(ctx->encoder, p.scene[eye], p.eyeTargets[eye], p.targetFormat);
        }
        static bool logged = false;
        if (!logged) {
            logged = true;
            mods::log::info("First eye composition encoded on the render worker");
        }
    }
    if (p.quad.enabled && p.quadTarget != nullptr) {
        if (p.hud[0] != nullptr && p.hud[1] != nullptr) {
            gpu::combine_hud(ctx->encoder, p.hud[0], p.hud[1], p.quadTarget, p.targetFormat);
        } else if (p.mono != nullptr) {
            gpu::blit(ctx->encoder, p.mono, p.quadTarget, p.targetFormat);
        }
    }
    g_workerThread.store(GetCurrentThreadId(), std::memory_order_release);
    if (p.xrId != 0) {
        xr::arm_submit(p.xrId, p.stereo, p.quad, p.quadToken);
    } else if (p.stereo) {
        g_simulationArmed.store(true, std::memory_order_release);
    }
}

void hk_queue_submit(WGPUQueue queue, size_t count, const WGPUCommandBuffer* buffers) {
    g_origQueueSubmit(queue, count, buffers);
    if (GetCurrentThreadId() == g_workerThread.load(std::memory_order_acquire)) {
        xr::on_queue_submitted();
        if (g_simulationArmed.exchange(false)) {
            xr::simulation_readback_once();
        }
    }
}

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
    mods::log::info("Applied VR overrides: frame interpolation unlimited, vsync off, no letterbox/mirror");
}

HookAction begin_frame_pre(ModContext*, void*, void*, void*) {
    refresh_config();
    f = {};
    const auto& cfg = config();
    f.mode = cfg.mode;
    if (f.mode == Mode::Off) {
        return HOOK_CONTINUE;
    }
    xr::poll();
    if (xr::session_running()) {
        apply_game_overrides();
        f.xrId = xr::wait_frame();
    }
    f.active = f.xrId != 0 || (cfg.simulateHmd && f.mode == Mode::Stereo);
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

WGPUTextureView resolve_color() {
    GfxResolveDesc desc = GFX_RESOLVE_DESC_INIT;
    desc.color = true;
    GfxResolvedTargets out = GFX_RESOLVED_TARGETS_INIT;
    if (svc_gfx->resolve_pass(mod_ctx, &desc, &out) != MOD_OK) {
        return nullptr;
    }
    return out.color;
}

void close_hud_pass() {
    if (!f.hudOpen) {
        return;
    }
    f.hudOpen = false;
    const WGPUTextureView view = resolve_color();
    if (f.eye == 0 || f.eye == 1) {
        f.hud[f.eye] = view;
    }
}

void painter_replace(ModContext*, void*, void* retval, void*) {
    int result = 1;
    auto run = [&] { result = Painter::g_orig(); };

    if (!f.active) {
        run();
        if (retval) *static_cast<int*>(retval) = result;
        return;
    }

    bool stereo = false;
    if (f.mode == Mode::Stereo) {
        if (f.xrId != 0) {
            stereo = xr::locate_views(f.xrId, f.info);
        } else if (config().simulateHmd) {
            xr::simulated_views(f.info);
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
    Mtx34 gameView;
    if (cam != nullptr) {
        backup_view(cam->view, backup);
        gameView = load(backup.viewMtx);
        if (config().levelHorizon) {
            gameView = level_view(gameView);
        }
    }

    for (int eye = 0; eye < 2; ++eye) {
        f.eye = eye;
        if (eye == 1) {
            // The painter never clears depth itself (the XFB copy does on hardware), so the second
            // eye would otherwise depth-test against the first eye's scene.
            gpu::push_clear();
        }
        if (cam != nullptr) {
            apply_eye(cam->view, backup, gameView, eye);
        }
        run();
        close_hud_pass(); // in case the painter skipped the HUD stage
        f.scene[eye] = resolve_color();
    }
    f.eye = -1;
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
        close_hud_pass();
    }
    return HOOK_CONTINUE;
}

void run_stage_post(ModContext*, void* args, void*, void*) {
    if (mods::arg<int>(args, 0) != GFX_STAGE_FRAME_BEFORE_HUD || f.eye < 0 || f.hudOpen) {
        return;
    }
    // Everything the painter draws from here to FRAME_AFTER_HUD is 2D (HUD, text, menus). Record it
    // into its own layer: over black for the first eye and over white for the second, which lets
    // the compositor recover exact alpha for the headset's quad layer.
    uint32_t w, h;
    efb_size(w, h);
    if (svc_gfx->create_pass(mod_ctx, w, h) != MOD_OK) {
        return;
    }
    f.hudOpen = true;
    if (f.eye == 1) {
        gpu::push_fill_white();
    }
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

HookAction depth_of_field_pre(ModContext*, void*, void*, void*) {
    return f.eye >= 0 && config().disableDof ? HOOK_SKIP_ORIGINAL : HOOK_CONTINUE;
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

HookAction end_frame_pre(ModContext*, void*, void*, void*) {
    if (!f.active) {
        return HOOK_CONTINUE;
    }
    const bool simulating = f.xrId == 0 && f.scene[0] != nullptr && f.scene[1] != nullptr &&
                            xr::ensure_simulation_targets(1024, 1024);
    Packet p;
    p.xrId = f.xrId;
    p.stereo = f.scene[0] != nullptr && f.scene[1] != nullptr && (f.xrId != 0 || simulating);
    p.scene = f.scene;
    p.hud = f.hud;
    p.mono = f.mono;
    p.quad = f.quad;
    p.targetFormat = xr::target_format();
    p.eyeTargets = {xr::eye_target_view(0), xr::eye_target_view(1)};
    if (p.quad.enabled && f.xrId != 0) {
        uint32_t w, h;
        efb_size(w, h);
        p.quadTarget = xr::ensure_quad_target(w, h);
        p.quadToken = xr::quad_token();
        if (p.quadTarget == nullptr) {
            p.quad.enabled = false;
        }
    } else {
        p.quad.enabled = false;
    }
    if (p.xrId == 0 && !simulating) {
        return HOOK_CONTINUE;
    }

    PacketRef ref{};
    {
        std::lock_guard lock{g_packetMutex};
        p.seq = ++g_packetSeq;
        ref.slot = static_cast<uint32_t>(p.seq % kPackets);
        ref.seq = p.seq;
        g_packets[ref.slot] = p;
    }
    if (const ModResult r = svc_gfx->push_compute(mod_ctx, g_finishCompute, &ref, sizeof(ref)); r != MOD_OK) {
        mods::log::error("push_compute(finish) failed ({})", static_cast<int>(r));
    }
    return HOOK_CONTINUE;
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

bool install() {
    bool ok = true;
    ok &= check<AuroraBeginFrame>(mods::hook::add_pre<AuroraBeginFrame>(begin_frame_pre), "aurora_begin_frame");
    ok &= check<AuroraBeginFrame>(mods::hook::add_post<AuroraBeginFrame>(begin_frame_post), "aurora_begin_frame");
    ok &= check<AuroraEndFrame>(mods::hook::add_pre<AuroraEndFrame>(end_frame_pre), "aurora_end_frame");
    ok &= check<Painter>(mods::hook::replace<Painter>(painter_replace), "mDoGph_Painter");
    ok &= check<RunStage>(mods::hook::add_pre<RunStage>(run_stage_pre), "gfx_run_stage");
    ok &= check<RunStage>(mods::hook::add_post<RunStage>(run_stage_post), "gfx_run_stage");
    ok &= check<OriginalFrames>(mods::hook::add_pre<OriginalFrames>(original_frames_pre), "original_frames");
    check<ImguiPreDraw>(mods::hook::add_pre<ImguiPreDraw>(skip_on_second_eye), "ImGuiConsole::PreDraw", false);
    check<ImguiPostDraw>(mods::hook::add_pre<ImguiPostDraw>(skip_on_second_eye), "ImGuiConsole::PostDraw", false);
    check<MotionBlur>(mods::hook::add_pre<MotionBlur>(motion_blur_pre), "motionBlure", false);
    check<DrawDepth>(mods::hook::add_pre<DrawDepth>(depth_of_field_pre), "drawDepth2", false);
    check<Trimming>(mods::hook::add_pre<Trimming>(trimming_pre), "trimming", false);
    check<ClipperSetup>(mods::hook::add_pre<ClipperSetup>(clipper_setup_pre), "mDoLib_clipper::setup", false);
    if (!ok) {
        return false;
    }

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

    void* orig = nullptr;
    if (!patch_import(GetModuleHandleW(nullptr), "webgpu_dawn.dll", "wgpuQueueSubmit",
            reinterpret_cast<void*>(&hk_queue_submit), &orig))
    {
        mods::log::error("could not observe wgpuQueueSubmit; VR frames cannot be delivered");
        return false;
    }
    g_origQueueSubmit = reinterpret_cast<QueueSubmitFn>(orig);

    void* overrideFn = nullptr;
    if (svc_hook->resolve(mod_ctx, "dusk::config::load_arg_override", &overrideFn, nullptr) == MOD_OK) {
        g_configOverride = reinterpret_cast<ConfigOverrideFn>(overrideFn);
    } else {
        mods::log::warn("could not resolve config overrides; enable Frame Interpolation (Unlimited) manually");
    }
    return true;
}

void uninstall() {
    if (g_origQueueSubmit != nullptr) {
        restore_import(GetModuleHandleW(nullptr), "webgpu_dawn.dll", "wgpuQueueSubmit",
            reinterpret_cast<void*>(g_origQueueSubmit));
        g_origQueueSubmit = nullptr;
    }
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
    return "stereo frames: " + std::to_string(g_stereoFrames) + ", mono frames: " + std::to_string(g_monoFrames);
}

} // namespace vr::render
