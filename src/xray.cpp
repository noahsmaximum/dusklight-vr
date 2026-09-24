#include "xray.hpp"

#ifdef _WIN32
#include "iat_hook.hpp"
#endif

#include "mods/svc/gfx.h"
#include "mods/svc/hook.hpp"
#include "mods/svc/log.hpp"

#include <webgpu/webgpu.h>

#include <atomic>
#include <cstdlib>
#include <cstring>
#include <string>
#include <string_view>

using CreateShaderModuleFn = WGPUShaderModule (*)(WGPUDevice, const WGPUShaderModuleDescriptor*);

#ifndef _WIN32
// Dawn is linked into the game on Android, so its entry point can be hooked like a game function.
DEFINE_HOOK_SYMBOL("wgpuDeviceCreateShaderModule",
    WGPUShaderModule(WGPUDevice, const WGPUShaderModuleDescriptor*), CreateShaderModule);
#endif

namespace vr::xray {
namespace {

std::atomic<bool> g_installed{false};
std::atomic<bool> g_loggedPatch{false};
#ifdef _WIN32
CreateShaderModuleFn g_origCreate = nullptr;
#endif

// What Aurora emits for this fog type (gx/shader.cpp): the orthographic comment, the reverse-exp2
// curve, and the final blend with the fog colour, which the cut-out replaces.
constexpr std::string_view kOrthoMarker = "// Orthographic fog";
constexpr std::string_view kRevExp2Marker = "fogF = 1.0 - fogF;";
constexpr std::string_view kFogBlend =
    "prev = vec4f(mix(prev.rgb, ubuf.fog.color.rgb, clamp(fogZ, 0.0, 1.0)), prev.a);";

// Eye data layout (8 x vec4f, see push_eye):
//   q0..q3 worldFromClip   q4 eye.xyz, target width   q5 Link.xyz, target height
//   q6 radius, taper, fadeNear, fadeFar                q7 margin
// Fog a/c hold the data's offset in 16-byte units: a = high bits, c = 2048 + low 10 bits.
constexpr std::string_view kCutout = R"(// Dusklight VR: tabletop x-ray (replaces this fog type's colour blend)
    {
        let xb = (u32(ubuf.fog.a) * 1024u + u32(ubuf.fog.c) - 2048u) * 4u;
        let q0 = bitcast<vec4f>(vec4u(abuf[xb + 0u], abuf[xb + 1u], abuf[xb + 2u], abuf[xb + 3u]));
        let q1 = bitcast<vec4f>(vec4u(abuf[xb + 4u], abuf[xb + 5u], abuf[xb + 6u], abuf[xb + 7u]));
        let q2 = bitcast<vec4f>(vec4u(abuf[xb + 8u], abuf[xb + 9u], abuf[xb + 10u], abuf[xb + 11u]));
        let q3 = bitcast<vec4f>(vec4u(abuf[xb + 12u], abuf[xb + 13u], abuf[xb + 14u], abuf[xb + 15u]));
        let q4 = bitcast<vec4f>(vec4u(abuf[xb + 16u], abuf[xb + 17u], abuf[xb + 18u], abuf[xb + 19u]));
        let q5 = bitcast<vec4f>(vec4u(abuf[xb + 20u], abuf[xb + 21u], abuf[xb + 22u], abuf[xb + 23u]));
        let q6 = bitcast<vec4f>(vec4u(abuf[xb + 24u], abuf[xb + 25u], abuf[xb + 26u], abuf[xb + 27u]));
        let q7 = bitcast<vec4f>(vec4u(abuf[xb + 28u], abuf[xb + 29u], abuf[xb + 30u], abuf[xb + 31u]));
        let xrSize = vec2f(q4.w, q5.w);
        if (all(abs(ubuf.render_viewport_size - xrSize) < vec2f(1.0))) {
            let xrUv = in.pos.xy / xrSize;
            let xrH = mat4x4f(q0, q1, q2, q3) * vec4f(xrUv.x * 2.0 - 1.0, 1.0 - xrUv.y * 2.0, in.pos.z, 1.0);
            let xrWorld = xrH.xyz / xrH.w;
            let xrToFrag = xrWorld - q4.xyz;
            // Close to the viewer's head: fade out.
            var xrKeep = smoothstep(q6.z, q6.w, length(xrToFrag));
            // In front of Link, inside the cylinder from the eye to him: cut away. It closes over a
            // short distance just in front of him, so the ground he stands on stays.
            let xrToLink = q5.xyz - q4.xyz;
            let xrLinkDist = length(xrToLink);
            let xrDir = xrToLink / max(xrLinkDist, 1e-4);
            let xrAlong = dot(xrToFrag, xrDir);
            let xrAhead = xrLinkDist - xrAlong;
            if (xrAlong > 0.0 && xrAhead > q7.x) {
                let xrRadius = q6.x * clamp((xrAhead - q7.x) / max(q6.y, 1e-4), 0.0, 1.0);
                if (xrRadius > 0.0) {
                    let xrPerp = length(xrToFrag - xrDir * xrAlong);
                    xrKeep = min(xrKeep, smoothstep(xrRadius * 0.8, xrRadius, xrPerp));
                }
            }
            // Screen-door transparency (opaque passes can't blend): 4x4 ordered dither.
            let xrP = vec2u(in.pos.xy) & vec2u(3u);
            let xrX = xrP.x ^ xrP.y;
            let xrBayer = ((xrX & 1u) << 3u) | ((xrP.y & 1u) << 2u) | (xrX & 2u) | ((xrP.y >> 1u) & 1u);
            if (xrKeep < (f32(xrBayer) + 0.5) / 16.0) {
                discard;
            }
        }
    })";

// Returns true and fills `out` when `code` is a GX shader using the x-ray fog type.
bool patch_source(std::string_view code, std::string& out) {
    if (code.find(kOrthoMarker) == std::string_view::npos || code.find(kRevExp2Marker) == std::string_view::npos) {
        return false;
    }
    const size_t blend = code.find(kFogBlend);
    if (blend == std::string_view::npos) {
        return false;
    }
    out.reserve(code.size() + kCutout.size());
    out.assign(code.substr(0, blend));
    // DUSKLIGHT_VR_XRAY_DEBUG=1 tints instead of cutting: green = x-ray draw, blue = other render
    // target (skipped), magenta = would be cut.
    static const bool debug = [] {
        const char* v = std::getenv("DUSKLIGHT_VR_XRAY_DEBUG");
        return v != nullptr && v[0] == '1';
    }();
    if (debug) {
        std::string dbg(kCutout);
        const auto replace = [&](std::string_view a, std::string_view b) {
            const size_t at = dbg.find(a);
            if (at != std::string::npos) dbg.replace(at, a.size(), b);
        };
        replace("                discard;", "                prev = vec4f(1.0, 0.0, 1.0, 1.0);");
        if (std::getenv("DUSKLIGHT_VR_XRAY_DEBUG")[1] == 'd') {
            // "1d": show the decoded data instead: r = data slot, g = target width ok, b = radius ok.
            replace("        let xrSize = vec2f(q4.w, q5.w);",
                "        let xrSize = vec2f(q4.w, q5.w);\n"
                "        prev = vec4f(f32(xb) / 64.0, q4.w / 1216.0, q6.x / 200.0, 1.0);");
        }
        replace("            let xrUv = in.pos.xy / xrSize;",
            ""
            "            let xrUv = in.pos.xy / xrSize;");
        replace("        if (all(abs(ubuf.render_viewport_size - xrSize) < vec2f(1.0))) {",
            ""
            "        if (all(abs(ubuf.render_viewport_size - xrSize) < vec2f(1.0))) {");
        out.append(dbg);
    } else {
        out.append(kCutout);
    }
    out.append(code.substr(blend + kFogBlend.size()));
    return true;
}

// Rewrites GX shaders with the x-ray fog type before Dawn compiles them; everything else passes
// through untouched.
WGPUShaderModule create_shader_module(
    WGPUDevice device, const WGPUShaderModuleDescriptor* desc, CreateShaderModuleFn next) {
    if (desc == nullptr || desc->nextInChain == nullptr || desc->nextInChain->sType != WGPUSType_ShaderSourceWGSL) {
        return next(device, desc);
    }
    const std::string_view label =
        desc->label.data != nullptr
            ? std::string_view(desc->label.data, desc->label.length == WGPU_STRLEN ? std::strlen(desc->label.data)
                                                                                    : desc->label.length)
            : std::string_view{};
    if (label.substr(0, 9) != "GX Shader") {
        return next(device, desc);
    }
    const auto* wgsl = reinterpret_cast<const WGPUShaderSourceWGSL*>(desc->nextInChain);
    if (wgsl->code.data == nullptr) {
        return next(device, desc);
    }
    const std::string_view code(wgsl->code.data,
        wgsl->code.length == WGPU_STRLEN ? std::strlen(wgsl->code.data) : wgsl->code.length);
    std::string patched;
    if (!patch_source(code, patched)) {
        return next(device, desc);
    }
    if (!g_loggedPatch.exchange(true)) {
        mods::log::info("Tabletop x-ray: GX shaders patched");
    }
    WGPUShaderSourceWGSL source = *wgsl;
    source.code = WGPUStringView{patched.c_str(), patched.size()};
    WGPUShaderModuleDescriptor copy = *desc;
    copy.nextInChain = &source.chain;
    return next(device, &copy);
}

#ifdef _WIN32
WGPUShaderModule hk_create_shader_module(WGPUDevice device, const WGPUShaderModuleDescriptor* desc) {
    return create_shader_module(device, desc, g_origCreate);
}
#else
void create_shader_module_replace(ModContext*, void* args, void* retval, void*) {
    *static_cast<WGPUShaderModule*>(retval) = create_shader_module(mods::arg<WGPUDevice>(args, 0),
        mods::arg<const WGPUShaderModuleDescriptor*>(args, 1), CreateShaderModule::g_orig);
}
#endif

} // namespace

