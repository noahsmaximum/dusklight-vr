# Android (Quest 3, Pico 4) — bring-up in progress

Branch: `vr-shared` (not merged). Windows is unaffected; everything here is additive.

## Where it stands (2026-09-23, tested on a Quest 3)

Working on device:

- The VR-edition app launches immersive, with the mod bundled inside; the mod loads and activates.
- The ROM runs from the headset (`/storage/emulated/0/Download/tp-linkle.iso`).
- `aurora::gfx::after_submit` resolves by name (the export patch works), so frame delivery is hooked.
- **OpenXR session creates on Meta Quest 3, 1680x1760 per eye.**
- **The AHardwareBuffer handoff works**: Dawn imports the shared memory and hands the texture back
  and forth (needs the image-layout handshake, see below).

Blocking bug:

- **With any of our hooks installed the game segfaults during the first frame; with none it runs
  fine** (audio plays, nothing is drawn because the mod never submits a frame). The fault address
  looks like a stack guard page, i.e. a detour that ends up calling itself.
- Bisected so far: game hooks off *and* only the Aurora hooks left still crashes. Zero hooks is
  stable. Next step is to narrow which of `aurora_begin_frame` (pre/post), `aurora_end_frame` (pre),
  `aurora::gfx::after_submit` (post), `rmlui::record_frame` (post) and
  `WebGPURenderInterface::BeginFrame` (pre) is responsible — add a `hookLevel` int cvar (0 none,
  1 aurora frame hooks, 2 +after_submit, 3 +rmlui, 4 +game hooks) and work up.

Other known gaps on Android:

- Hooks declared with MSVC-decorated names don't resolve: `J3DUClipper::clip` (both overloads) and
  `J3DTexMtx::calcTexMtx`. They need Itanium (`_ZN...`) names under `#ifdef __ANDROID__`, which
  costs tabletop culling and the water texgen fix until done.
- "Dusklight UI unavailable in the headset" is logged, so one of the RmlUi lookups fails even though
  Aurora symbols are exported — check which.
- `XR_FB_passthrough` and `ALPHA_BLEND` are both reported unavailable, so tabletop see-through has no
  transparent background yet (may need a Quest passthrough permission in the manifest).
- Each CI run signs the APK with a fresh key, so installs need an uninstall first. Add a fixed
  sideload keystore to the repo.
- Temporary bring-up code to remove when the crash is fixed: `VR_MARK` markers, the painter depth
  counter, the painter pointer log, and the `minimalHooks` cvar with its `gameHooks` gate.

## The device loop (no APK rebuild needed)

The APK ships the mod, but a mod in a sideloaded folder shadows the bundled one, so iteration is:

```bash
export PATH="/f/Android/sdk/platform-tools:$PATH"
cmake --build build-android
adb push build-android/mods/dusklight_vr.dusk /storage/emulated/0/Download/dusk-mods/
adb shell am force-stop dev.twilitrealm.dusk.vr
adb logcat -c
adb shell "am start -n dev.twilitrealm.dusk.vr/dev.twilitrealm.dusk.DuskActivity --es dusk_args \
  '--dvd /storage/emulated/0/Download/tp-linkle.iso --mods /storage/emulated/0/Download/dusk-mods \
   --cvar mod.com_noahsmaximum_dusklight__vr.mode=0 \
   --cvar mod.com_noahsmaximum_dusklight__vr.minimalHooks=true'"
sleep 30
adb logcat -d | grep -iE "noahsmaximum|signal 11|aurora::gpu"
```

Notes:

- `dusk_args` takes the same options as the desktop build; it splits on spaces, so **no spaces or
  commas in paths** (that is why the ROM was renamed).
- The app needs storage permission once:
  `adb shell appops set dev.twilitrealm.dusk.vr MANAGE_EXTERNAL_STORAGE allow`.
- Quest refuses to launch VR apps while the controllers are asleep ("controllers required" dialog).
  The manifest patch now declares hand tracking optional, which should stop that.
