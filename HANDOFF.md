# Dusklight VR — handoff

Repo: https://github.com/noahsmaximum/dusklight-vr (local: `C:\Users\Noah\Projects\dusklight-vr`)
Latest release: **v1.0.0** (Windows + Quest 3; published by CI on tag push, the Quest APK is built by
the manual "Android VR APK" workflow from the tag and attached with `gh release upload`).

## Status

`main` is at v1.0.0 (the 1.0 work from `tabletop-xray` merged); start new work on a branch off `main`.

Windows — user-tested in the headset (v0.3.0 re-checked through Virtual Desktop):

| Area | State |
| --- | --- |
| Per-eye stereo, 6DOF, level horizon | Working |
| HUD quad layer, menus, cinema screen | Working |
| Tabletop mode (diorama + see-through) | Working; VDXR offers no passthrough, so the background uses the chroma-key colour |
| Water / reflections in VR | Fixed (see below); the Ordon Village river fix (post-transform texture matrices) is not yet confirmed in the headset |
| Presets, colour picker, table X/rotation | Working |
| Dusklight's own menus on a panel in VR | Working |
| Frame rate | 45 fps at 3584x2688 per eye on VDXR; GPU-bound. Render scale is the lever |

Quest 3 (standalone, experimental) — **see `docs/android.md`, the live document**: stereo, cinema and
the UI panel work on device; water confirmed by the user. Stereo 36 fps at 60% render scale (Android
default), cinema mostly 72, tabletop ~22 with working passthrough. Untested: pause/resume. The launch menu shows in the headset from
the first launch (VR edition patch section 5 loads mods before it). Next big item: SpaceWarp (`XR_FB_space_warp`) with camera-motion vectors
from per-eye depth, to display 72 while rendering 36.

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
- `tools/run_test.ps1`: launches testgame with the built mod in Ordon Village by the river (`--stage F_SP103,0,5,-1`; point 0 is the ranch path and Link walks straight back out), default
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
- **Water / refraction** (all modes). Three causes of the "portal" water:
  1. `drawDepth2` (the DOF pass) also makes the framebuffer copy the water samples. Never skip it; DOF
     is turned off through the `game.depthOfFieldMode=0` override instead.
  2. View-dependent texture matrices are computed before the painter, using the game view:
     - frame interpolation `callbacks_run` (`calcMaterial` for recorded models; on Android it is inlined
       away, so `interp_mirror` keeps a copy of the list from `add_interpolation_callback`)
     - `dKy_bg_MAxx_proc` for map water (map models are recorded per frame; after it runs, `calcMaterial` + `diff` re-patch the display lists).
     Both are re-run per eye with `j3dSys` set to the eye view. The camera's `widezoom_correction` callback is skipped during the re-run.
  3. The effect matrices project with the game FOV. `view_class` fovy/aspect stay the game's during the eye loop, and:
     - `J3DTexMtx::calcTexMtx` pre/post (modes 3/9) patches effect x Pg^-1 x Pe.
     - `J3DTexMtx::calcPostTexMtx` gets the same patch: models flagged for post-transform texture
       matrices (flag 0x20) never go through `calcTexMtx` (this was the Ordon Village river).
     - `C_MTXLightPerspective` (direct GX users: particles, rain effects) is rewritten from Pe, except during the re-run.
  Verified in simulation at Ordon Spring (`--stage F_SP104,1,0,-1`), Stereo and Tabletop.

- **Dusklight UI in the headset**: post-hook `aurora::rmlui::record_frame` (drew anything?; its return type
  mirror must be non-trivial like Aurora's, or arm64 returns it in registers and the callee writes through
  a stale pointer) + the render target, taken from `WebGPURenderInterface::BeginFrame`'s argument (the
  `aurora::rmlui::s_renderTarget` static has no symbol on Android; layout mirrored as `RmlRenderTarget`). The view is AddRef'd
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

## Tabletop x-ray (working in desktop simulation, branch `tabletop-xray`, 2026-09-24)

User's spec: in **tabletop mode** Link stays visible at all times: a **long cylinder of clear view from
the camera (eye) to Link** (whatever is inside it, in front of him, is removed), and objects near the
viewer's head fade out.

How it works (no Aurora/Dusklight changes): a per-fragment cut-out inside Aurora's own GX shaders,
because an occluder has already overwritten Link by the time the frame is done.
- `src/xray.cpp` intercepts `wgpuDeviceCreateShaderModule` (Windows: import-table patch of
  `webgpu_dawn.dll`; Android: symbol replace hook). GX shaders (label "GX Shader") with fog type
  `GX_FOG_ORTHO_REVEXP` (0x0E, unused by the game) get the fog colour blend replaced by the cut-out
  WGSL: reads the eye's data from `abuf` (Aurora's storage buffer), rebuilds the world position from
  `in.pos` + depth, cylinder test + near fade, 4x4 ordered-dither `discard`. Only perspective draws
  (`ubuf.proj[3].w == 0`) into a target of the eye's size are touched.
- Per tabletop eye, `begin_xray()` pushes 8 x vec4 (worldFromClip, eye pos, Link pos, target size,
  radius/taper/fade/margin) with `push_storage`. `start_xray()` at `GFX_STAGE_SCENE_BEGIN` calls
  `GXSetFog(0x0E, ...)` whose a/c carry the data offset (a = 1 + (offset16 >> 10), c = 2048 + low 10
  bits; GXSetFog zeroes both when farZ == nearZ, hence the +1). `end_xray()` at `FRAME_BEFORE_HUD`
  sends an end marker (NONE, c = -3).
