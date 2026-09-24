# Android (Quest 3, Pico 4)

Released since v0.3.0 (current: v1.0.0) through the Dusklight VR edition APK. The 1.0 tabletop
features (x-ray, aim lines, quick wheel, Hawkeye screen) are tested on Windows only so far.

## Where it stands (2026-09-23, tested on a Quest 3)

**The game runs in the headset.** Cinema mode confirmed by eye; stable with every hook installed.

Working on device:

- The VR-edition app launches immersive with the mod bundled; the mod loads and activates.
- OpenXR session on Meta Quest 3 (1680x1760 per eye) goes IDLE → READY → FOCUSED and renders.
- The AHardwareBuffer handoff between Dawn and the OpenXR Vulkan device.
- Dusklight's UI panel finds its render target (4128x2208).

Fixed on the way (worth knowing, each looked like something else):

- **First-frame crash with any hook** — not a hook recursion. `aurora::rmlui::record_frame` returns
  a struct holding a `wgpu::BindGroup`, which is non-trivial, so the callee writes the result
  through the hidden return pointer (x8). Our mirror struct was trivial, so arm64 returned it in
  x0/x1 and the trampoline never passed x8 on. The real function then wrote through a stale pointer.
  Windows x64 returns both through memory, which is why it only broke here. **Hook signatures
  must match the callee's calling convention exactly, not only its layout.**
  `RmlRecordedFrame` now has a user-provided destructor to force the same return path.
- **Session stuck in IDLE (three loading dots)** — the instance got the Application, not the
  Activity. Meta's runtime follows the Activity's lifecycle. We now fetch `SDLActivity.mSingleton`
  (falling back to `SDL.getContext()`) through the app's class loader; native threads can't
  `FindClass` app classes.
- **fdsan abort in `vkDestroySemaphore`** — Dawn's exported sync fd still belongs to its fence;
  a Vulkan import takes ownership. Import a `dup()`.
- **"Dusklight UI unavailable"** — `aurora::rmlui::s_renderTarget` is in an anonymous namespace,
  so there is no symbol for it on Android. The target is now taken from
  `WebGPURenderInterface::BeginFrame`'s argument (by reference) on every platform.

Built but **not yet tried on the headset** (next session, first thing):

- **Water in stereo/tabletop, and tabletop culling**: the three overloaded hooks now use Itanium
  names on non-Windows (`J3DUClipper::clip` ×2, `J3DTexMtx::calcTexMtx`).
- **Per-eye material refresh**: `callbacks_run` has no symbol on Android (clang inlined its only
  call site). Fallback `interp_mirror` in `render_hooks.cpp` hooks
  `dusk::interp::add_interpolation_callback` (both overloads) and `begin_sim_tick`, and keeps its
  own copy of the list following the game's rules. Log line on success: "Per-eye material refresh:
  mirrored interpolation callbacks".
- **Cinema render size**: the game rendered at the full 4128x2208 window. On Android, Cinema now
  renders at roughly what the virtual screen can show (about 1400x788 at the default screen size).
  Log line: "Rendering at WxH for the headset".
- **No CPU stall in the copy**: the Vulkan copy signals a semaphore exported as a sync fd. Dawn
  imports it as the fence to wait on before rendering into the targets again. Log line:
  "Vulkan copy sync: semaphore handed to Dawn" (or "CPU wait" as a fallback).
- A temporary `PROBE` loop in `render_hooks.cpp` `install()` logs which interp/clip/texmtx names
  resolve. Read its output once, then delete it.

Performance before those fixes: **22 of 72 fps in Cinema**, app GPU time about 36 ms (VrApi logcat
line `FPS=22/72 ... App=36ms`). Read it with `adb logcat -d | grep "VrApi.*FPS="`.

Other known gaps:

- Tabletop passthrough works since the manifest declares `com.oculus.feature.PASSTHROUGH`
  (`XR_FB_passthrough` + `ALPHA_BLEND` available); tabletop runs at ~22 fps (no culling, depth cut).
- Disc selection: Dusklight's launch menu draws to the invisible 2D surface, so the VR edition
  (patch section 5) loads mods before the launch menu, and the VR mod shows it on its UI panel.
  Verified on a clean install.
- APKs are signed with a fixed key (repo secrets), so updates install over the previous build.
- Pause/resume and headset removal are untested.

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
   --cvar mod.com_noahsmaximum_dusklight__vr.mode=2'"
sleep 30
adb logcat -d | grep -iE "noahsmaximum|signal (6|11)|SESSION_STATE"
adb logcat -d | grep "VrApi.*FPS=" | tail -3   # frame rate / GPU time
```

`mode`: 0 off, 1 stereo, 2 cinema, 3 tabletop. `hookLevel` (default 4) bisects hook crashes:
0 none, 1 Aurora frame hooks, 2 + frame delivery, 3 + Dusklight UI, 4 everything.

Notes:

- `dusk_args` takes the same options as the desktop build; it splits on spaces, so **no spaces or
  commas in paths** (that is why the ROM was renamed).
- The app needs storage permission once:
  `adb shell appops set dev.twilitrealm.dusk.vr MANAGE_EXTERNAL_STORAGE allow`.
- The headset must be awake: `adb shell dumpsys power | grep mWakefulness` must say `Awake`.
  Asleep, or with the controllers asleep, the launch silently does nothing.
- Crashes: `adb logcat -d | grep -A25 "backtrace:"`. Symbolize our frames with
  `llvm-symbolizer --obj=build-android/dusklight_vr.so -C -f -p 0x<pc>` (NDK `bin/`). A
  one-frame stack inside `libmain.so` usually means a hook signature/ABI mismatch.
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
  (`src/interop_vulkan.cpp`). Imported semaphores stay alive until that copy slot's fence is next
  waited on.
- The copy signals `done[slot]`, exported as a sync fd and imported into Dawn
  (`wgpuDeviceImportSharedFence`, which keeps its own duplicate) as the fence for the next
  `BeginAccess`. If the driver can't export, or an export fails, it falls back to a CPU fence wait.

## Still to do

- Verify the four untested fixes above; then measure Cinema and Stereo again.
- Performance, if still short: `renderScalePercent` for stereo, eye-aspect EFB instead of 4:3, the
  HUD capture's extra passes, and `game.enableFrameInterpolation` (unlimited may cost more than it
  gives with xrWaitFrame pacing).
- Controllers: Dusklight already sees both Quest controllers as SDL gamepads; verify in play.
- Session lifecycle: focus loss, pause/resume, headset removal.
