#pragma once

#include "mods/svc/gfx.h"

#include <webgpu/webgpu.h>

// WebGPU passes the mod records itself:
//   - blit:    copy a resolved game frame into an XR render target
//   - combine: rebuild a premultiplied-alpha HUD from the 2D layer rendered over black and white
//   - mirror:  desktop presentation (HUD over the right eye, or side-by-side in simulation)
namespace vr::gpu {

bool initialize(WGPUDevice device);
void shutdown();

// Render-worker side (inside a GfxService compute callback). `dst` must be target_format views.
void blit(WGPUCommandEncoder encoder, WGPUTextureView src, WGPUTextureView dst, WGPUTextureFormat dstFormat);
void combine_hud(WGPUCommandEncoder encoder, WGPUTextureView black, WGPUTextureView white, WGPUTextureView dst,
    WGPUTextureFormat dstFormat);

struct MirrorPayload {
    WGPUTextureView sceneLeft = nullptr;  // side-by-side when both eyes are set
    WGPUTextureView sceneRight = nullptr;
    WGPUTextureView hudBlack = nullptr;   // HUD over black
    WGPUTextureView hudWhite = nullptr;   // HUD over white
};

// Game thread: queue the desktop mirror draw into the current EFB pass.
void push_mirror(const MirrorPayload& payload);
// Game thread: copy a resolved scene snapshot back into the current EFB pass (opaque).
void push_restore(WGPUTextureView scene);
// Game thread: clear colour (black or white) and depth of the current EFB pass.
void push_clear(bool white = false);

} // namespace vr::gpu
