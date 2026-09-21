#pragma once

#ifndef WIN32_LEAN_AND_MEAN
#define WIN32_LEAN_AND_MEAN
#endif
#ifndef NOMINMAX
#define NOMINMAX
#endif
#include <windows.h>

namespace vr {

// Redirects `module`'s import of `dll!function` to `replacement`. Returns the previous target in
// `outOriginal`. Used to observe the game's wgpuQueueSubmit calls, which live in webgpu_dawn.dll
// (outside the hookable game image).
bool patch_import(HMODULE module, const char* dll, const char* function, void* replacement, void** outOriginal);

// Restores an import previously patched with patch_import.
bool restore_import(HMODULE module, const char* dll, const char* function, void* original);

} // namespace vr
