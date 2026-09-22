# Dusklight VR

OpenXR VR for [Dusklight](https://github.com/TwilitRealm/dusklight) 2.0 as a self-contained native mod
(`.dusk`). No patched Dusklight or Aurora build: drop the mod in and it works with the official release.

- **True per-eye stereo.** Each eye is rendered by the game itself with its own camera, so water, shadows,
  billboards and screen effects are correct in both eyes (the old render-twice/uniform-patching approach
  could not fix screen-space reflections).
- **6DOF head tracking** on top of the game camera, with a **level-horizon** comfort mode that removes the
  game camera's pitch and roll.
- **HUD, text and menus on an OpenXR quad layer**: crisp, composited by the runtime, never clipped.
  The HUD follows your head lazily; pause menus lock in place in front of you; 2D-only screens (title,
  file select) become a large virtual screen.
- **Cinema mode**: the flat game on a big world-locked screen.
- **Tabletop mode**: the world as a small diorama on your real table (scale 1:50 by default), with
  the sky see-through so your room shows around it (passthrough, where the runtime supports it).
- Frames are paced by the headset (`xrWaitFrame`) and rendered through Dusklight's frame interpolation,
  so every headset refresh gets a fresh frame instead of the game's 30 Hz simulation rate.

## Requirements

- Windows, Dusklight **v2.0.0**, graphics backend **D3D12** (the default).
- Any OpenXR runtime with D3D12 support (SteamVR, Virtual Desktop, Meta Quest Link, WMR, ...).

## Install

Copy `dusklight_vr.dusk` into `%APPDATA%\TwilitRealm\Dusklight\mods` and enable it in the Mods window.
Settings live in the mod's panel there (mode, world scale, HUD/menu placement, recenter).

While a headset session is running the mod applies these in-memory overrides (never saved to your
config): frame interpolation *Unlimited*, vsync off, letterboxing off, mirror mode off.

## How it works

| Piece | Mechanism |
| --- | --- |
| Stereo | `mDoGph_Painter` is replace-hooked and run once per eye with the camera's `view_class` rewritten (eye pose × game view, headset off-axis frustum). Dusklight's J3D computes view×model at paint time, so the draw lists recorded by the simulation re-render correctly for each eye. Depth is cleared between eyes; UI timers are frozen for the second eye. |
| Culling | `mDoLib_clipper::setup` is widened so turning your head never reveals culled geometry. |
| HUD layer | At `GFX_STAGE_FRAME_BEFORE_HUD` the scene is snapshotted and the main framebuffer cleared to black (eye 1) / white (eye 2); the 2D phase draws over it, the result is snapshotted at `FRAME_AFTER_HUD` and the scene put back. The compositor recovers exact premultiplied alpha from the pair. |
| Tabletop | The eye views are built from a table placement (player at the table centre, game-camera heading pointing away from you, scale 1:N) instead of the game camera. Frustum culling (`J3DUClipper::clip`), the sky lists and fog are switched off. At `FRAME_BEFORE_HUD` each eye's depth is snapshotted and a pass rebuilds world positions from it, fading out everything beyond the table radius / below the table into premultiplied alpha. The runtime shows the room behind it via `XR_FB_passthrough`, or the `ALPHA_BLEND` environment blend mode. |
| GPU handoff | Eye/HUD images are composited on Aurora's render worker (GfxService compute callback) into Dawn textures whose `ID3D12Resource` is captured when they are created. Right after the frame's `wgpuQueueSubmit` (observed via the import table), they are copied into the OpenXR swapchains on Dawn's own D3D12 queue and `xrEndFrame` is called. Dawn's device/queue come from `webgpu_dawn.dll` exports. |
| Frame loop | `xrWaitFrame` at `aurora_begin_frame`, `xrBeginFrame` when the worker starts the frame, `xrEndFrame` after its submit. |

## Building

```sh
cmake -B build -G Ninja
cmake --build build
```

Produces `build/mods/dusklight_vr.dusk`. The OpenXR loader is fetched and linked statically.

For development against a local Dusklight checkout / game install:

```sh
cmake -B build -G Ninja -DDUSKLIGHT_DIR=<dusklight checkout> -DDUSK_GAME_EXE=<game>/sdk/windows-amd64.lib
```

`tools/run_test.ps1` launches a portable test copy of the game with the built mod. The
**Simulate headset** option runs the whole stereo pipeline without an HMD and shows both eyes
side by side on the desktop.

## Known limitations

- Dusklight's own overlay UI (settings, mod manager) only appears on the desktop window.
- Screen-space post effects are computed per eye; depth of field is disabled by default for comfort,
  motion blur is always off in stereo (it would blend in the other eye's frame).
- Pause-menu 3D item models render flat on the menu panel (they are part of the 2D layer).
