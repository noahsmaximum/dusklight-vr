#include "vr_config.hpp"
#include "vr_config_vars.hpp"

#include "mods/svc/config.h"
#include "mods/svc/log.hpp"

#include <algorithm>

namespace vr {
namespace {
Config g_config;
} // namespace

ConfigVars g_vars;

const Config& config() { return g_config; }

static bool reg_int(ConfigVarHandle& out, const char* name, int64_t def) {
    ConfigVarDesc desc = CONFIG_VAR_DESC_INIT;
    desc.name = name;
    desc.type = CONFIG_VAR_INT;
    desc.default_int = def;
    if (svc_config->register_var(mod_ctx, &desc, &out) != MOD_OK) {
        mods::log::error("failed to register config var {}", name);
        return false;
    }
    return true;
}

static bool reg_bool(ConfigVarHandle& out, const char* name, bool def) {
    ConfigVarDesc desc = CONFIG_VAR_DESC_INIT;
    desc.name = name;
    desc.type = CONFIG_VAR_BOOL;
    desc.default_bool = def;
    if (svc_config->register_var(mod_ctx, &desc, &out) != MOD_OK) {
        mods::log::error("failed to register config var {}", name);
        return false;
    }
    return true;
}

bool register_config() {
    bool ok = true;
    ok &= reg_int(g_vars.mode, "mode", static_cast<int>(Mode::Stereo));
    ok &= reg_int(g_vars.unitsPerMeter, "unitsPerMeter", 100);
    ok &= reg_int(g_vars.ipdPercent, "ipdPercent", 100);
    ok &= reg_bool(g_vars.levelHorizon, "levelHorizon", true);
    ok &= reg_int(g_vars.cullFov, "cullFov", 170);
    ok &= reg_bool(g_vars.disableDof, "disableDof", true);
    ok &= reg_int(g_vars.hudFollow, "hudFollow", static_cast<int>(HudFollow::Smooth));
    ok &= reg_int(g_vars.hudDistanceCm, "hudDistanceCm", 160);
    ok &= reg_int(g_vars.hudWidthCm, "hudWidthCm", 170);
    ok &= reg_int(g_vars.hudHeightCm, "hudHeightCm", -15);
    ok &= reg_int(g_vars.menuDistanceCm, "menuDistanceCm", 140);
    ok &= reg_int(g_vars.menuWidthCm, "menuWidthCm", 180);
    ok &= reg_int(g_vars.screenDistanceCm, "screenDistanceCm", 300);
    ok &= reg_int(g_vars.screenWidthCm, "screenWidthCm", 400);
    ok &= reg_int(g_vars.tableScale, "tableScale", 50);
    ok &= reg_int(g_vars.tableHeightCm, "tableHeightCm", -50);
    ok &= reg_int(g_vars.tableDistanceCm, "tableDistanceCm", 45);
    ok &= reg_int(g_vars.tableRadiusCm, "tableRadiusCm", 40);
    ok &= reg_int(g_vars.tableDepthCm, "tableDepthCm", 15);
    ok &= reg_bool(g_vars.tablePassthrough, "tablePassthrough", true);
    ok &= reg_bool(g_vars.tableFollowYaw, "tableFollowYaw", true);
    ok &= reg_bool(g_vars.mirrorHud, "mirrorHud", true);
    ok &= reg_bool(g_vars.simulateHmd, "simulateHmd", false);
    ok &= reg_int(g_vars.renderScalePercent, "renderScalePercent", 100);
    refresh_config();
    return ok;
}

static int64_t get_int(ConfigVarHandle h, int64_t fallback) {
    int64_t v = fallback;
    if (h == 0 || svc_config->get_int(mod_ctx, h, &v) != MOD_OK) {
        return fallback;
    }
    return v;
}

static bool get_bool(ConfigVarHandle h, bool fallback) {
    bool v = fallback;
    if (h == 0 || svc_config->get_bool(mod_ctx, h, &v) != MOD_OK) {
        return fallback;
    }
    return v;
}

void refresh_config() {
    Config c;
    c.mode = static_cast<Mode>(std::clamp<int64_t>(get_int(g_vars.mode, 1), 0, 3));
    c.unitsPerMeter = static_cast<float>(std::clamp<int64_t>(get_int(g_vars.unitsPerMeter, 100), 10, 1000));
    c.ipdScale = static_cast<float>(std::clamp<int64_t>(get_int(g_vars.ipdPercent, 100), 0, 400)) / 100.0f;
    c.levelHorizon = get_bool(g_vars.levelHorizon, true);
    c.cullFovDeg = static_cast<int>(std::clamp<int64_t>(get_int(g_vars.cullFov, 170), 30, 178));
    c.disableDof = get_bool(g_vars.disableDof, true);
    c.hudFollow = static_cast<HudFollow>(std::clamp<int64_t>(get_int(g_vars.hudFollow, 0), 0, 2));
    c.hudDistance = static_cast<float>(std::clamp<int64_t>(get_int(g_vars.hudDistanceCm, 160), 30, 1000)) / 100.0f;
    c.hudWidth = static_cast<float>(std::clamp<int64_t>(get_int(g_vars.hudWidthCm, 170), 20, 1000)) / 100.0f;
    c.hudHeight = static_cast<float>(std::clamp<int64_t>(get_int(g_vars.hudHeightCm, -15), -300, 300)) / 100.0f;
    c.menuDistance = static_cast<float>(std::clamp<int64_t>(get_int(g_vars.menuDistanceCm, 140), 30, 1000)) / 100.0f;
    c.menuWidth = static_cast<float>(std::clamp<int64_t>(get_int(g_vars.menuWidthCm, 180), 20, 1000)) / 100.0f;
    c.screenDistance =
        static_cast<float>(std::clamp<int64_t>(get_int(g_vars.screenDistanceCm, 300), 50, 3000)) / 100.0f;
    c.screenWidth = static_cast<float>(std::clamp<int64_t>(get_int(g_vars.screenWidthCm, 400), 20, 3000)) / 100.0f;
    // Scale 1:N with the game's ~1 unit per centimetre.
    c.tableUnitsPerMeter = static_cast<float>(std::clamp<int64_t>(get_int(g_vars.tableScale, 50), 2, 1000)) * 100.0f;
    c.tableHeight = static_cast<float>(std::clamp<int64_t>(get_int(g_vars.tableHeightCm, -50), -200, 50)) / 100.0f;
    c.tableDistance = static_cast<float>(std::clamp<int64_t>(get_int(g_vars.tableDistanceCm, 45), 0, 300)) / 100.0f;
    c.tableRadius = static_cast<float>(std::clamp<int64_t>(get_int(g_vars.tableRadiusCm, 40), 5, 500)) / 100.0f;
    c.tableDepth = static_cast<float>(std::clamp<int64_t>(get_int(g_vars.tableDepthCm, 15), 0, 500)) / 100.0f;
    c.tablePassthrough = get_bool(g_vars.tablePassthrough, true);
    c.tableFollowYaw = get_bool(g_vars.tableFollowYaw, true);
    c.mirrorHud = get_bool(g_vars.mirrorHud, true);
    c.simulateHmd = get_bool(g_vars.simulateHmd, false);
    c.renderScale = static_cast<float>(std::clamp<int64_t>(get_int(g_vars.renderScalePercent, 100), 50, 200)) / 100.0f;
    g_config = c;
}

} // namespace vr
