#include "iat_hook.hpp"

#include <cstring>

namespace vr {
namespace {

IMAGE_THUNK_DATA* find_thunk(HMODULE module, const char* dll, const char* function) {
    auto* base = reinterpret_cast<BYTE*>(module);
    auto* dos = reinterpret_cast<IMAGE_DOS_HEADER*>(base);
    if (dos->e_magic != IMAGE_DOS_SIGNATURE) {
        return nullptr;
    }
    auto* nt = reinterpret_cast<IMAGE_NT_HEADERS*>(base + dos->e_lfanew);
    const auto& dir = nt->OptionalHeader.DataDirectory[IMAGE_DIRECTORY_ENTRY_IMPORT];
    if (dir.VirtualAddress == 0) {
        return nullptr;
    }
    for (auto* imp = reinterpret_cast<IMAGE_IMPORT_DESCRIPTOR*>(base + dir.VirtualAddress); imp->Name != 0;
         ++imp) {
        if (_stricmp(reinterpret_cast<const char*>(base + imp->Name), dll) != 0) {
            continue;
        }
        auto* names = reinterpret_cast<IMAGE_THUNK_DATA*>(base + imp->OriginalFirstThunk);
        auto* addrs = reinterpret_cast<IMAGE_THUNK_DATA*>(base + imp->FirstThunk);
        for (; names->u1.AddressOfData != 0; ++names, ++addrs) {
            if (IMAGE_SNAP_BY_ORDINAL(names->u1.Ordinal)) {
                continue;
            }
            auto* byName = reinterpret_cast<IMAGE_IMPORT_BY_NAME*>(base + names->u1.AddressOfData);
            if (std::strcmp(reinterpret_cast<const char*>(byName->Name), function) == 0) {
                return addrs;
            }
        }
    }
    return nullptr;
}

bool write_thunk(IMAGE_THUNK_DATA* thunk, void* value) {
    DWORD old = 0;
    if (!VirtualProtect(&thunk->u1.Function, sizeof(void*), PAGE_READWRITE, &old)) {
        return false;
    }
    InterlockedExchangePointer(reinterpret_cast<void**>(&thunk->u1.Function), value);
    VirtualProtect(&thunk->u1.Function, sizeof(void*), old, &old);
    return true;
}

} // namespace

bool patch_import(HMODULE module, const char* dll, const char* function, void* replacement, void** outOriginal) {
    IMAGE_THUNK_DATA* thunk = find_thunk(module, dll, function);
    if (thunk == nullptr) {
        return false;
    }
    *outOriginal = reinterpret_cast<void*>(thunk->u1.Function);
    return write_thunk(thunk, replacement);
}

bool restore_import(HMODULE module, const char* dll, const char* function, void* original) {
    IMAGE_THUNK_DATA* thunk = find_thunk(module, dll, function);
    return thunk != nullptr && write_thunk(thunk, original);
}

} // namespace vr
