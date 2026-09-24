#include "JSystem/J3DGraphAnimator/J3DModel.h"
#include "JSystem/J3DGraphBase/J3DSys.h"
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
DEFINE_HOOK_SYMBOL("src/f_op/f_op_actor.cpp#fopAc_Draw", int(void*), ActorDraw);
DEFINE_HOOK_SYMBOL("J3DModel::entry", void(J3DModel*), ModelEntry);

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

// --- Characters out of the x-ray ----------------------------------------------------------------
// The tabletop x-ray cuts what the map and actor draw lists hold, but characters should stay whole.
// While Link, an NPC or an enemy draws, its models go into the "dark" actor lists instead: they are
// drawn at the same point of the frame, and the x-ray leaves them alone (see render_hooks).

bool g_drawingCharacter = false;
J3DDrawBuffer* g_savedOpa = nullptr;
J3DDrawBuffer* g_savedXlu = nullptr;

HookAction actor_draw_pre(ModContext*, void* args, void*, void*) {
    const auto* actor = static_cast<const fopAc_ac_c*>(mods::arg<void*>(args, 0));
    g_drawingCharacter = actor != nullptr && config().mode == Mode::Tabletop && vr_running() &&
                         (actor->group == fopAc_PLAYER_e || actor->group == fopAc_ENEMY_e ||
                             actor->group == fopAc_NPC_e);
    return HOOK_CONTINUE;
}

void actor_draw_post(ModContext*, void*, void*, void*) { g_drawingCharacter = false; }

HookAction model_entry_pre(ModContext*, void*, void*, void*) {
    g_savedOpa = g_savedXlu = nullptr;
    if (!g_drawingCharacter) {
        return HOOK_CONTINUE;
    }
    auto& lists = g_dComIfG_gameInfo.drawlist;
    if (j3dSys.getDrawBuffer(J3DSysDrawBuf_Opa) == lists.mDrawBuffers[dDlst_list_c::DB_OPA_LIST]) {
        g_savedOpa = j3dSys.getDrawBuffer(J3DSysDrawBuf_Opa);
        j3dSys.setDrawBuffer(lists.mDrawBuffers[dDlst_list_c::DB_OPA_LIST_DARK], J3DSysDrawBuf_Opa);
    }
    if (j3dSys.getDrawBuffer(J3DSysDrawBuf_Xlu) == lists.mDrawBuffers[dDlst_list_c::DB_XLU_LIST]) {
        g_savedXlu = j3dSys.getDrawBuffer(J3DSysDrawBuf_Xlu);
        j3dSys.setDrawBuffer(lists.mDrawBuffers[dDlst_list_c::DB_XLU_LIST_DARK], J3DSysDrawBuf_Xlu);
    }
    return HOOK_CONTINUE;
}

void model_entry_post(ModContext*, void*, void*, void*) {
    if (g_savedOpa != nullptr) {
        j3dSys.setDrawBuffer(g_savedOpa, J3DSysDrawBuf_Opa);
    }
    if (g_savedXlu != nullptr) {
        j3dSys.setDrawBuffer(g_savedXlu, J3DSysDrawBuf_Xlu);
    }
    g_savedOpa = g_savedXlu = nullptr;
}

} // namespace

bool aiming_third_person() {
    const auto& cfg = config();
    if (cfg.mode != Mode::Stereo || !vr_running() || dComIfGp_checkPlayerStatus0(0, 0x200000)) {
        return false; // not stereo, or the Hawkeye (its own 3D screen)
    }
    daAlink_c* link = daAlink_getAlinkActorClass();
    if (link == nullptr) {
        return false;
    }
    // Aiming: the item's sight is up, the subject view is on, or the camera went to Link's eyes (it
    // then asks him not to draw).
    if (!link->mSight.getDrawFlg() && !dComIfGp_checkPlayerStatus0(0, 0x2000) &&
        !dComIfGp_checkCameraAttentionStatus(dComIfGp_getPlayerCameraID(0), 2))
    {
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
    if (mods::hook::add_pre<ActorDraw>(actor_draw_pre) != MOD_OK ||
        mods::hook::add_post<ActorDraw>(actor_draw_post) != MOD_OK ||
        mods::hook::add_pre<ModelEntry>(model_entry_pre) != MOD_OK ||
        mods::hook::add_post<ModelEntry>(model_entry_post) != MOD_OK)
    {
        mods::log::warn("Tabletop x-ray will also cut characters (could not tell their models apart)");
    }
}

} // namespace vr::game_tweaks
