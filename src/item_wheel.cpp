#include "d/d_com_inf_game.h"
#include "d/d_menu_ring.h"
#include "d/d_menu_window.h"
#include "d/d_meter_HIO.h"
#include "d/d_pane_class.h"
#include "d/d_meter2_info.h"
#include "m_Do/m_Do_controller_pad.h"

#include "item_wheel.hpp"

#include "vr_config.hpp"
#include "xr_runtime.hpp"

#include "mods/svc/hook.hpp"
#include "mods/svc/log.hpp"

#include <chrono>
#include <cstdio>
#include <cstdlib>
#include <cstring>

// Opening the item wheel normally pauses the game: the menu window snapshots the frame (which sets
// the pause flag) and shows the snapshot behind the wheel. In tabletop the wheel can instead run
// "quick": no snapshot, so the world keeps going around Link, and only Link and the camera stop
// hearing the controller (the wheel itself still reads it).
DEFINE_HOOK_SYMBOL("dDlst_MENU_CAPTURE_c::draw", void(void*), MenuCaptureDraw);
DEFINE_HOOK_SYMBOL("dMw_c::_draw", int(void*), MenuWindowDraw);
DEFINE_HOOK_SYMBOL("dMenu_Ring_c::_draw", void(dMenu_Ring_c*), RingDraw);
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

// --- Layout of the wheel around Link ---------------------------------------------------------------
// The wheel reads its layout from the g_ringHIO tuning block every draw (and moves its panes when a
// value changed), so the tabletop layout is swapped in around the wheel's own draw only.

bool wheel_at_link() {
    const auto& cfg = config();
    return quick_wheel() && cfg.tableWheelAtLink;
}

struct RingLayout {
    f32 overlayAlpha;
    f32 itemNamePosX, itemNamePosY;
    f32 guidePosX[10], guidePosY[10];
};
RingLayout g_savedLayout{};
bool g_layoutSwapped = false;

RingLayout current_layout() {
    RingLayout l{};
    l.overlayAlpha = g_ringHIO.mOverlayAlpha;
    l.itemNamePosX = g_ringHIO.mItemNamePosX;
    l.itemNamePosY = g_ringHIO.mItemNamePosY;
    std::memcpy(l.guidePosX, g_ringHIO.mGuidePosX, sizeof(l.guidePosX));
    std::memcpy(l.guidePosY, g_ringHIO.mGuidePosY, sizeof(l.guidePosY));
    return l;
}

void apply_layout(const RingLayout& l) {
    g_ringHIO.mOverlayAlpha = l.overlayAlpha;
    g_ringHIO.mItemNamePosX = l.itemNamePosX;
    g_ringHIO.mItemNamePosY = l.itemNamePosY;
    std::memcpy(g_ringHIO.mGuidePosX, l.guidePosX, sizeof(l.guidePosX));
    std::memcpy(g_ringHIO.mGuidePosY, l.guidePosY, sizeof(l.guidePosY));
}

