#pragma once

#include <cstdint>

// Persistent user settings (ConfigService vars), cached into plain values each frame so hot paths
// never call into the service.
namespace vr {

enum class Mode : int {
    Off = 0,
    Stereo = 1, // per-eye rendering, 6DOF head tracking
    Cinema = 2, // the flat game on a large world-locked virtual screen
};

enum class HudFollow : int {
    Smooth = 0,     // panel lazily re-centres in front of the head (yaw only)
    HeadLocked = 1, // panel rigidly attached to the head
    World = 2,      // panel stays where it was placed at recentre
};

struct Config {
    Mode mode = Mode::Stereo;
    float unitsPerMeter = 100.0f; // game units per real-world metre
    float ipdScale = 1.0f;        // extra stereo separation multiplier
    bool levelHorizon = true;     // strip game-camera pitch/roll (comfort)
    int cullFovDeg = 170;         // sim-time frustum culling FOV (wide so turning your head never reveals holes)
    bool disableDof = true;       // depth-of-field is uncomfortable in a headset

    HudFollow hudFollow = HudFollow::Smooth;
    float hudDistance = 1.6f; // metres
    float hudWidth = 1.7f;    // metres
    float hudHeight = -0.15f; // metres relative to eye level
    float menuDistance = 1.4f;
    float menuWidth = 1.8f;
    float screenDistance = 3.0f; // cinema / 2D-only screens
    float screenWidth = 4.0f;

    bool mirrorHud = true;       // composite the HUD onto the desktop mirror
    bool simulateHmd = false;    // debug: run the stereo pipeline without a headset
    float renderScale = 1.0f;    // XR swapchain size multiplier (relative to the runtime's recommendation)
};

const Config& config();

// Registers the ConfigService vars. Call once from mod_initialize.
bool register_config();
// Refresh the cached values from ConfigService (cheap; call once per frame on the game thread).
void refresh_config();

} // namespace vr
