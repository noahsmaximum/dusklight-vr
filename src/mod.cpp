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

UiElementHandle g_statusText = 0;
UiElementHandle g_frameText = 0;

const char* const kModes[] = {"Off", "Stereo (6DOF)", "Cinema screen", "Tabletop"};
const char* const kHudFollow[] = {"Smooth follow", "Head-locked", "Fixed in world"};
// Chroma-key friendly presets for the tabletop background.
const char* const kKeyColors[] = {"000000", "00FF00", "FF00FF", "0000FF"};

void add_toggle(UiElementHandle pane, const char* label, ConfigVarHandle var) {
    UiControlDesc c = UI_CONTROL_DESC_INIT;
    c.kind = UI_CONTROL_TOGGLE;
    c.label = label;
    c.binding = UI_BINDING_CONFIG_VAR;
    c.config_var = var;
    svc_ui->pane_add_control(mod_ctx, pane, &c, nullptr);
}

void add_number(UiElementHandle pane, const char* label, ConfigVarHandle var, int64_t min, int64_t max,
    int64_t step, const char* suffix) {
    UiControlDesc c = UI_CONTROL_DESC_INIT;
    c.kind = UI_CONTROL_NUMBER;
    c.label = label;
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

ModResult build_panel(ModContext*, UiElementHandle pane, void*, ModError*) {
    using vr::g_vars;
    svc_ui->pane_add_section(mod_ctx, pane, "Status");
    svc_ui->pane_add_text(mod_ctx, pane, "", &g_statusText);
    svc_ui->pane_add_text(mod_ctx, pane, "", &g_frameText);

    UiControlDesc recenter = UI_CONTROL_DESC_INIT;
    recenter.kind = UI_CONTROL_BUTTON;
    recenter.label = "Recenter view";
    recenter.on_pressed = [](ModContext*, void*) { vr::xr::request_recenter(); };
    svc_ui->pane_add_control(mod_ctx, pane, &recenter, nullptr);

    svc_ui->pane_add_section(mod_ctx, pane, "Presets");
    for (size_t i = 0; i < vr::preset_count(); ++i) {
        UiControlDesc p = UI_CONTROL_DESC_INIT;
        p.kind = UI_CONTROL_BUTTON;
        p.label = vr::preset_name(i);
        p.user_data = reinterpret_cast<void*>(i);
        p.on_pressed = [](ModContext*, void* index) { vr::apply_preset(reinterpret_cast<size_t>(index)); };
        svc_ui->pane_add_control(mod_ctx, pane, &p, nullptr);
    }

    svc_ui->pane_add_section(mod_ctx, pane, "View");
    add_dropdown(pane, "VR mode", g_vars.mode, kModes, 4);
    add_number(pane, "World scale", g_vars.unitsPerMeter, 10, 1000, 5, " units/m");
    add_number(pane, "Stereo separation", g_vars.ipdPercent, 0, 400, 5, "%");
    add_toggle(pane, "Level horizon (comfort)", g_vars.levelHorizon);
    add_toggle(pane, "Disable depth of field", g_vars.disableDof);
    add_number(pane, "Culling field of view", g_vars.cullFov, 30, 178, 1, " deg");
    add_number(pane, "Render scale (restart session)", g_vars.renderScalePercent, 50, 200, 5, "%");

    svc_ui->pane_add_section(mod_ctx, pane, "Tabletop");
    add_number(pane, "Scale 1:", g_vars.tableScale, 2, 1000, 5, "");
    add_number(pane, "Table height (from eyes)", g_vars.tableHeightCm, -200, 50, 5, " cm");
    add_number(pane, "Table position X (right)", g_vars.tableOffsetXCm, -300, 300, 5, " cm");
    add_number(pane, "Table position Y (forward)", g_vars.tableDistanceCm, 0, 300, 5, " cm");
    add_number(pane, "Table rotation", g_vars.tableYawDeg, -180, 180, 5, " deg");
    add_number(pane, "Visible radius", g_vars.tableRadiusCm, 5, 500, 5, " cm");
    add_number(pane, "Visible depth below table", g_vars.tableDepthCm, 0, 500, 5, " cm");
    add_toggle(pane, "See-through background (passthrough)", g_vars.tablePassthrough);
    add_toggle(pane, "Turn with the game camera", g_vars.tableFollowYaw);
    {
        UiControlDesc c = UI_CONTROL_DESC_INIT;
        c.kind = UI_CONTROL_COLOR;
        c.label = "Background without passthrough";
        c.help_rml = "Fills the empty space around the diorama when the headset runtime cannot show your room. Pick a chroma-key colour if you use a chroma-key passthrough tool.";
        c.binding = UI_BINDING_CONFIG_VAR;
        c.config_var = g_vars.tableKeyColor;
        c.color_presets = kKeyColors;
        c.color_preset_count = 4;
        svc_ui->pane_add_control(mod_ctx, pane, &c, nullptr);
    }

    svc_ui->pane_add_section(mod_ctx, pane, "HUD & menus");
    add_toggle(pane, "Show Dusklight menus in the headset", g_vars.showDuskUi);
    add_dropdown(pane, "HUD placement", g_vars.hudFollow, kHudFollow, 3);
    add_number(pane, "HUD distance", g_vars.hudDistanceCm, 30, 1000, 5, " cm");
    add_number(pane, "HUD width", g_vars.hudWidthCm, 20, 1000, 5, " cm");
    add_number(pane, "HUD height", g_vars.hudHeightCm, -300, 300, 5, " cm");
    add_number(pane, "Menu distance", g_vars.menuDistanceCm, 30, 1000, 5, " cm");
    add_number(pane, "Menu width", g_vars.menuWidthCm, 20, 1000, 5, " cm");
    add_number(pane, "Screen distance", g_vars.screenDistanceCm, 50, 3000, 10, " cm");
    add_number(pane, "Screen width", g_vars.screenWidthCm, 20, 3000, 10, " cm");

    svc_ui->pane_add_section(mod_ctx, pane, "Desktop / debug");
    add_toggle(pane, "Show HUD on desktop mirror", g_vars.mirrorHud);
    add_toggle(pane, "Simulate headset (side-by-side, no HMD)", g_vars.simulateHmd);
    return MOD_OK;
}

ModResult update_panel(ModContext*, void*, ModError*) {
    if (g_statusText != 0) {
        svc_ui->elem_set_text(mod_ctx, g_statusText, vr::xr::status().c_str());
    }
    if (g_frameText != 0) {
        svc_ui->elem_set_text(mod_ctx, g_frameText, vr::render::status().c_str());
    }
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
    // DUSKLIGHT_VR_NO_XR (dev harness): never start a headset session, even if one is connected.
    if (std::getenv("DUSKLIGHT_VR_NO_XR") == nullptr) {
        vr::xr::initialize(info.device, info.adapter);
    }

    UiModsPanelDesc panel = UI_MODS_PANEL_DESC_INIT;
    panel.build = build_panel;
    panel.update = update_panel;
    svc_ui->register_mods_panel(mod_ctx, &panel);

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
