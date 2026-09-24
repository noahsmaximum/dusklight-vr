#include "gpu.hpp"
#include "render_hooks.hpp"
#include "vr_config.hpp"
#include "vr_config_vars.hpp"
#include "xr_runtime.hpp"

#include "mods/service.hpp"
#include "mods/svc/config.h"
#include "mods/svc/gfx.h"
#include "mods/svc/hook.h"
#include "mods/svc/log.hpp"
#include "mods/svc/ui.h"

#include <cstdlib>
#include <string>

DEFINE_MOD();

IMPORT_SERVICE(LogService, svc_log);
IMPORT_SERVICE(HookService, svc_hook);
IMPORT_SERVICE(GfxService, svc_gfx);
IMPORT_SERVICE(ConfigService, svc_config);
IMPORT_SERVICE(UiService, svc_ui);

namespace {

// Status lines, one pair per place they are shown (the Mods panel and the VR window's first tab).
struct StatusText {
    UiElementHandle xr = 0;
    UiElementHandle frame = 0;
};
StatusText g_panelStatus;
StatusText g_windowStatus;
UiWindowHandle g_window = 0;
UiMenuTabHandle g_menuTab = 0;

const char* const kModes[] = {"Off", "Stereo (6DOF)", "Cinema screen", "Tabletop"};
const char* const kHudFollow[] = {"Smooth follow", "Head-locked", "Fixed in world"};
// Chroma-key friendly presets for the tabletop background.
const char* const kKeyColors[] = {"000000", "00FF00", "FF00FF", "0000FF"};

void add_toggle(UiElementHandle pane, const char* label, ConfigVarHandle var, const char* help = nullptr) {
    UiControlDesc c = UI_CONTROL_DESC_INIT;
    c.kind = UI_CONTROL_TOGGLE;
    c.label = label;
    c.help_rml = help;
    c.binding = UI_BINDING_CONFIG_VAR;
    c.config_var = var;
    svc_ui->pane_add_control(mod_ctx, pane, &c, nullptr);
}

void add_number(UiElementHandle pane, const char* label, ConfigVarHandle var, int64_t min, int64_t max,
    int64_t step, const char* suffix, const char* help = nullptr) {
    UiControlDesc c = UI_CONTROL_DESC_INIT;
    c.kind = UI_CONTROL_NUMBER;
    c.label = label;
    c.help_rml = help;
    c.binding = UI_BINDING_CONFIG_VAR;
    c.config_var = var;
    c.min = min;
    c.max = max;
    c.step = step;
    c.suffix = suffix;
    svc_ui->pane_add_control(mod_ctx, pane, &c, nullptr);
}

void add_dropdown(UiElementHandle pane, const char* label, ConfigVarHandle var, const char* const* options,
    size_t count) {
    UiControlDesc c = UI_CONTROL_DESC_INIT;
    c.kind = UI_CONTROL_DROPDOWN;
    c.label = label;
    c.binding = UI_BINDING_CONFIG_VAR;
    c.config_var = var;
    c.options = options;
    c.option_count = count;
    svc_ui->pane_add_control(mod_ctx, pane, &c, nullptr);
}

void add_button(UiElementHandle pane, const char* label, UiPressedFn onPressed, void* userData = nullptr) {
    UiControlDesc c = UI_CONTROL_DESC_INIT;
    c.kind = UI_CONTROL_BUTTON;
    c.label = label;
    c.on_pressed = onPressed;
    c.user_data = userData;
    svc_ui->pane_add_control(mod_ctx, pane, &c, nullptr);
}

void add_status(UiElementHandle pane, StatusText& status) {
    svc_ui->pane_add_text(mod_ctx, pane, "", &status.xr);
    svc_ui->pane_add_text(mod_ctx, pane, "", &status.frame);
}

void update_status(const StatusText& status) {
    if (status.xr != 0) {
        svc_ui->elem_set_text(mod_ctx, status.xr, vr::xr::status().c_str());
    }
    if (status.frame != 0) {
        svc_ui->elem_set_text(mod_ctx, status.frame, vr::render::status().c_str());
    }
}

void recenter(ModContext*, void*) { vr::xr::request_recenter(); }

// --- VR settings window -------------------------------------------------------------------------
// Tabs are rebuilt on every activation; only the General tab shows (and updates) the status.

ModResult build_general_tab(ModContext*, UiWindowHandle, UiElementHandle left, UiElementHandle, void*, ModError*) {
    using vr::g_vars;
    svc_ui->pane_add_section(mod_ctx, left, "Status");
    add_status(left, g_windowStatus);
    add_button(left, "Recenter view", recenter);
    add_dropdown(left, "VR mode", g_vars.mode, kModes, 4);

    svc_ui->pane_add_section(mod_ctx, left, "Presets");
    for (size_t i = 0; i < vr::preset_count(); ++i) {
        add_button(left, vr::preset_name(i),
            [](ModContext*, void* index) { vr::apply_preset(reinterpret_cast<size_t>(index)); },
            reinterpret_cast<void*>(i));
    }
    return MOD_OK;
}

ModResult update_general_tab(ModContext*, void*, ModError*) {
    update_status(g_windowStatus);
    return MOD_OK;
}

ModResult build_view_tab(ModContext*, UiWindowHandle, UiElementHandle left, UiElementHandle, void*, ModError*) {
    using vr::g_vars;
    g_windowStatus = {};
    svc_ui->pane_add_section(mod_ctx, left, "Stereo");
    add_number(left, "World scale", g_vars.unitsPerMeter, 10, 1000, 5, " units/m",
        "Game units per real metre in stereo. Lower makes the world feel bigger.");
    add_number(left, "Stereo separation", g_vars.ipdPercent, 0, 400, 5, "%");
    add_toggle(left, "Level horizon (comfort)", g_vars.levelHorizon,
        "Removes the game camera's pitch and roll, so the horizon stays level.");
    add_toggle(left, "Disable depth of field", g_vars.disableDof);
    add_number(left, "Culling field of view", g_vars.cullFov, 30, 178, 1, " deg",
        "How wide around the camera the game keeps drawing objects, so turning your head never shows gaps.");

    svc_ui->pane_add_section(mod_ctx, left, "Performance");
    add_number(left, "Render scale (restart session)", g_vars.renderScalePercent, 50, 200, 5, "%");
    return MOD_OK;
}

ModResult build_tabletop_tab(ModContext*, UiWindowHandle, UiElementHandle left, UiElementHandle, void*, ModError*) {
    using vr::g_vars;
    g_windowStatus = {};
    svc_ui->pane_add_section(mod_ctx, left, "Table");
    add_number(left, "Scale 1:", g_vars.tableScale, 2, 1000, 5, "");
    add_number(left, "Table height (from eyes)", g_vars.tableHeightCm, -200, 50, 5, " cm");
    add_number(left, "Table position X (right)", g_vars.tableOffsetXCm, -300, 300, 5, " cm");
    add_number(left, "Table position Y (forward)", g_vars.tableDistanceCm, 0, 300, 5, " cm");
    add_number(left, "Table rotation", g_vars.tableYawDeg, -180, 180, 5, " deg");
    add_number(left, "Visible radius", g_vars.tableRadiusCm, 5, 500, 5, " cm");
    add_number(left, "Visible depth below table", g_vars.tableDepthCm, 0, 500, 5, " cm");
    add_toggle(left, "Turn with the game camera", g_vars.tableFollowYaw);

    svc_ui->pane_add_section(mod_ctx, left, "Background");
    add_toggle(left, "See-through background (passthrough)", g_vars.tablePassthrough);
    {
        UiControlDesc c = UI_CONTROL_DESC_INIT;
        c.kind = UI_CONTROL_COLOR;
        c.label = "Background without passthrough";
        c.help_rml = "Fills the empty space around the diorama when the headset runtime cannot show your room. Pick a chroma-key colour if you use a chroma-key passthrough tool.";
        c.binding = UI_BINDING_CONFIG_VAR;
        c.config_var = g_vars.tableKeyColor;
        c.color_presets = kKeyColors;
        c.color_preset_count = 4;
        svc_ui->pane_add_control(mod_ctx, left, &c, nullptr);
    }

    svc_ui->pane_add_section(mod_ctx, left, "X-ray");
    add_toggle(left, "Keep Link visible", g_vars.tableXray,
        "When the level hides Link, a cylinder of clear view opens from your eyes to him. It widens "
        "the more of him is hidden; leaves and other see-through scenery always clear.");
    add_number(left, "X-ray radius (game world)", g_vars.tableXrayRadius, 50, 2000, 25, " cm");
    add_number(left, "Fade objects near your eyes", g_vars.tableFadeNearCm, 0, 200, 5, " cm",
        "Scenery closer to your eyes than this fades out (0 turns it off).");

    svc_ui->pane_add_section(mod_ctx, left, "HUD & item wheel");
    add_toggle(left, "HUD flat on the table", g_vars.tableHudFlat,
        "The HUD lies on the table, facing up, instead of floating in front of you.");
    add_number(left, "HUD size on the table", g_vars.tableHudWidthCm, 5, 300, 5, " cm");
    add_toggle(left, "Item wheel around Link", g_vars.tableWheelAtLink,
        "The item wheel opens around Link in the diorama (when it does not pause the game).");
    add_number(left, "Item wheel size", g_vars.tableWheelWidthCm, 10, 300, 5, " cm");
    add_toggle(left, "Item wheel pauses the game", g_vars.tableWheelPause,
        "Off: quick switching. The world keeps going while the wheel is open, and Link stands still. "
        "On: the wheel pauses the game as usual.");
    return MOD_OK;
}

ModResult build_hud_tab(ModContext*, UiWindowHandle, UiElementHandle left, UiElementHandle, void*, ModError*) {
    using vr::g_vars;
    g_windowStatus = {};
    svc_ui->pane_add_section(mod_ctx, left, "HUD");
    add_dropdown(left, "HUD placement", g_vars.hudFollow, kHudFollow, 3);
    add_number(left, "HUD distance", g_vars.hudDistanceCm, 30, 1000, 5, " cm");
    add_number(left, "HUD width", g_vars.hudWidthCm, 20, 1000, 5, " cm");
    add_number(left, "HUD height", g_vars.hudHeightCm, -300, 300, 5, " cm");

    svc_ui->pane_add_section(mod_ctx, left, "Menus");
    add_toggle(left, "Show Dusklight menus in the headset", g_vars.showDuskUi);
    add_number(left, "Menu distance", g_vars.menuDistanceCm, 30, 1000, 5, " cm");
    add_number(left, "Menu width", g_vars.menuWidthCm, 20, 1000, 5, " cm");

    svc_ui->pane_add_section(mod_ctx, left, "Cinema screen");
    add_number(left, "Screen distance", g_vars.screenDistanceCm, 50, 3000, 10, " cm");
    add_number(left, "Screen width", g_vars.screenWidthCm, 20, 3000, 10, " cm");
    return MOD_OK;
}

ModResult build_advanced_tab(ModContext*, UiWindowHandle, UiElementHandle left, UiElementHandle, void*, ModError*) {
    using vr::g_vars;
    g_windowStatus = {};
    svc_ui->pane_add_section(mod_ctx, left, "Desktop");
    add_toggle(left, "Show HUD on desktop mirror", g_vars.mirrorHud);
    add_toggle(left, "Simulate headset (side-by-side, no HMD)", g_vars.simulateHmd,
        "Runs the whole VR pipeline without a headset and shows both eyes on the desktop.");
    return MOD_OK;
}

void open_window(ModContext*, void*) {
    if (g_window != 0) {
        return;
    }
    UiTabDesc tabs[5] = {UI_TAB_DESC_INIT, UI_TAB_DESC_INIT, UI_TAB_DESC_INIT, UI_TAB_DESC_INIT, UI_TAB_DESC_INIT};
    tabs[0].title = "General";
    tabs[0].build = build_general_tab;
    tabs[0].update = update_general_tab;
    tabs[1].title = "View";
    tabs[1].build = build_view_tab;
    tabs[2].title = "Tabletop";
    tabs[2].build = build_tabletop_tab;
    tabs[3].title = "HUD & menus";
    tabs[3].build = build_hud_tab;
    tabs[4].title = "Advanced";
    tabs[4].build = build_advanced_tab;
    UiWindowDesc desc = UI_WINDOW_DESC_INIT;
    desc.tabs = tabs;
    desc.tab_count = 5;
    desc.on_closed = [](ModContext*, UiWindowHandle, void*) {
        g_window = 0;
        g_windowStatus = {};
    };
    if (svc_ui->window_push(mod_ctx, &desc, &g_window) != MOD_OK) {
        mods::log::error("failed to open the VR settings window");
        g_window = 0;
    }
}

// --- Mods panel ---------------------------------------------------------------------------------

ModResult build_panel(ModContext*, UiElementHandle pane, void*, ModError*) {
    using vr::g_vars;
    svc_ui->pane_add_section(mod_ctx, pane, "Status");
    add_status(pane, g_panelStatus);
    add_button(pane, "Open VR settings", open_window);
    add_button(pane, "Recenter view", recenter);
    add_dropdown(pane, "VR mode", g_vars.mode, kModes, 4);
    return MOD_OK;
}

ModResult update_panel(ModContext*, void*, ModError*) {
    update_status(g_panelStatus);
    return MOD_OK;
}

} // namespace

