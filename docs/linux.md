# Linux / Steam Frame

Status: **in development** (branch `linux`). Target: a Steam Frame running SteamOS standalone
(arm64), and Linux PCs (SteamOS, desktop distros) with SteamVR or Monado.

## How it works

No special Dusklight build is needed, unlike Quest. The official Dusklight AppImages
(`linux-x86_64`, `linux-arm64`) link with `-rdynamic`, so they export Aurora, the WebGPU C API
*and* Dawn's internals. The mod uses that the same way the Windows build uses Dawn's D3D12
exports (`src/interop_linux.cpp`):

- OpenXR gets **Dawn's own Vulkan device** (`XR_KHR_vulkan_enable`, not `enable2`: the device
  exists already). Instance, physical device, device, queue family and queue come from
  `dawn::native::vulkan::{Device,PhysicalDevice,Queue}` accessors looked up with `dlsym`.
- Eye/quad targets are ordinary Dawn textures; the `VkImage` behind each comes from
  `dawn::native::vulkan::Texture::GetHandle`.
- After Aurora submits a frame (`aurora::gfx::after_submit`, render thread), the copy into the
  runtime's swapchain images is recorded and submitted **on Dawn's queue, from Dawn's thread**:
  queue order is the synchronisation, nothing is shared or imported. The source goes from the layout
  Dawn tracks (`Texture::GetCurrentLayout`) to `TRANSFER_SRC` and back, so Dawn's tracking stays true.
- Vulkan entry points come from the `libvulkan.so.1` Dawn already loaded (headers only at build
  time; no link dependency). The OpenXR loader is linked statically, as on Windows.
- The `.dusk` carries `lib/linux-x86_64/mod.so` and `lib/linux-aarch64/mod.so`; Dusklight's
  loader picks the one for the machine.

Everything else (hooks, x-ray, quick wheel, aim lines) is the shared code. Symbols resolve through
the exe's exports and its embedded symbol database, as on Android. One name differs from Android:
the libstdc++ mangling of `dusk::interp::add_interpolation_callback(..., std::shared_ptr<void>)`.

`VR_STANDALONE` (vr_config.hpp) is set on Android and on Linux arm64 (Steam Frame): 60% default
render scale, cinema render size from the headset's resolution, and the hidden 2D surface shrunk to
the render size.

## Building

On Linux (or WSL), with clang, cmake >= 3.26, ninja, and `libx11-dev` (the OpenXR SDK's configure
wants a presentation backend's headers even for the loader alone):

```sh
cmake -S . -B build -G Ninja -DCMAKE_BUILD_TYPE=RelWithDebInfo -DCMAKE_C_COMPILER=clang -DCMAKE_CXX_COMPILER=clang++
cmake --build build
```

CI builds `linux-x86_64` (ubuntu-24.04) and `linux-aarch64` (ubuntu-24.04-arm) and merges them into
the release `.dusk`. The mod needs glibc 2.38; Dusklight itself needs 2.39.

## Testing on Windows through WSL

WSL has no GPU Vulkan driver, only llvmpipe, and Aurora ignores CPU adapters.
`tools/linux/fake_gpu_layer.c` is a tiny Vulkan layer that reports CPU devices as integrated GPUs, so
the official x86_64 AppImage runs under WSLg (a few fps, enough to exercise the whole path):

```sh
cc -shared -fPIC -O2 -o /opt/layer/libfake_gpu_layer.so tools/linux/fake_gpu_layer.c
cp tools/linux/fake_gpu_layer.json /opt/layer/
export VK_ADD_LAYER_PATH=/opt/layer VK_INSTANCE_LAYERS=VK_LAYER_DUSKLIGHT_fake_gpu
./Dusklight-*-linux-x86_64.AppImage --dvd <iso> --mods build/mods --stage F_SP103,0,5,-1 \
    --cvar mod.com_noahsmaximum_dusklight__vr.simulateHmd=true
```

With `DUSKLIGHT_VR_NO_XR=1` this is the desktop simulation (the copy self-test reads a pixel back
through Dawn's Vulkan queue). Without it, point the loader at Monado's simulated HMD
(`XR_RUNTIME_JSON=/usr/share/openxr/1/openxr_monado.json`, `monado-service` running) for a real
OpenXR session.

## Verified so far

- Official Dusklight 2.0.3 x86_64 AppImage under WSLg (llvmpipe): the mod loads, every hook installs,
  the interop reaches Dawn's Vulkan device, stereo simulation renders Ordon, and the copy self-test
  reads the scene back through Dawn's queue.
- Every symbol the mod imports is exported by the official 2.0.3 x86_64 and arm64 binaries.

## Not yet verified

- A real OpenXR session (Monado simulated HMD in WSL, then SteamVR).
- Anything on a Steam Frame: SteamVR's Vulkan device-extension requirements against what Dawn
  enables, performance on the Snapdragon 8 Gen 3, launching the AppImage from the Frame's library,
  controller input.
