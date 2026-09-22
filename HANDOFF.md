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
| **Tabletop mode** | Designed, not started (plan below) |

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

## Next: Tabletop mode (designed, not implemented)

Goal: the world as a small diorama sitting on a real table, see-through sky (passthrough).

1. **Mode/config**: `Mode::Tabletop = 3`; vars `tabletopScale` (units/m, ~1500 → Link ≈ 10 cm),
   `tableHeightCm` (~-55 below eyes at recentre), `tableDistanceCm` (~60), `tableRadiusCm` (~50),
   `tabletopPassthrough`, `followYaw`.
2. **Camera** (`apply_eye`): replace `gameView` with world→tracking
   `T(tablePos·s) · RotY(align) · T(-anchor)`; view = inverse(eye pose scaled by s) × that.
   Anchor = player position (`dComIfGp_getPlayer(0)->current.pos`) smoothed with a deadzone; align =
   game-camera yaw (smoothed) so stick-up moves Link away from the player.
3. **Culling**: behind-the-game-camera geometry must still draw. Hook both
   `J3DUClipper::clip` overloads (J3DUClipper.cpp:32 and :63; overloaded → need the mangled names or
   `DEFINE_HOOK` with a `static_cast` member pointer) to return "visible" in tabletop.
4. **Sky**: skip `dComIfGd_drawOpaListSky` / `dComIfGd_drawXluListSky` (NOINLINE wrappers, hookable).
5. **Transparency**: take the depth snapshot at `FRAME_BEFORE_HUD` (`resolve_pass` with
   `depth=true`, R32Float — use `textureLoad`, not filterable; the HUD clear later wipes depth). New
   blit variant: alpha = 0 where depth is far (reversed-Z: 0), otherwise reconstruct view-space z from
   depth with the eye's projection (reversed: `zv = m23 / (d - m22)`; standard:
   `zv = -m23 / (d + m22 - 1)`), rebuild the world position via the inverse view, and feather alpha to
   0 beyond `tableRadius` from the anchor (XZ). Output premultiplied. Per-eye params in a small
   uniform buffer written with `wgpuQueueWriteBuffer` in the compute callback.
6. **Passthrough** (`xr_runtime`): enable `XR_FB_passthrough` at instance creation if offered →
   create passthrough + reconstruction layer, submit `XrCompositionLayerPassthroughFB` before the
   projection layer; else use `XR_ENVIRONMENT_BLEND_MODE_ALPHA_BLEND` if
   `xrEnumerateEnvironmentBlendModes` lists it; else opaque (black void). Projection layer gets
   `XR_COMPOSITION_LAYER_BLEND_TEXTURE_SOURCE_ALPHA_BIT` in tabletop; `arm_submit` needs a flag.
7. HUD quad stays head-following; recentre re-places the table.
