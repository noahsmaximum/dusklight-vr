#pragma once

#include <cstdint>

// Tabletop x-ray: keeps Link visible through whatever stands between the viewer and him, and fades
// out geometry close to the viewer's head.
//
// It has to happen while the scene is drawn (an occluder has already overwritten Link by the time
// the frame is finished), so the mod adds a cut-out to Aurora's generated GX shaders. The shaders
// are rewritten as Dawn creates them: one fog type the game never uses (orthographic reverse-exp2)
// is replaced by the cut-out. In tabletop, fog is off anyway, so every 3D draw of an eye is given
// that fog type, and its fog parameters carry where this eye's x-ray data sits in Aurora's storage
// buffer (which GX fragment shaders can read).
namespace vr::xray {

// The fog type that selects the cut-out.
constexpr int kFogType = 0x0F; // GX_FOG_ORTHO_REVEXP2

// Installs the shader rewrite. Without it the x-ray is unavailable (tabletop still works).
bool install();
void uninstall();
bool available();

// One eye's x-ray, in game world units.
struct EyeParams {
    float worldFromClip[16]; // column-major, clip space as the GPU sees it (see cut_params)
    float eye[3];            // eye position
    float target[3];         // Link's centre
    float targetWidth;       // render target size in pixels; other targets (shadow maps) are left alone
    float targetHeight;
    float radius;            // x-ray radius around Link
    float taper;             // the cylinder closes over this distance in front of Link
    float margin;            // nothing closer than this in front of Link is cut
    float fadeNear;          // geometry closer to the eye than this is gone...
    float fadeFar;           // ...and fully visible from here (fadeFar <= fadeNear: no fade)
};

// The fog parameters that point a shader at pushed eye data.
struct FogArgs {
    float startZ, endZ, nearZ, farZ;
};

// Game thread, while recording: uploads `p` to this frame's storage buffer. False if the upload
// failed (the eye then draws without x-ray).
bool push_eye(const EyeParams& p, FogArgs& out);

} // namespace vr::xray