extern "C" {

MOD_EXPORT ModResult mod_initialize(ModError* error) {
    if (!vr::register_config()) {
        return mods::set_error(error, MOD_ERROR, "failed to register settings");
    }

    GfxDeviceInfo info = GFX_DEVICE_INFO_INIT;
    if (svc_gfx->get_device_info(mod_ctx, &info) != MOD_OK || info.device == nullptr) {
        return mods::set_error(error, MOD_UNAVAILABLE, "no graphics device");
    }
    if (!vr::gpu::initialize(info.device)) {
        return mods::set_error(error, MOD_ERROR, "failed to create VR GPU resources");
    }
    if (!vr::render::install()) {
        return mods::set_error(error, MOD_ERROR, "failed to hook the renderer (see log)");
    }
    // A missing runtime/headset is not fatal: the mod idles (and "Simulate headset" still works).
    // DUSKLIGHT_VR_NO_XR (dev harness): never start a headset session, even if one is connected
    // (graphics interop is still set up so "Simulate headset" exercises the full copy path).
    vr::xr::initialize(info.device, info.adapter, std::getenv("DUSKLIGHT_VR_NO_XR") == nullptr);

    UiModsPanelDesc panel = UI_MODS_PANEL_DESC_INIT;
    panel.build = build_panel;
    panel.update = update_panel;
    svc_ui->register_mods_panel(mod_ctx, &panel);

    // "VR" in Dusklight's top bar opens the settings window.
    UiMenuTabDesc tab = UI_MENU_TAB_DESC_INIT;
    tab.label = "VR";
    tab.on_selected = open_window;
    if (svc_ui->register_menu_tab(mod_ctx, &tab, &g_menuTab) != MOD_OK) {
        mods::log::warn("could not add VR to the menu bar");
    }

    mods::log::info("Dusklight VR ready");
    return MOD_OK;
}

MOD_EXPORT ModResult mod_update(ModError*) { return MOD_OK; }

MOD_EXPORT ModResult mod_shutdown(ModError*) {
    vr::render::uninstall();
    vr::xr::shutdown();
    vr::gpu::shutdown();
    return MOD_OK;
}
}