- Crashes: `adb logcat -d | grep -A25 "backtrace:"`. Stacks are usually one frame (stack overflow),
  so prefer bisecting with cvars over reading tombstones.
- Quoting: run `am start` through `adb shell "..."` so the device shell keeps the argument string
  together; `MSYS_NO_PATHCONV=1` stops Git Bash rewriting `/storage/...` paths.

## Building

Mod, locally (SDK/NDK live on `F:\Android`, NDK 29.0.14206865 matches CI):

```
cmake -B build-android -G Ninja \
  "-DCMAKE_TOOLCHAIN_FILE=F:/Android/sdk/ndk/29.0.14206865/build/cmake/android.toolchain.cmake" \
  -DANDROID_ABI=arm64-v8a -DANDROID_PLATFORM=android-28 \
  -DDUSKLIGHT_DIR=<repo>/ref/dusklight-2.0
cmake --build build-android
```

CI builds `android-aarch64` next to Windows and merges both into one `.dusk`.

The **Android VR APK** workflow (manual, ~20 min) builds the VR edition: it patches an upstream
Dusklight checkout, builds the mod from a chosen branch (`mod_ref`, use `vr-shared`), bundles it, and
signs the APK. Inputs: `dusklight_ref` (default v2.0.1), `mod_ref` (default main). The workflow file
must exist on `main` to be dispatchable.

## Why a custom Dusklight build

The official Android build blocks the mod twice:

- **The app isn't a VR app.** Its manifest declares only a flat launcher activity, so Quest and Pico
  run it as a 2D panel. A mod can't change the app manifest.
- **Its release build exports nothing we can use.** On Windows the mod reaches Dawn's D3D12 device
  through exported functions and hooks Aurora internals by name. The Android build exports the
  game's own functions and Dawn's public API only — no Aurora internals, no Dawn internals. Dawn's
  shared-texture API *is* exported, but Dusklight never enables those features on its device.

`android/patch-dusklight.sh` applies four additive changes to a fresh checkout:

1. **Manifest**: head tracking, the Khronos `IMMERSIVE_HMD` and Meta `VR` launch categories,
   Quest/Pico metadata, optional hand tracking, and the runtime-broker `queries` entry.
2. **Aurora** (`lib/webgpu/gpu.cpp`): request Dawn's shared-texture/shared-fence features when the
   GPU offers them.
3. **Exports** (`cmake/AndroidExports.cmake`): keep Aurora's symbols (`_ZN6aurora*`) in the generated
   version script, so the mod can hook them by name as it does on Windows.
4. **App identity**: package `dev.twilitrealm.dusk.vr`, label "Dusklight VR", so it installs beside
   the official app and never touches its save data.

Nothing is upstreamed; Dusklight is CC0, so our own build is fine to share.

## How the frame handoff works on Android

- Render targets are `AHardwareBuffer`s, imported into Dusklight's Dawn device through
  `wgpuDeviceImportSharedTextureMemory` and into the Vulkan device OpenXR creates as a `VkImage`.
- Dawn composites into them. **Begin/EndAccess must chain
  `SharedTextureMemoryVkImageLayoutBeginState`/`EndState`** (old/new `VkImageLayout`), or Dawn aborts
  the process. `everUsed` tracks the first access (`initialized = false`, layout `UNDEFINED`).
- `EndAccess` returns sync-fd fences; they are imported as Vulkan semaphores so the copy waits for
  Dawn's work. The copy into the swapchain images runs on the OpenXR device
  (`src/interop_vulkan.cpp`).
- `copy_to_swapchains` currently waits on a CPU fence before handing buffers back to Dawn. Correct
  but a per-frame stall; replace with an exported sync-fd semaphore later.

## Still to do (after the crash)

- Controllers: Dusklight already sees both Quest controllers as SDL gamepads, so this may need
  little or nothing — verify once something renders.
- Session lifecycle: focus loss, pause/resume, headset removal.
- Performance: drawing twice plus the HUD capture copies is expensive on mobile. Cinema mode first,
  then stereo once measured.
- Not available on Android yet: the headset render-size override and the Dusklight UI panel.
