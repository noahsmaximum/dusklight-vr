# Dusklight VR — handoff

Repo: https://github.com/noahsmaximum/dusklight-vr (local: `C:\Users\Noah\Projects\dusklight-vr`)
Latest release: **v0.2.0** (published by CI on tag push; tags with a `-` suffix could be marked prerelease by hand).

## Status

Windows (`main`, released **v0.2.0**) — all user-tested in the headset:

| Area | State |
| --- | --- |
| Per-eye stereo, 6DOF, level horizon | Working |
| HUD quad layer, menus, cinema screen | Working |
| Tabletop mode (diorama + see-through) | Working; VDXR offers no passthrough, so the background uses the chroma-key colour |
| Water / reflections in VR | Fixed (three separate causes, see below) |
| Presets, colour picker, table X/rotation | Working |
| Dusklight's own menus on a panel in VR | Working |
| Frame rate | 45 fps at 3584x2688 per eye on VDXR; GPU-bound. Render scale, or matching the eye aspect instead of 4:3, are the levers |

Android / Quest 3 (`vr-shared`, in progress) — **see `docs/android.md`, that is the live document**:
the session, the shared-buffer handoff and the mod load all work on device; the game segfaults in
the first frame with any hook installed, and runs (audio, no image) with none. A bisect switch
(`minimalHooks` cvar) is in place; next step is a `hookLevel` cvar to find the culprit.

Windows regression risk from the Android work: `vr-shared` contains the fix for a real bug the
refactor introduced — `xrCreateSession` was called without a system id, which breaks VR on Windows
too (simulation never creates a session, so it went unnoticed). **`vr-shared` has not been tried in
the Windows headset yet**; do that before merging.

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
- `run_test.ps1` sets `DUSKLIGHT_VR_NO_XR=1` so the harness never opens a session in a connected headset;
  pass `-Headset` to allow it. Ordon Spring (`-Stage "F_SP104,1,0,-1"`) is a good water test spot.
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

- **VDXR (Virtual Desktop) reports neither XR_FB_passthrough nor ALPHA_BLEND** (confirmed 2026-09-21), so the
  transparent area is filled with the "Background without passthrough" colour (black / green / magenta)
  for chroma-key passthrough tools.
- **Water / refraction** (all modes). Three separate causes of the "portal" water:
  1. `drawDepth2` (the DOF pass) also makes the framebuffer copy the water samples. Never skip it; DOF
     is turned off through the `game.depthOfFieldMode=0` override instead.
  2. View-dependent texture matrices are computed before the painter, using the game view:
     - frame interpolation `callbacks_run` (`calcMaterial` for recorded models)
     - `dKy_bg_MAxx_proc` for map water (map models are recorded per frame; after it runs, `calcMaterial` + `diff` re-patch the display lists).
     Both are re-run per eye with `j3dSys` set to the eye view. The camera's `widezoom_correction` callback is skipped during the re-run.
  3. The effect matrices project with the game FOV. `view_class` fovy/aspect stay the game's during the eye loop, and:
     - `J3DTexMtx::calcTexMtx` pre/post (modes 3/9) patches effect x Pg^-1 x Pe.
     - `C_MTXLightPerspective` (direct GX users: particles, rain effects) is rewritten from Pe, except during the re-run.
  Verified in simulation at Ordon Spring (`--stage F_SP104,1,0,-1`), Stereo and Tabletop.

- **Dusklight UI in the headset**: post-hook `aurora::rmlui::record_frame` (drew anything?) + the
  `aurora::rmlui::s_renderTarget` data symbol (layout mirrored as `RmlRenderTarget`). The view is AddRef'd
  on the game thread and released by the worker; it is blitted premultiplied into quad slot `kQuadUi`, world-locked
  in front of the head when UI opens (1 frame late: RmlUi renders after our composite is queued).
  Menus with a backdrop blur make RmlUi render the whole scene as base layer; during a session a pre-hook on
  `aurora::rmlui::WebGPURenderInterface::BeginFrame` forces `BaseLayerContent::Transparent` and the
  `record_frame` post-hook flips `overlay` to true so the desktop still composites it over the scene
  (`context_has_visible_backdrop_filter` and its helper are inlined; not hookable).
- **Presets** (`vr_config.cpp` `kPresets`): buttons that set the mode plus its settings. **Key colour** is a
  string var `tableKeyColorHex` (RRGGBB) bound to `UI_CONTROL_COLOR`, uploaded to a 1x1 texture for `fs_key`.
  **Table X / rotation**: `tableOffsetXCm`, `tableYawDeg` (added to the camera-follow yaw).

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

## Cross-platform layout (since the Android work)

- `src/interop.hpp` is the graphics-handoff interface; `interop_d3d12.cpp` (Windows) and
  `interop_vulkan.cpp` (Android) implement it, chosen by CMake. `xr_runtime.cpp` is API-agnostic.
- Frame delivery hooks `aurora::gfx::after_submit` on every platform; the Windows import-table hook
  on `wgpuQueueSubmit` is only a fallback.
- Android extras: `src/android_loader.cpp` (OpenXR loader init via JNI),
  `android/patch-dusklight.sh` (VR-edition build), `.github/workflows/android-apk.yml`.
- `tools/run_test.ps1` sets `DUSKLIGHT_VR_NO_XR=1`, which now skips only the OpenXR instance, so
  simulation still exercises the whole GPU copy path.

## Picking this up again

1. Read `docs/android.md` (device loop, bisect state, known gaps).
2. The Quest and the SDK are on this machine: `F:\Android\sdk` (adb in `platform-tools`), ROM already
   at `/storage/emulated/0/Download/tp-linkle.iso`, app installed as `dev.twilitrealm.dusk.vr`.
3. Finish the hook bisect, then strip the temporary bring-up code listed in `docs/android.md`.
4. Before merging `vr-shared`: test it on the Windows headset (it carries the session-id fix).