- **Most world materials set fog from display lists, not GXSetFog**, so the x-ray fog is enforced
  at Aurora's register decoders: pre-hooks on
  `extern/aurora/lib/gx/regs.cpp#aurora::gx::fifo::bp_fog0` / `bp_fog3` (resolved via the manifest
  alias) see our begin marker, then replace every fog register write until the end marker. They run
  in GX command order. Note `handle_bp` skips writes equal to the last (original) value.
- Config: `tableXray` (on), `tableXrayRadius` (200 game units), `tableFadeNearCm` (20); panel
  entries under Tabletop.
- Debug: env `DUSKLIGHT_VR_XRAY_DEBUG=1` paints instead of cutting (magenta = would be cut, yellow =
  the eye-to-Link line where it meets geometry); `=1s` shows each perspective draw's viewport size.

Status: works in both eyes in the Windows desktop simulation (cut region on the roof between the
viewer and Link; near trees dither away). Android build compiles, untested on the Quest (check the
log for "Tabletop x-ray unavailable"). Next: headset test, tune radius/taper/margin and the dither
look, Quest performance (every tabletop draw now has a `discard`).

## Tabletop HUD, quick item wheel, aim lines, Hawkeye (branch `tabletop-xray`, 2026-09-24)

- X-ray gating: the x-ray fog is on only inside the map's draw lists (`dComIfGd_drawOpaListBG`,
  `DarkBG`, `Middle`, `drawXluListBG`, `XluListDarkBG`, pre/post hooks): actors (NPCs, enemies,
  Link) are never cut. Above Link's head (+10) the full radius applies (roofs without collision).
- `src/game_tweaks.cpp`: black fades in VR (`JUTFader::draw`, `darwFilter`), manual camera
  (`manualCamera`: `game.freeCamera` forced on, `dCamera_c::freeCamera` enters manual mode at once),
  third-person aiming in stereo (`checkPlayerNoDraw` cleared while aiming from the subject view;
  render_hooks pulls the stereo view back 230 / up 40 / right 45, eased).

- X-ray strength: `link_coverage()` casts 15 camera line checks (5 heights x 3 across) from the eye
  to Link, eased (~80 ms). Alpha-tested shaders (leaves) always use the full radius (collision rays
  can't see them). Nothing within 50 units of Link's body is cut; nothing below his feet + 20.
- Table HUD: flat on the table facing up (`tableHudFlat`, `tableHudWidthCm` 150).
- Item wheel (`src/item_wheel.cpp`): quick mode (default in tabletop, `tableWheelPause` off) skips the
  menu capture that pauses the game, raises the pause flag only around `dMw_c::_draw` (the wheel is
  only queued while paused), and remaps the pad: the menu window sees the C-stick as its move stick,
  Link keeps the move stick (no buttons / C-stick), the camera gets neither. The HUD panel moves to
  Link (`tableWheelAtLink`, `tableWheelWidthCm` 100), offset so the ring centre (247, 217 of the
  608x448 2D screen) is on him; `g_ringHIO` is swapped around `dMenu_Ring_c::_draw` (no backdrop,
  item name lower, guides stacked under the button cluster).
- Aim lines (`update_aim` / `push_aim_line`, `gpu::push_line`): while `mSight` draws, a dashed glowing
  ribbon from the item to the aim point per eye (bow yellow, slingshot brown, clawshot red, dominion
  rod teal, boomerang white); the reticle (`daAlink_sight_c::draw`) is skipped in the eye passes and
  `game.aimingReticle` is forced on in memory so the bow reports its sight.
- Hawkeye (player status0 0x200000): stereo/tabletop render the game camera (zoom included) per eye,
  shown as a 3D screen: two quad layers on the eye swapchains with LEFT/RIGHT eye visibility at the
  cinema screen pose (`xr::arm_submit(..., stereoScreen)`).
- Settings: tabbed VR window (`open_window` in mod.cpp) from the top bar "VR" entry or the Mods panel.
- Dev switches: `DUSKLIGHT_VR_TEST_WHEEL=<s>` opens the wheel, `DUSKLIGHT_VR_WHEEL_DUMP=<file>` dumps
  its pane positions, `DUSKLIGHT_VR_TEST_AIM` draws a test aim line, `DUSKLIGHT_VR_TEST_HAWK` forces
  the Hawkeye view, `DUSKLIGHT_VR_XRAY_DEBUG=1` paints the x-ray instead of cutting. The test
  harness kills the game before its log is flushed: write measurements to a file instead.

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

1. State: v1.0.0 released (Windows + Quest 3) from `main`: tabletop x-ray, table HUD, quick wheel,
   aim lines and lock-on markers, Hawkeye screen, third-person aiming, manual camera, black fades,
   stereo shadows (sections above). Work on a feature branch off `main`, PR/merge back.
2. Quest: the 1.0 features are Windows-tested only; check them on the headset (x-ray cost with a
   `discard` in every tabletop draw).
3. Windows test: `tools/run_test.ps1` (desktop simulation; `-Headset` for Virtual Desktop). Quest:
   `docs/android.md` device loop; SDK/adb on `F:\Android\sdk`, ROM at
   `/storage/emulated/0/Download/tp-linkle.iso`, app `dev.twilitrealm.dusk.vr`.
4. Still unconfirmed: the Ordon Village river in the Windows headset; Quest pause/resume.
