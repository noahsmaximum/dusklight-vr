# Dusklight VR — handoff

Repo: https://github.com/noahsmaximum/dusklight-vr (local: `C:\Users\Noah\Projects\dusklight-vr`)
Latest release: **v0.1.0-beta** (prerelease, published by CI on tag push).

## Status

| Area | State |
| --- | --- |
| Per-eye stereo, 6DOF, level horizon | Working in headset (user-tested on Virtual Desktop) |
| HUD quad layer | Fixed (was shrunk into a corner); user reports "looking good" |
| Headset render size (4:3 at eye height) | Shipped; 31 fps report was before this. **Waiting on the user's panel stats line** (fps / xrWaitFrame / both-eyes ms / render size) to judge GPU vs pacing |
| Cinema mode, recenter, menu lock | Implemented, not explicitly confirmed in headset |
| **Tabletop mode** | Implemented on branch `tabletop`; verified in simulation (diorama, cut, alpha); **not yet tried in a headset** (see checklist below) |

Runtime on the user's machine: Virtual Desktop (VDXR). It recommends 2688 px tall per eye, so the
mod renders 3584x2688 twice per frame; "Render scale" in the panel lowers it (takes effect when
the session restarts).

## Architecture (all through mod services/hooks — no patched Dusklight/Aurora)

- **Stereo**: replace-hook `mDoGph_Painter`, run it once per eye with the camera `view_class`
  rewritten (eye pose × game view, headset off-axis frustum). Works because Dusklight 2.0's J3D
  concatenates view×model at paint time (frame-interpolation presentation path).
  Between eyes: clear colour+depth (`gpu::push_clear`), freeze `dusk::game_clock::original_frames`,
  skip ImGui Pre/PostDraw, skip motion blur (and DOF by option), skip letterbox trimming.
- **Culling**: `mDoLib_clipper::setup` widened to `cullFov`.
- **HUD**: hook `dusk::mods::gfx_run_stage`. At `FRAME_BEFORE_HUD` resolve the scene and clear the EFB
  black (eye 1) / white (eye 2); at `FRAME_AFTER_HUD` resolve the HUD and restore the scene. The pair
  gives exact premultiplied alpha → OpenXR quad layer. Do **not** use `create_pass` offscreen passes
  for game 2D: Aurora maps the logical 2D viewport 1:1 there (that was the corner bug).
- **Render size**: post-hook `aurora::window::get_window_size` forces fb to 4:3 at eye height during
  a session, applied via `aurora::webgpu::refresh_surface(false)`.
- **GPU handoff**: Dawn's D3D12 device/queue from `webgpu_dawn.dll` exports. The host device lacks
  SharedTextureMemory features, so the `ID3D12Resource` behind our Dawn eye/quad textures is captured
  by vtable-patching `ID3D12Device::Create*Resource` during `wgpuDeviceCreateTexture`
  (`dawn_d3d12.cpp`). A GfxService compute callback composites on the render worker; an IAT hook on
  the exe's `wgpuQueueSubmit` then copies into the XR swapchains on Dawn's queue and calls
  `xrEndFrame` (`xr_runtime.cpp`).
- **Frame loop**: `xrWaitFrame` in `aurora_begin_frame` pre-hook; `xrBeginFrame` in a compute
  callback pushed right after it (runs when the worker starts the frame); `xrEndFrame` after submit.
- **Overrides** (in-memory via `dusk::config::load_arg_override`, never saved): frame interpolation
  Unlimited, vsync off, letterbox off, mirror mode off — applied once a session runs.

Files: `render_hooks.cpp` (game hooks, per-eye loop, quad placement, timing), `xr_runtime.cpp`
(OpenXR session/swapchains/copies), `dawn_d3d12.cpp` (+`d3d12_vtbl.c`), `gpu.cpp` (WGSL blit /
HUD combine / clear / restore / mirror), `vr_config*` (settings), `mod.cpp` (panel, lifecycle).

## Building / testing

```
cmake -B build -G Ninja -DDUSKLIGHT_DIR=<repo>/ref/dusklight-2.0 -DDUSK_GAME_EXE=<repo>/testgame/sdk/windows-amd64.lib
cmake --build build
```
(needs VS BuildTools vcvarsall x64; CI fetches Dusklight v2.0.0 itself.)

- `ref/` (gitignored): old `vr-port` dusklight + `openxr-vr-spike` aurora clones, Dusklight v2.0.0
  source with aurora submodule, mod-template.
