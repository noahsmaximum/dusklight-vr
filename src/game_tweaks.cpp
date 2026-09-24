#include "JSystem/JUtility/JUTFader.h"
#include "d/actor/d_a_alink.h"
#include "d/d_camera.h"
#include "d/d_com_inf_game.h"

#include "game_tweaks.hpp"

#include "vr_config.hpp"
#include "xr_runtime.hpp"

#include "mods/svc/hook.hpp"
#include "mods/svc/log.hpp"

// Small changes to the game's own behaviour that make it play better in a headset.
DEFINE_HOOK_SYMBOL("JUTFader::draw", void(JUTFader*), FaderDraw);
DEFINE_HOOK_SYMBOL("darwFilter", void(GXColor), ColorFilter);
DEFINE_HOOK_SYMBOL("dCamera_c::freeCamera", bool(dCamera_c*), FreeCamera);
DEFINE_HOOK_SYMBOL("daAlink_c::checkPlayerNoDraw", u32(daAlink_c*), PlayerNoDraw);

namespace vr::game_tweaks {
namespace {

bool vr_running() {
    const auto& cfg = config();
    return cfg.mode != Mode::Off && (xr::session_running() || cfg.simulateHmd);
}

// --- Fades --------------------------------------------------------------------------------------
// Many scene transitions fade through white (the game picks it per wipe type), which is a harsh
// flash filling a headset. In VR every fade goes through black instead; only the colour changes.

HookAction fader_draw_pre(ModContext*, void* args, void*, void*) {
    if (vr_running()) {
        JUtility::TColor& c = mods::arg<JUTFader*>(args, 0)->mColor;
        c.r = c.g = c.b = 0;
    }
    return HOOK_CONTINUE;
}

HookAction color_filter_pre(ModContext*, void* args, void*, void*) {
    if (vr_running()) {
        GXColor& c = mods::arg_ref<GXColor>(args, 0);
        c.r = c.g = c.b = 0;
    }
    return HOOK_CONTINUE;
}

// --- Camera: turns only when you turn it ---------------------------------------------------------
// Dusklight's free camera keeps the angle the player set with the C-stick instead of swinging back
// behind Link, but only once the C-stick has been touched. With the option on, it holds the current
// angle straight away. The game drops back to its own camera for Z-targeting and in every camera
// style other than the normal follow camera (cutscenes, transitions, fixed-angle spots), as before.

HookAction free_camera_pre(ModContext*, void* args, void*, void*) {
    if (config().manualCamera && vr_running()) {
        dCamera_c* cam = mods::arg<dCamera_c*>(args, 0);
        if (!cam->mCamParam.mManualMode) {
            cam->mCamParam.freeXAngle = cam->mViewCache.mDirection.mAzimuth.Degree();
            cam->mCamParam.freeYAngle = cam->mViewCache.mDirection.mInclination.Degree();
            cam->mCamParam.mManualMode = 1;
        }
    }
    return HOOK_CONTINUE;
}

// --- Aiming in stereo: Link stays visible --------------------------------------------------------
// The aiming camera sits at Link's eyes and asks him not to draw. In stereo the view is pulled back
// behind him instead (render_hooks), so he must draw. Anything else hiding him is left alone.

void player_no_draw_post(ModContext*, void* args, void* retval, void*) {
    u32& hidden = *static_cast<u32*>(retval);
    if (hidden != 0 && aiming_third_person()) {
        const daAlink_c* link = mods::arg<daAlink_c*>(args, 0);
        if (!link->checkNoResetFlg0(daPy_py_c::FLG0_PLAYER_NO_DRAW)) {
            hidden = 0;
        }
    }
}

} // namespace

bool aiming_third_person() {
    const auto& cfg = config();
    if (cfg.mode != Mode::Stereo || !vr_running() || !dComIfGp_checkPlayerStatus0(0, 0x2000) ||
        dComIfGp_checkPlayerStatus0(0, 0x200000))
    {
        return false; // not stereo, not aiming (subject view), or the Hawkeye
    }
    const daAlink_c* link = daAlink_getAlinkActorClass();
    if (link == nullptr) {
        return false;
    }
    switch (link->mEquipItem) {
    case dItemNo_BOW_e:
    case dItemNo_BOMB_ARROW_e:
    case dItemNo_HAWK_ARROW_e:
    case dItemNo_PACHINKO_e:
    case dItemNo_HOOKSHOT_e:
    case dItemNo_W_HOOKSHOT_e:
    case dItemNo_COPY_ROD_e:
    case dItemNo_BOOMERANG_e:
        return true;
    default:
        return false;
    }
}

void install() {
    bool ok = mods::hook::add_pre<FaderDraw>(fader_draw_pre) == MOD_OK;
    ok &= mods::hook::add_pre<ColorFilter>(color_filter_pre) == MOD_OK;
    if (!ok) {
        mods::log::warn("Black fades unavailable");
    }
    if (mods::hook::add_pre<FreeCamera>(free_camera_pre) != MOD_OK) {
        mods::log::warn("Manual camera unavailable");
    }
    if (mods::hook::add_post<PlayerNoDraw>(player_no_draw_post) != MOD_OK) {
        mods::log::warn("Third-person aiming unavailable");
    }
}

} // namespace vr::game_tweaks
