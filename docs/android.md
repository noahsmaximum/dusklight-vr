# Android (Quest 3, Pico 4) — work in progress

Standalone headsets need two things the Windows build never did: a Dusklight app that launches into
VR, and a Vulkan path for handing rendered frames to OpenXR.

## Why a custom Dusklight build

The official Android build blocks the mod twice:

- **The app isn't a VR app.** Its manifest only declares a flat launcher activity, so Quest and Pico
  run it as a 2D panel and refuse an immersive session. A mod can't change the app manifest.
- **Its release build exports nothing we can use.** On Windows the mod reaches Dawn's D3D12 device
  through exported functions and hooks Aurora internals by name. The Android build exports the
  game's own functions and Dawn's public API only — no Aurora internals, no Dawn internals. Dawn's
  shared-texture API *is* exported, but Dusklight never enables those features on its device, and a
  mod can't enable them after the fact.

So we build Dusklight ourselves with an additive patch (`android/patch-dusklight.sh`):

1. **Manifest**: head-tracking feature, the Khronos `IMMERSIVE_HMD` and Meta `VR` launch categories,
   Quest/Pico device metadata, and the runtime-broker `queries` entry the OpenXR loader needs.
2. **Aurora** (`lib/webgpu/gpu.cpp`): request Dawn's shared-texture/shared-fence features when the
   GPU offers them (AHardwareBuffer, dma-buf, opaque FD, sync FD).

Nothing is sent upstream; the patch is applied to a fresh checkout at build time. Dusklight is CC0,
so building and sharing our own build is fine.

## Building the APK

Run the **Android VR APK** workflow from the Actions tab (input: the Dusklight tag, default
`v2.0.1`). It mirrors Dusklight's own Android CI, applies the patch, builds the APK, signs it with a
throwaway key for sideloading, and uploads it as an artifact.

Install it with `adb install -r dusklight-vr-edition-arm64.apk` or SideQuest. It installs alongside
the official app (separate package name), so official saves and settings are untouched. The VR
edition keeps its own game data, so point it at your ROM again on first run.

## How the frame handoff works on Android

With the patch in place the mod needs no engine internals:

- Render targets live in `AHardwareBuffer`s, imported into Dusklight's Dawn device through
  `wgpuDeviceImportSharedTextureMemory` and into the Vulkan device OpenXR creates.
- Dawn composites the eyes/quads into them; Dawn's shared fence (sync FD) is imported as a Vulkan
  semaphore so the copy waits for Dawn's work.
- The copy into the XR swapchain images runs on the OpenXR Vulkan device
  (`src/interop_vulkan.cpp`).

## Still to do

- Controllers: OpenXR input mapped onto a virtual gamepad (Quest/Pico controllers are not gamepads)
- Session lifecycle: focus loss, pause/resume, headset removal
- First run on device: nothing here has run on a headset yet — only builds
- Performance: rendering twice plus the HUD capture copies is expensive on mobile GPUs. Cinema mode
  (one flat image on a big screen) is the first target; stereo needs measurement.
- `copy_to_swapchains` waits on a fence before handing the targets back to Dawn; replacing that with
  an exported sync-fd semaphore removes a per-frame CPU stall.
- Not available on Android: the headset render-size override and the Dusklight UI panel, which both
  use Aurora internals the release build doesn't export.

## Building the mod for Android

CI builds `android-aarch64` alongside Windows, and the combined `.dusk` carries both. Locally:

```
cmake -B build-android -G Ninja \
  -DCMAKE_TOOLCHAIN_FILE=$ANDROID_HOME/ndk/29.0.14206865/build/cmake/android.toolchain.cmake \
  -DANDROID_ABI=arm64-v8a -DANDROID_PLATFORM=android-28
cmake --build build-android
```