- `testgame/` (gitignored): official v2.0.0 Windows build in **portable mode**
  (`data_location.json`), so the user's `%APPDATA%` config is never touched.
- `tools/run_test.ps1`: launches testgame with the built mod at Ordon (`--stage F_SP103`), default
  cvar `simulateHmd=true` (side-by-side stereo without a headset), captures the window with
  PrintWindow. **If Virtual Desktop is connected it starts a real session in the user's headset.**
- Simulation also runs a one-shot D3D12 readback self-test (logs a centre pixel ~10 s in).
- Kill `dusklight.exe` before rebuilding (it locks the `.dusk`).
- Include game headers before anything pulling in `windows.h` (`IN` macro clashes with game enums).
- Don't write repo files with PowerShell 5 `Set-Content -Encoding utf8`: it adds a BOM and symgen
  rejects `mod.json`.

## Releasing

Bump `MOD_VERSION` in `CMakeLists.txt` (and `project(VERSION)` numerically), update
`.github/release-notes.md` (install guide first, short bullets), commit, tag `vX.Y.Z[-beta]`, push
the tag. CI builds Windows x64, merges, and publishes the release with those notes.

## Tabletop mode (branch `tabletop`)

The world is a small diorama on a real table, and the sky is see-through.

How it works (see README "How it works" for the one-line version):

- **Camera** (`render_hooks.cpp` `table_transform`): trackingFromWorld =
  `T(tablePos*s) * RotY(align) * T(-anchor)`, eye view = inverse(eye pose scaled by `s`) x that.
  `s` = `tableScale` x 100 units/m (1:50 by default, Link about 3 cm). The anchor follows the player
  (`dComIfGp_getPlayer(0)->current.pos`) with a dead zone (10% of the radius) and exponential
  smoothing. Y follows more slowly. It snaps on a warp (more than 4 radii away). `align` = -(game-camera
  yaw), smoothed. `tableFollowYaw` off freezes it.
- **Culling**: pre-hooks on both `J3DUClipper::clip` overloads, using MSVC decorated names, return
  "visible" while tabletop runs (headset session or simulation).
- **Sky / fog**: `dComIfGd_draw{Opa,Xlu}ListSky` are skipped during the eye loop. `GXSetFog` is
  forced to `GX_FOG_NONE`, because the eye is thousands of units away and everything would fog over.
- **Cut** (`gpu.cpp` `fs_cut`):
  - At `FRAME_BEFORE_HUD`: `resolve_pass(depth)` (R32Float), then `push_cut`.
  - `push_cut` rebuilds the world position from depth with inverse(P' x V). P' is the projection as
    Aurora uploads it: reversed Z negates row 2.
  - Mask = radial smoothstep (8% rim) x floor smoothstep (`tableDepth` below the anchor). Pixels with
    cleared depth get 0.
  - Blend: colour = dst x mask, alpha = mask. The EFB then holds premultiplied colour + alpha.
  - The HUD restore and the worker blit keep alpha in tabletop (`BlitAlpha`).
- **See-through** (`xr_runtime.cpp`):
  - `XR_FB_passthrough` is enabled on the instance when offered. The passthrough and its
    reconstruction layer are created in `poll()` while tabletop plus the passthrough option are on,
    and submitted under the projection layer.
  - Otherwise `XR_ENVIRONMENT_BLEND_MODE_ALPHA_BLEND` if the system lists it, otherwise opaque (black
    around the table).
  - The projection layer gets `BLEND_TEXTURE_SOURCE_ALPHA`. The panel status shows which path is active.
- **Simulation**: the fake head pitches 40 degrees down in tabletop so the table is in view.

**Headset test checklist:**
- The table sits roughly on a real surface. Adjust height/distance, then Recenter.
- The room shows around the diorama. Check the status line for "see-through: passthrough / alpha blend". VD may
  need passthrough enabled in its settings.
- Walking Link around: the table glides and follows, with no jitter. Camera swings turn the diorama.
- Big areas (Hyrule Field) perform OK with culling off.
- Screen fades and loading screens: check that alpha isn't broken by the fader.
- Cutscenes, interiors (ceilings may cover the view; there is no roof cut yet), first-person aiming.

**Ideas not done yet:** roof/ceiling cut in interiors (a height cap), a table rim or base, grab-to-move
the table with controllers, a sphere-based clip test instead of culling nothing.
