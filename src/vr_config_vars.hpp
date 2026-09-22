#pragma once

#include "mods/svc/config.h"

namespace vr {

// Raw ConfigService handles, shared with the settings panel so controls bind straight to them.
struct ConfigVars {
    ConfigVarHandle mode = 0;
    ConfigVarHandle unitsPerMeter = 0;
    ConfigVarHandle ipdPercent = 0;
    ConfigVarHandle levelHorizon = 0;
    ConfigVarHandle cullFov = 0;
    ConfigVarHandle disableDof = 0;
    ConfigVarHandle hudFollow = 0;
    ConfigVarHandle hudDistanceCm = 0;
    ConfigVarHandle hudWidthCm = 0;
    ConfigVarHandle hudHeightCm = 0;
    ConfigVarHandle menuDistanceCm = 0;
    ConfigVarHandle menuWidthCm = 0;
    ConfigVarHandle screenDistanceCm = 0;
    ConfigVarHandle screenWidthCm = 0;
    ConfigVarHandle tableScale = 0; // 1:N
    ConfigVarHandle tableHeightCm = 0;
    ConfigVarHandle tableDistanceCm = 0;
    ConfigVarHandle tableRadiusCm = 0;
    ConfigVarHandle tableDepthCm = 0;
    ConfigVarHandle tablePassthrough = 0;
    ConfigVarHandle tableFollowYaw = 0;
    ConfigVarHandle mirrorHud = 0;
    ConfigVarHandle simulateHmd = 0;
    ConfigVarHandle renderScalePercent = 0;
};

extern ConfigVars g_vars;

} // namespace vr