HookAction ring_draw_pre(ModContext*, void* args, void*, void*) {
    if (!wheel_at_link()) {
        return HOOK_CONTINUE;
    }
    g_savedLayout = current_layout();
    g_layoutSwapped = true;
    RingLayout l = g_savedLayout;
    l.overlayAlpha = 0.0f; // no dimmed backdrop: the diorama stays visible around the wheel
    // Link stands in the middle of the wheel, so keep it clear: the item name moves down to the
    // bottom of the ring, and the button guides (set item, rotate, direct select) to the right,
    // stacked under the main button cluster. The guide values are offsets from each pane's layout
    // position; these targets were measured in the wheel's 608x448 screen (defaults in comments).
    struct Move {
        int guide;
        f32 fromX, fromY; // screen position with the game's default offsets
        f32 toX, toY;
    };
    for (const Move& m : {Move{dMeter_ringHIO_c::SET_ITEM, 245.0f, 161.0f, 505.0f, 180.0f},
             Move{dMeter_ringHIO_c::ROTATE, 200.0f, 252.0f, 505.0f, 210.0f},
             Move{dMeter_ringHIO_c::DIRECT_SELECT, 257.0f, 289.0f, 495.0f, 240.0f}})
    {
        l.guidePosX[m.guide] += m.toX - m.fromX;
        l.guidePosY[m.guide] += m.toY - m.fromY;
    }
    l.itemNamePosY += 110.0f; // from the centre (y 211) to near the bottom of the ring
    apply_layout(l);

    static int logged = 0;
    if (const char* path = std::getenv("DUSKLIGHT_VR_WHEEL_DUMP"); path != nullptr && logged < 30 && ++logged == 30) {
        // Dev: pane centres in screen space, to place the tabletop layout.
        if (FILE* out = std::fopen(path, "w")) {
            dMenu_Ring_c* ring = mods::arg<dMenu_Ring_c*>(args, 0);
            CPaneMgr mgr;
            const auto dump = [&](const char* name, J2DPane* pane) {
                if (pane != nullptr) {
                    const Vec c = mgr.getGlobalVtxCenter(pane, true, 0);
                    std::fprintf(out, "%s %.1f %.1f\n", name, c.x, c.y);
                }
            };
            for (int i : {0, 1, 3}) {
                if (ring->mpTextParent[i] != nullptr) {
                    char name[16];
                    std::snprintf(name, sizeof(name), "guide%d", i);
                    dump(name, ring->mpTextParent[i]->getPanePtr());
                    std::fprintf(out, "guide%d_offset %.1f %.1f\n", i, g_ringHIO.mGuidePosX[i], g_ringHIO.mGuidePosY[i]);
                }
            }
            dump("name", ring->mpNameParent->getPanePtr());
            std::fprintf(out, "name_offset %.1f %.1f\n", g_ringHIO.mItemNamePosX, g_ringHIO.mItemNamePosY);
            dump("circle", ring->mpCircle->getPanePtr());
            std::fclose(out);
        }
    }
    return HOOK_CONTINUE;
}

void ring_draw_post(ModContext*, void*, void*, void*) {
    if (g_layoutSwapped) {
        apply_layout(g_savedLayout);
        g_layoutSwapped = false;
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

void debug_tick() {
    // DUSKLIGHT_VR_TEST_WHEEL=<seconds> (dev harness): open the item wheel once after that long.
    static const int delay = [] {
        const char* v = std::getenv("DUSKLIGHT_VR_TEST_WHEEL");
        return v != nullptr ? std::atoi(v) : 0;
    }();
    static const auto start = std::chrono::steady_clock::now();
    static bool done = false;
    if (delay > 0 && !done && std::chrono::steady_clock::now() - start > std::chrono::seconds(delay)) {
        done = true;
        if (dMw_c* menu = dMeter2Info_getMenuWindowClass()) {
            menu->field_0x14B = 2;
            mods::log::info("test: opening the item wheel");
        }
    }
}

void install() {
    bool ok = mods::hook::add_pre<MenuCaptureDraw>(capture_pre) == MOD_OK;
    ok &= mods::hook::add_pre<MenuWindowDraw>(menu_draw_pre) == MOD_OK;
    ok &= mods::hook::add_post<MenuWindowDraw>(menu_draw_post) == MOD_OK;
    ok &= mods::hook::add_pre<RingDraw>(ring_draw_pre) == MOD_OK;
    ok &= mods::hook::add_post<RingDraw>(ring_draw_post) == MOD_OK;
    ok &= mods::hook::add_pre<LinkExecute>(mute_pre) == MOD_OK;
    ok &= mods::hook::add_post<LinkExecute>(mute_post) == MOD_OK;
    ok &= mods::hook::add_pre<CameraExecute>(mute_pre) == MOD_OK;
    ok &= mods::hook::add_post<CameraExecute>(mute_post) == MOD_OK;
    if (!ok) {
        mods::log::warn("Quick item wheel unavailable (the wheel pauses the game as usual)");
    }
}

} // namespace vr::item_wheel
