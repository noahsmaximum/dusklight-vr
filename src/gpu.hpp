#pragma once

#include "mods/svc/gfx.h"

#include <webgpu/webgpu.h>

// WebGPU passes the mod records itself:
//   - blit:    copy a resolved game frame into an XR render target
//   - combine: rebuild a premultiplied-alpha HUD from the 2D layer rendered over black and white
//   - mirror:  desktop presentation (HUD over the right eye, or side-by-side in simulation)
//   - cut:     tabletop mode; fades the scene out (premultiplied alpha) beyond the table
namespace vr::gpu {

bool initialize(WGPUDevice device);
void shutdown();

// True when the scene depth buffer uses reversed Z (1.0 = near).
bool reversed_z();

// Render-worker side (inside a GfxService compute callback). `dst` must be target_format views.
enum class BlitMode {
    Opaque,     // alpha forced to 1
    KeepAlpha,  // tabletop with a see-through runtime (premultiplied, black where transparent)
    KeyGreen,   // tabletop, transparent areas filled with a chroma-key colour
    KeyMagenta,
};
void blit(WGPUCommandEncoder encoder, WGPUTextureView src, WGPUTextureView dst, WGPUTextureFormat dstFormat,
    BlitMode mode = BlitMode::Opaque);
void combine_hud(WGPUCommandEncoder encoder, WGPUTextureView black, WGPUTextureView white, WGPUTextureView dst,
    WGPUTextureFormat dstFormat);

struct MirrorPayload {
    WGPUTextureView sceneLeft = nullptr;  // side-by-side when both eyes are set
    WGPUTextureView sceneRight = nullptr;
    WGPUTextureView hudBlack = nullptr;   // HUD over black
    WGPUTextureView hudWhite = nullptr;   // HUD over white
};

// Uniforms of the tabletop cut (WGSL layout: mat4x4f + 2 x vec4f).
struct CutParams {
    float worldFromClip[16]{}; // column-major (WGSL), clip space as the GPU sees it
    float anchor[4]{};         // xyz = table centre in world units, w = radius (world units)
    float params[4]{};         // x = radial feather, y = floor height, z = floor feather, w = far depth value
};

// Game thread: queue the desktop mirror draw into the current EFB pass.
void push_mirror(const MirrorPayload& payload);
// Game thread: copy a resolved scene snapshot back into the current EFB pass.
void push_restore(WGPUTextureView scene, bool keepAlpha = false);
// Game thread: clear colour (black or white) and depth of the current EFB pass.
void push_clear(bool white = false);
// Game thread: multiply the current EFB colour by the table mask computed from `depth` (a
// resolve_pass depth snapshot of the same pass) and write the mask into alpha.
void push_cut(WGPUTextureView depth, const CutParams& params);

} // namespace vr::gpu
