#include "d/d_com_inf_game.h"
#include "d/d_menu_window.h"
#include "d/d_meter2_info.h"
#include "m_Do/m_Do_controller_pad.h"

#include "item_wheel.hpp"

#include "vr_config.hpp"
#include "xr_runtime.hpp"

#include "mods/svc/hook.hpp"
#include "mods/svc/log.hpp"

#include <cstring>

// Opening the item wheel normally pauses the game: the menu window snapshots the frame (which sets
// the pause flag) and shows the snapshot behind the wheel. In tabletop the wheel can instead run
// "quick": no snapshot, so the world keeps going around Link, and only Link and the camera stop
// hearing the controller (the wheel itself still reads it).
DEFINE_HOOK_SYMBOL("dDlst_MENU_CAPTURE_c::draw", void(void*), MenuCaptureDraw);
DEFINE_HOOK_SYMBOL("dMw_c::_draw", int(void*), MenuWindowDraw);
DEFINE_HOOK_SYMBOL("src/d/actor/d_a_alink.cpp#daAlink_Execute", int(void*), LinkExecute);
DEFINE_HOOK_SYMBOL("src/d/d_camera.cpp#camera_execute", int(void*), CameraExecute);

namespace vr::item_wheel {
namespace {

bool wheel_menu_active() {
    const dMw_c* menu = dMeter2Info_getMenuWindowClass();
    return menu != nullptr && menu->mpMenuRing != nullptr;
}

bool quick_wheel() {
    const auto& cfg = config();
    return cfg.mode == Mode::Tabletop && !cfg.tableWheelPause && (xr::session_running() || cfg.simulateHmd) &&
           wheel_menu_active();
}

HookAction capture_pre(ModContext*, void*, void*, void*) {
    return quick_wheel() ? HOOK_SKIP_ORIGINAL : HOOK_CONTINUE;
}

// The menu window only queues the wheel for drawing while the game is paused, so a quick wheel
// raises the pause flag for just that call.
bool g_pauseRaised = false;

HookAction menu_draw_pre(ModContext*, void*, void*, void*) {
    g_pauseRaised = quick_wheel() && !dComIfGp_isPauseFlag();
    if (g_pauseRaised) {
        dComIfGp_onPauseFlag();
    }
    return HOOK_CONTINUE;
}

void menu_draw_post(ModContext*, void*, void*, void*) {
    if (g_pauseRaised) {
        dComIfGp_offPauseFlag();
        g_pauseRaised = false;
    }
}

// Controller state hidden from Link and the camera while a quick wheel is open (port 1 only; the
// wheel is driven from it).
interface_of_controller_pad g_saved{};
int g_muted = 0;

HookAction mute_pre(ModContext*, void*, void*, void*) {
    if (g_muted == 0 && quick_wheel()) {
        auto& pad = mDoCPd_c::m_cpadInfo[PAD_1];
        g_saved = pad;
        pad.mMainStickPosX = pad.mMainStickPosY = pad.mMainStickValue = 0.0f;
        pad.mMainStickAngle = 0;
        pad.mCStickPosX = pad.mCStickPosY = pad.mCStickValue = 0.0f;
        pad.mCStickAngle = 0;
        pad.mAnalogA = pad.mAnalogB = pad.mTriggerLeft = pad.mTriggerRight = 0.0f;
        pad.mButtonFlags = pad.mPressedButtonFlags = 0;
        g_muted = 1;
    } else if (g_muted != 0) {
        ++g_muted; // nested (shouldn't happen): keep the outer save
    }
    return HOOK_CONTINUE;
}

void mute_post(ModContext*, void*, void*, void*) {
    if (g_muted == 1) {
        mDoCPd_c::m_cpadInfo[PAD_1] = g_saved;
    }
    if (g_muted > 0) {
        --g_muted;
    }
}

} // namespace

void install() {
    bool ok = mods::hook::add_pre<MenuCaptureDraw>(capture_pre) == MOD_OK;
    ok &= mods::hook::add_pre<MenuWindowDraw>(menu_draw_pre) == MOD_OK;
    ok &= mods::hook::add_post<MenuWindowDraw>(menu_draw_post) == MOD_OK;
    ok &= mods::hook::add_pre<LinkExecute>(mute_pre) == MOD_OK;
    ok &= mods::hook::add_post<LinkExecute>(mute_post) == MOD_OK;
    ok &= mods::hook::add_pre<CameraExecute>(mute_pre) == MOD_OK;
    ok &= mods::hook::add_post<CameraExecute>(mute_post) == MOD_OK;
    if (!ok) {
        mods::log::warn("Quick item wheel unavailable (the wheel pauses the game as usual)");
    }
}

} // namespace vr::item_wheel
