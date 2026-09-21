#pragma once

#include "dawn_d3d12.hpp"
#include "vr_math.hpp"

#include <cstdint>
#include <string>

// OpenXR session + frame loop.
//
// Threading model (Dusklight records GX on the game thread and encodes/submits on a render
// worker thread):
//   game thread   : poll()/wait_frame() at the top of each frame (xrWaitFrame paces the game to
//                   the headset), locate_views() right before rendering the eyes.
//   render worker : begin_frame() when the worker starts encoding the frame (xrBeginFrame),
//                   arm_submit() once the eye/quad targets are written, and on_queue_submitted()
//                   right after the frame's wgpuQueueSubmit, which copies the targets into the XR
//                   swapchains on Dawn's own D3D12 queue and calls xrEndFrame.
namespace vr::xr {

struct EyeView {
    Pose pose; // eye pose in the (recentred) app space, metres
    Fov fov;
};

enum class QuadSpace : uint8_t {
    App,  // world-locked
    View, // head-locked
};

struct QuadLayer {
    bool enabled = false;
    QuadSpace space = QuadSpace::App;
    Pose pose;
    float width = 1.0f; // metres
    float height = 1.0f;
    bool premultipliedAlpha = true; // false = opaque
};

struct FrameInfo {
    uint64_t id = 0;
    bool shouldRender = false;
    bool viewsValid = false;
    EyeView views[2];
};

// Lifecycle ---------------------------------------------------------------------------------------

// Creates the XrInstance (no headset needed). Returns false if no OpenXR runtime is installed.
bool initialize(WGPUDevice device, WGPUAdapter adapter);
void shutdown();

// Game thread, once per frame before rendering: pumps events, (re)creates the session when a
// headset appears, and applies recentre requests.
void poll();

bool instance_ready();
bool session_running();
std::string status();
void request_recenter();

// Frame loop ------------------------------------------------------------------------------------

// Game thread. Blocks in xrWaitFrame. Returns an XR frame id (0 when no XR frame is active).
uint64_t wait_frame();
// Game thread. Locates the eye views for frame `id` at its predicted display time.
bool locate_views(uint64_t id, FrameInfo& out);
// Game thread fallback when the frame never reaches the renderer (aurora skipped it).
void abandon_frame(uint64_t id);

// Render worker.
void begin_frame(uint64_t id);
// quadToken identifies the quad resources the frame was rendered into (quad_token()).
void arm_submit(uint64_t id, bool stereo, const QuadLayer& quad, void* quadToken);
void on_queue_submitted();

// Render targets (Dawn textures backed by capturable D3D12 resources) ------------------------------

uint32_t eye_width();
uint32_t eye_height();
WGPUTextureFormat target_format();
// Game-thread snapshot of the render targets for this frame (views are borrowed).
WGPUTextureView eye_target_view(int eye);
// Ensures the quad target exists with the given size (game thread). Returns its view or null.
WGPUTextureView ensure_quad_target(uint32_t width, uint32_t height);
void* quad_token();

// Debug: fake stereo views used when no headset is present (simulateHmd).
void simulated_views(FrameInfo& out);
// Debug: create eye targets without a session so simulation exercises capture + composition.
bool ensure_simulation_targets(uint32_t width, uint32_t height);
// Debug: once, copy the left simulation target out through D3D12 right after a frame submit.
void simulation_readback_once();

} // namespace vr::xr
