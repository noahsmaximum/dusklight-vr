#pragma once

#include <cstddef>
#include <cstdint>

// Persistent user settings (ConfigService vars), cached into plain values each frame so hot paths
// never call into the service.
namespace vr {

enum class Mode : int {
    Off = 0,
    Stereo = 1, // per-eye rendering, 6DOF head tracking
    Cinema = 2, // the flat game on a large world-locked virtual screen
    Tabletop = 3, // the world as a small diorama on a real table, sky see-through
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

    // Tabletop (all positions relative to the head at recentre, metres)
    float tableUnitsPerMeter = 5000.0f; // game units per metre of table (scale 1:50)
    float tableHeight = -0.5f;          // table surface relative to eye level
    float tableDistance = 0.45f;        // table centre in front of the head
    float tableRadius = 0.4f;           // visible world radius around the player
    float tableDepth = 0.15f;           // how far below the surface the world stays visible
    bool tablePassthrough = true;       // show the real room around the diorama when the runtime can
    bool tableFollowYaw = true;         // turn the diorama with the game camera (stick-up = away from you)
    float tableOffsetX = 0.0f;          // table centre moved right (+) / left (-)
    float tableYawDeg = 0.0f;           // diorama turned about the table centre (counter-clockwise +)
    uint32_t tableKeyColor = 0x000000;  // background when the runtime is opaque (0xRRGGBB)
    bool tableXray = true;              // keep Link visible: cut away what stands between you and him
    float tableXrayRadius = 200.0f;     // x-ray radius around Link, game units (~cm of game world)
    float tableFadeNear = 0.2f;         // geometry closer to your eyes than this fades out (metres, 0 = off)
    bool tableHudFlat = true;           // HUD lies flat on the table around the diorama, facing up
    bool tableWheelAtLink = true;       // the item wheel appears around Link in the diorama
    float tableWheelWidth = 0.45f;      // width of the HUD panel while the item wheel is open (metres)
    bool tableWheelPause = false;       // the item wheel pauses the game (off: quick switching, world keeps going)
    bool showDuskUi = true;             // Dusklight's own UI (settings, mod manager) on a panel in the headset
    int hookLevel = 4;                  // bring-up switch: how much of the hook set to install (see render_hooks.cpp)

    bool mirrorHud = true;      // composite the HUD onto the desktop mirror
    bool simulateHmd = false;    // debug: run the stereo pipeline without a headset
    float renderScale = 1.0f;    // XR swapchain size multiplier (relative to the runtime's recommendation)
};

const Config& config();

// Registers the ConfigService vars. Call once from mod_initialize.
bool register_config();
// Refresh the cached values from ConfigService (cheap; call once per frame on the game thread).
void refresh_config();

// Presets: each sets a mode and the settings that matter for it (persisted like manual changes).
size_t preset_count();
const char* preset_name(size_t index);
void apply_preset(size_t index);

} // namespace vr