bool install() {
    if (g_installed) {
        return true;
    }
#ifdef _WIN32
    // Aurora lives in the game executable and calls Dawn through webgpu_dawn.dll's exports.
    void* orig = nullptr;
    if (!patch_import(GetModuleHandleW(nullptr), "webgpu_dawn.dll", "wgpuDeviceCreateShaderModule",
            reinterpret_cast<void*>(&hk_create_shader_module), &orig))
    {
        mods::log::warn("Tabletop x-ray unavailable: could not intercept shader creation");
        return false;
    }
    g_origCreate = reinterpret_cast<CreateShaderModuleFn>(orig);
#else
    if (mods::hook::replace<CreateShaderModule>(create_shader_module_replace) != MOD_OK) {
        mods::log::warn("Tabletop x-ray unavailable: could not intercept shader creation");
        return false;
    }
#endif
    g_installed = true;
    return true;
}

void uninstall() {
    if (!g_installed) {
        return;
    }
#ifdef _WIN32
    restore_import(GetModuleHandleW(nullptr), "webgpu_dawn.dll", "wgpuDeviceCreateShaderModule",
        reinterpret_cast<void*>(g_origCreate));
    g_origCreate = nullptr;
#endif
    g_installed = false;
}

bool available() { return g_installed; }

bool push_eye(const EyeParams& p, FogArgs& out) {
    float data[32]{};
    std::memcpy(data, p.worldFromClip, sizeof(p.worldFromClip));
    data[16] = p.eye[0];
    data[17] = p.eye[1];
    data[18] = p.eye[2];
    data[19] = p.targetWidth;
    data[20] = p.target[0];
    data[21] = p.target[1];
    data[22] = p.target[2];
    data[23] = p.targetHeight;
    data[24] = p.radius;
    data[25] = p.taper;
    data[26] = p.fadeNear;
    data[27] = p.fadeFar;
    data[28] = p.margin;
    GfxRange range{};
    if (svc_gfx->push_storage(mod_ctx, data, sizeof(data), &range) != MOD_OK || (range.offset & 15u) != 0) {
        return false;
    }
    const uint32_t units = range.offset / 16u;
    const uint32_t hi = units >> 10;
    if (hi >= 2048u) {
        return false; // beyond what the fog registers carry exactly (32 MiB)
    }
    // Orthographic fog: a = (farZ - nearZ) / (endZ - startZ), c = (startZ - nearZ) / (endZ - startZ).
    // Both are small integers, exact in the registers' 12-bit mantissa. c > a also keeps an
    // unpatched shader (built before the hook) from fogging: its fog factor clamps to ~0.
    out.startZ = 2048.0f + static_cast<float>(units & 1023u);
    out.endZ = out.startZ + 1.0f;
    out.nearZ = 0.0f;
    out.farZ = static_cast<float>(hi);
    return true;
}

} // namespace vr::xray
