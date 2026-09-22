#include "gpu.hpp"

#include "mods/svc/log.hpp"

#include <mutex>
#include <unordered_map>

namespace vr::gpu {
namespace {

#define VR_FULLSCREEN_VS R"(
struct VsOut {
    @builtin(position) pos: vec4f,
    @location(0) uv: vec2f,
};

@vertex fn vs_main(@builtin(vertex_index) i: u32) -> VsOut {
    var p = array<vec2f, 3>(vec2f(-1.0, -1.0), vec2f(3.0, -1.0), vec2f(-1.0, 3.0));
    var o: VsOut;
    o.pos = vec4f(p[i], 0.0, 1.0);
    o.uv = vec2f(p[i].x * 0.5 + 0.5, 0.5 - p[i].y * 0.5);
    return o;
}
)"

constexpr const char* kShader = VR_FULLSCREEN_VS R"(
@group(0) @binding(0) var samp: sampler;
@group(0) @binding(1) var tex0: texture_2d<f32>;
@group(0) @binding(2) var tex1: texture_2d<f32>;

@fragment fn fs_blit(in: VsOut) -> @location(0) vec4f {
    return vec4f(textureSample(tex0, samp, in.uv).rgb, 1.0);
}

@fragment fn fs_key(in: VsOut) -> @location(0) vec4f {
    let c = textureSample(tex0, samp, in.uv);
    let key = textureLoad(tex1, vec2i(0, 0), 0).rgb; // 1x1 key colour texture
    return vec4f(c.rgb + (1.0 - c.a) * key, 1.0);
}

@fragment fn fs_blit_alpha(in: VsOut) -> @location(0) vec4f {
    return textureSample(tex0, samp, in.uv);
}

// tex0 = 2D layer drawn over black, tex1 = the same layer over white. For any "over" blend,
// white - black = (1 - alpha), and the black result is the premultiplied colour.
@fragment fn fs_combine(in: VsOut) -> @location(0) vec4f {
    let b = textureSample(tex0, samp, in.uv).rgb;
    let w = textureSample(tex1, samp, in.uv).rgb;
    let a = clamp(1.0 - dot(w - b, vec3f(1.0 / 3.0)), 0.0, 1.0);
    return vec4f(min(b, vec3f(a)), a);
}

struct ClearOut {
    @location(0) color: vec4f,
    @builtin(frag_depth) depth: f32,
};

// Clears the EFB colour and depth (reversed-Z puts the far plane at 0).
@fragment fn fs_clear_black_rev(in: VsOut) -> ClearOut {
    return ClearOut(vec4f(0.0, 0.0, 0.0, 1.0), 0.0);
}
@fragment fn fs_clear_black_std(in: VsOut) -> ClearOut {
    return ClearOut(vec4f(0.0, 0.0, 0.0, 1.0), 1.0);
}
@fragment fn fs_clear_white_rev(in: VsOut) -> ClearOut {
    return ClearOut(vec4f(1.0, 1.0, 1.0, 1.0), 0.0);
}
@fragment fn fs_clear_white_std(in: VsOut) -> ClearOut {
    return ClearOut(vec4f(1.0, 1.0, 1.0, 1.0), 1.0);
}
)";

// Tabletop mask. Rebuilds each pixel's world position from the depth snapshot and keeps what lies
// inside the table cylinder (radius around the anchor, down to a floor below the surface). The
// blend state multiplies the scene colour by the mask and stores the mask as alpha, leaving
// premultiplied colour for the compositor.
constexpr const char* kCutShader = VR_FULLSCREEN_VS R"(
struct Cut {
    world_from_clip: mat4x4f,
    anchor: vec4f,
    params: vec4f,
};

@group(0) @binding(0) var depth_tex: texture_2d<f32>;
@group(0) @binding(1) var<uniform> cut: Cut;

@fragment fn fs_cut(in: VsOut) -> @location(0) vec4f {
    let dims = vec2f(textureDimensions(depth_tex));
    let texel = vec2i(clamp(in.uv * dims, vec2f(0.0), dims - vec2f(1.0)));
    let d = textureLoad(depth_tex, texel, 0).r;
    if (abs(d - cut.params.w) < 1e-7) {
        return vec4f(0.0); // nothing drawn here (the sky is skipped)
    }
    let ndc = vec2f(in.uv.x * 2.0 - 1.0, 1.0 - in.uv.y * 2.0);
    let h = cut.world_from_clip * vec4f(ndc, d, 1.0);
    let world = h.xyz / h.w;
    let r = distance(world.xz, cut.anchor.xz);
    var a = 1.0 - smoothstep(cut.anchor.w - cut.params.x, cut.anchor.w, r);
    a *= smoothstep(cut.params.y - cut.params.z, cut.params.y, world.y);
    return vec4f(0.0, 0.0, 0.0, a);
}
)";

enum class Kind { Blit, BlitAlpha, Key, Combine, CombineOver, ClearBlackRev, ClearBlackStd, ClearWhiteRev, ClearWhiteStd, Cut };

struct State {
    WGPUDevice device = nullptr;
    WGPUShaderModule module = nullptr;
    WGPUSampler sampler = nullptr;
    WGPUBindGroupLayout bgl = nullptr;
    WGPUPipelineLayout layout = nullptr;
    WGPUTextureView dummyView = nullptr;
    WGPUTexture dummy = nullptr;
    // Tabletop key colour as a 1x1 texture (shares the blit bind group layout).
    WGPUTexture keyTex = nullptr;
    WGPUTextureView keyView = nullptr;
    uint32_t keyColor = 0xFFFFFFFF; // forces the first upload
    WGPUQueue queue = nullptr;
    std::mutex mutex;
    // Offscreen-target pipelines keyed by (format, kind); scene pipelines keyed by (layout key, kind).
    std::unordered_map<uint64_t, WGPURenderPipeline> targetPipelines;
    std::unordered_map<uint64_t, WGPURenderPipeline> scenePipelines;
    GfxDrawTypeHandle mirrorDraw = 0;
    GfxDrawTypeHandle restoreDraw = 0;
    GfxDrawTypeHandle clearDraw = 0;
    GfxDrawTypeHandle cutDraw = 0;
    // Tabletop cut: its own module/layout (unfilterable depth + uniform buffer).
    WGPUShaderModule cutModule = nullptr;
    WGPUBindGroupLayout cutBgl = nullptr;
    WGPUPipelineLayout cutLayout = nullptr;
    bool reversedZ = true;
    bool formatLogged = false;
};
State g;

WGPUStringView sv(const char* s) { return WGPUStringView{s, WGPU_STRLEN}; }

WGPUBlendState premultiplied_over() {
    WGPUBlendState b{};
    b.color = {WGPUBlendOperation_Add, WGPUBlendFactor_One, WGPUBlendFactor_OneMinusSrcAlpha};
    b.alpha = {WGPUBlendOperation_Add, WGPUBlendFactor_One, WGPUBlendFactor_OneMinusSrcAlpha};
    return b;
}

const char* entry_for(Kind k) {
    switch (k) {
    case Kind::Blit:
        return "fs_blit";
    case Kind::Key:
        return "fs_key";
    case Kind::BlitAlpha:
        return "fs_blit_alpha";
    case Kind::Cut:
        return "fs_cut";
    case Kind::Combine:
    case Kind::CombineOver:
        return "fs_combine";
    case Kind::ClearBlackRev:
        return "fs_clear_black_rev";
    case Kind::ClearBlackStd:
        return "fs_clear_black_std";
    case Kind::ClearWhiteRev:
        return "fs_clear_white_rev";
    case Kind::ClearWhiteStd:
        return "fs_clear_white_std";
    }
    return "fs_blit";
}

WGPURenderPipeline create_pipeline(const WGPUColorTargetState* targets, uint32_t targetCount,
    WGPUTextureFormat depthFormat, uint32_t samples, Kind kind) {
    const bool cut = kind == Kind::Cut;
    WGPUFragmentState fs = WGPU_FRAGMENT_STATE_INIT;
    fs.module = cut ? g.cutModule : g.module;
    fs.entryPoint = sv(entry_for(kind));
    fs.targetCount = targetCount;
    fs.targets = targets;

    WGPUDepthStencilState ds = WGPU_DEPTH_STENCIL_STATE_INIT;
    ds.format = depthFormat;
    const bool clears = kind == Kind::ClearBlackRev || kind == Kind::ClearBlackStd || kind == Kind::ClearWhiteRev ||
                        kind == Kind::ClearWhiteStd;
    ds.depthWriteEnabled = clears ? WGPUOptionalBool_True : WGPUOptionalBool_False;
    ds.depthCompare = WGPUCompareFunction_Always;

    WGPURenderPipelineDescriptor desc = WGPU_RENDER_PIPELINE_DESCRIPTOR_INIT;
    desc.label = sv("Dusklight VR composite");
    desc.layout = cut ? g.cutLayout : g.layout;
    desc.vertex.module = cut ? g.cutModule : g.module;
    desc.vertex.entryPoint = sv("vs_main");
    desc.primitive.topology = WGPUPrimitiveTopology_TriangleList;
    desc.depthStencil = depthFormat != WGPUTextureFormat_Undefined ? &ds : nullptr;
    desc.multisample.count = samples;
    desc.fragment = &fs;
    return wgpuDeviceCreateRenderPipeline(g.device, &desc);
}

WGPURenderPipeline target_pipeline(WGPUTextureFormat format, Kind kind) {
    const uint64_t key = (static_cast<uint64_t>(format) << 8) | static_cast<uint64_t>(kind);
    std::lock_guard lock{g.mutex};
    if (auto it = g.targetPipelines.find(key); it != g.targetPipelines.end()) {
        return it->second;
    }
    WGPUColorTargetState target = WGPU_COLOR_TARGET_STATE_INIT;
    target.format = format;
    target.writeMask = WGPUColorWriteMask_All;
    WGPURenderPipeline p = create_pipeline(&target, 1, WGPUTextureFormat_Undefined, 1, kind);
    g.targetPipelines.emplace(key, p);
    return p;
}

WGPURenderPipeline scene_pipeline(const GfxRenderTargetLayout& layout, Kind kind) {
    const uint64_t key = (layout.key * 16) ^ static_cast<uint64_t>(kind);
    std::lock_guard lock{g.mutex};
    if (auto it = g.scenePipelines.find(key); it != g.scenePipelines.end()) {
        return it->second;
    }
    const WGPUBlendState over = premultiplied_over();
    // colour = dst * mask, alpha = mask
    WGPUBlendState mask{};
    mask.color = {WGPUBlendOperation_Add, WGPUBlendFactor_Zero, WGPUBlendFactor_SrcAlpha};
    mask.alpha = {WGPUBlendOperation_Add, WGPUBlendFactor_One, WGPUBlendFactor_Zero};
    const WGPUBlendState* blend = kind == Kind::CombineOver ? &over : (kind == Kind::Cut ? &mask : nullptr);
    WGPUColorTargetState targets[GFX_MAX_COLOR_ATTACHMENTS];
    const uint32_t count = gfx_init_color_target_states(&layout, targets, blend, WGPUColorWriteMask_All);
    if (!g.formatLogged) {
        g.formatLogged = true;
        mods::log::info("Scene target: colour format {}, {} sample(s), reversed Z {}",
            static_cast<int>(layout.color_attachments[GFX_SCENE_COLOR_ATTACHMENT_INDEX].format), layout.sample_count, g.reversedZ);
    }
    WGPURenderPipeline p = create_pipeline(targets, count, layout.depth_stencil_format, layout.sample_count, kind);
    g.scenePipelines.emplace(key, p);
    return p;
}

WGPUBindGroup make_bind_group(WGPUTextureView a, WGPUTextureView b) {
    WGPUBindGroupEntry entries[3] = {WGPU_BIND_GROUP_ENTRY_INIT, WGPU_BIND_GROUP_ENTRY_INIT,
        WGPU_BIND_GROUP_ENTRY_INIT};
    entries[0].binding = 0;
    entries[0].sampler = g.sampler;
    entries[1].binding = 1;
    entries[1].textureView = a != nullptr ? a : g.dummyView;
    entries[2].binding = 2;
    entries[2].textureView = b != nullptr ? b : (a != nullptr ? a : g.dummyView);
    WGPUBindGroupDescriptor desc = WGPU_BIND_GROUP_DESCRIPTOR_INIT;
    desc.layout = g.bgl;
    desc.entryCount = 3;
    desc.entries = entries;
    return wgpuDeviceCreateBindGroup(g.device, &desc);
}

void draw_fullscreen(WGPURenderPassEncoder pass, WGPURenderPipeline pipeline, WGPUBindGroup bg) {
    wgpuRenderPassEncoderSetPipeline(pass, pipeline);
    wgpuRenderPassEncoderSetBindGroup(pass, 0, bg, 0, nullptr);
    wgpuRenderPassEncoderDraw(pass, 3, 1, 0, 0);
}

void encode_target_pass(WGPUCommandEncoder encoder, WGPUTextureView dst, WGPURenderPipeline pipeline,
    WGPUBindGroup bg, WGPUColor clear) {
    WGPURenderPassColorAttachment att = WGPU_RENDER_PASS_COLOR_ATTACHMENT_INIT;
    att.view = dst;
    att.loadOp = WGPULoadOp_Clear;
    att.storeOp = WGPUStoreOp_Store;
    att.clearValue = clear;
    WGPURenderPassDescriptor rp = WGPU_RENDER_PASS_DESCRIPTOR_INIT;
    rp.label = sv("Dusklight VR target");
    rp.colorAttachmentCount = 1;
    rp.colorAttachments = &att;
    WGPURenderPassEncoder pass = wgpuCommandEncoderBeginRenderPass(encoder, &rp);
    draw_fullscreen(pass, pipeline, bg);
    wgpuRenderPassEncoderEnd(pass);
    wgpuRenderPassEncoderRelease(pass);
}

// --- push_draw callbacks (render worker, inside a game render pass) --------------------------------

void mirror_draw(ModContext*, const GfxDrawContext* ctx, const void* payloadRaw, size_t size, void*) {
    if (size != sizeof(MirrorPayload)) {
        return;
    }
    const auto& p = *static_cast<const MirrorPayload*>(payloadRaw);
    const float w = static_cast<float>(ctx->layout.color_attachments[0].width);
    const float h = static_cast<float>(ctx->layout.color_attachments[0].height);
    const bool sbs = p.sceneLeft != nullptr && p.sceneRight != nullptr;
    const bool hud = p.hudBlack != nullptr && p.hudWhite != nullptr;

    WGPUBindGroup hudBg = hud ? make_bind_group(p.hudBlack, p.hudWhite) : nullptr;
    if (sbs) {
        WGPURenderPipeline blit = scene_pipeline(ctx->layout, Kind::Blit);
        WGPURenderPipeline over = scene_pipeline(ctx->layout, Kind::CombineOver);
        const WGPUTextureView scenes[2] = {p.sceneLeft, p.sceneRight};
        for (int eye = 0; eye < 2; ++eye) {
            wgpuRenderPassEncoderSetViewport(ctx->pass, eye * w * 0.5f, 0.0f, w * 0.5f, h, 0.0f, 1.0f);
            WGPUBindGroup bg = make_bind_group(scenes[eye], nullptr);
            draw_fullscreen(ctx->pass, blit, bg);
            wgpuBindGroupRelease(bg);
            if (hudBg != nullptr) {
                draw_fullscreen(ctx->pass, over, hudBg);
            }
        }
    } else if (hudBg != nullptr) {
        draw_fullscreen(ctx->pass, scene_pipeline(ctx->layout, Kind::CombineOver), hudBg);
    }
    if (hudBg != nullptr) {
        wgpuBindGroupRelease(hudBg);
    }
}

struct RestorePayload {
    WGPUTextureView scene;
    bool keepAlpha;
};

void restore_draw(ModContext*, const GfxDrawContext* ctx, const void* payload, size_t size, void*) {
    if (size != sizeof(RestorePayload)) {
        return;
    }
    const auto& p = *static_cast<const RestorePayload*>(payload);
    WGPUBindGroup bg = make_bind_group(p.scene, nullptr);
    draw_fullscreen(ctx->pass, scene_pipeline(ctx->layout, p.keepAlpha ? Kind::BlitAlpha : Kind::Blit), bg);
    wgpuBindGroupRelease(bg);
}

struct CutPayload {
    WGPUTextureView depth;
    GfxRange uniform;
};

void cut_draw(ModContext*, const GfxDrawContext* ctx, const void* payload, size_t size, void*) {
    if (size != sizeof(CutPayload) || ctx->uniform_buffer == nullptr) {
        return;
    }
    const auto& p = *static_cast<const CutPayload*>(payload);
    WGPUBindGroupEntry entries[2] = {WGPU_BIND_GROUP_ENTRY_INIT, WGPU_BIND_GROUP_ENTRY_INIT};
    entries[0].binding = 0;
    entries[0].textureView = p.depth;
    entries[1].binding = 1;
    entries[1].buffer = ctx->uniform_buffer;
    entries[1].offset = p.uniform.offset;
    entries[1].size = sizeof(CutParams);
    WGPUBindGroupDescriptor desc = WGPU_BIND_GROUP_DESCRIPTOR_INIT;
    desc.layout = g.cutBgl;
    desc.entryCount = 2;
    desc.entries = entries;
    WGPUBindGroup bg = wgpuDeviceCreateBindGroup(g.device, &desc);
    draw_fullscreen(ctx->pass, scene_pipeline(ctx->layout, Kind::Cut), bg);
    wgpuBindGroupRelease(bg);
}

void clear_draw(ModContext*, const GfxDrawContext* ctx, const void* payload, size_t size, void*) {
    const bool white = size == sizeof(bool) && *static_cast<const bool*>(payload);
    const Kind kind = white ? (ctx->uses_reversed_z ? Kind::ClearWhiteRev : Kind::ClearWhiteStd)
                            : (ctx->uses_reversed_z ? Kind::ClearBlackRev : Kind::ClearBlackStd);
    WGPUBindGroup bg = make_bind_group(nullptr, nullptr);
    draw_fullscreen(ctx->pass, scene_pipeline(ctx->layout, kind), bg);
    wgpuBindGroupRelease(bg);
}

} // namespace

bool initialize(WGPUDevice device) {
    g.device = device;

    WGPUShaderSourceWGSL wgsl = WGPU_SHADER_SOURCE_WGSL_INIT;
    wgsl.code = sv(kShader);
    WGPUShaderModuleDescriptor smd = WGPU_SHADER_MODULE_DESCRIPTOR_INIT;
    smd.nextInChain = &wgsl.chain;
    smd.label = sv("Dusklight VR shaders");
    g.module = wgpuDeviceCreateShaderModule(device, &smd);

    WGPUSamplerDescriptor sd = WGPU_SAMPLER_DESCRIPTOR_INIT;
    sd.magFilter = WGPUFilterMode_Linear;
    sd.minFilter = WGPUFilterMode_Linear;
    sd.addressModeU = WGPUAddressMode_ClampToEdge;
    sd.addressModeV = WGPUAddressMode_ClampToEdge;
    g.sampler = wgpuDeviceCreateSampler(device, &sd);

    WGPUBindGroupLayoutEntry entries[3] = {WGPU_BIND_GROUP_LAYOUT_ENTRY_INIT, WGPU_BIND_GROUP_LAYOUT_ENTRY_INIT,
        WGPU_BIND_GROUP_LAYOUT_ENTRY_INIT};
    entries[0].binding = 0;
    entries[0].visibility = WGPUShaderStage_Fragment;
    entries[0].sampler.type = WGPUSamplerBindingType_Filtering;
    for (int i = 1; i < 3; ++i) {
        entries[i].binding = i;
        entries[i].visibility = WGPUShaderStage_Fragment;
        entries[i].texture.sampleType = WGPUTextureSampleType_Float;
        entries[i].texture.viewDimension = WGPUTextureViewDimension_2D;
    }
    WGPUBindGroupLayoutDescriptor bgld = WGPU_BIND_GROUP_LAYOUT_DESCRIPTOR_INIT;
    bgld.entryCount = 3;
    bgld.entries = entries;
    g.bgl = wgpuDeviceCreateBindGroupLayout(device, &bgld);

    WGPUPipelineLayoutDescriptor pld = WGPU_PIPELINE_LAYOUT_DESCRIPTOR_INIT;
    pld.bindGroupLayoutCount = 1;
    pld.bindGroupLayouts = &g.bgl;
    g.layout = wgpuDeviceCreatePipelineLayout(device, &pld);

    WGPUTextureDescriptor td = WGPU_TEXTURE_DESCRIPTOR_INIT;
    td.usage = WGPUTextureUsage_TextureBinding;
    td.size = {1, 1, 1};
    td.format = WGPUTextureFormat_RGBA8Unorm;
    g.dummy = wgpuDeviceCreateTexture(device, &td);
    g.dummyView = wgpuTextureCreateView(g.dummy, nullptr);
    td.usage = WGPUTextureUsage_TextureBinding | WGPUTextureUsage_CopyDst;
    g.keyTex = wgpuDeviceCreateTexture(device, &td);
    g.keyView = wgpuTextureCreateView(g.keyTex, nullptr);
    g.queue = wgpuDeviceGetQueue(device);

    // Tabletop cut: unfilterable R32Float depth snapshot + a uniform range from push_uniform.
    WGPUShaderSourceWGSL cutWgsl = WGPU_SHADER_SOURCE_WGSL_INIT;
    cutWgsl.code = sv(kCutShader);
    WGPUShaderModuleDescriptor cutSmd = WGPU_SHADER_MODULE_DESCRIPTOR_INIT;
    cutSmd.nextInChain = &cutWgsl.chain;
    cutSmd.label = sv("Dusklight VR tabletop cut");
    g.cutModule = wgpuDeviceCreateShaderModule(device, &cutSmd);

    WGPUBindGroupLayoutEntry cutEntries[2] = {WGPU_BIND_GROUP_LAYOUT_ENTRY_INIT, WGPU_BIND_GROUP_LAYOUT_ENTRY_INIT};
    cutEntries[0].binding = 0;
    cutEntries[0].visibility = WGPUShaderStage_Fragment;
    cutEntries[0].texture.sampleType = WGPUTextureSampleType_UnfilterableFloat;
    cutEntries[0].texture.viewDimension = WGPUTextureViewDimension_2D;
    cutEntries[1].binding = 1;
    cutEntries[1].visibility = WGPUShaderStage_Fragment;
    cutEntries[1].buffer.type = WGPUBufferBindingType_Uniform;
    cutEntries[1].buffer.minBindingSize = sizeof(CutParams);
    WGPUBindGroupLayoutDescriptor cutBgld = WGPU_BIND_GROUP_LAYOUT_DESCRIPTOR_INIT;
    cutBgld.entryCount = 2;
    cutBgld.entries = cutEntries;
    g.cutBgl = wgpuDeviceCreateBindGroupLayout(device, &cutBgld);
    WGPUPipelineLayoutDescriptor cutPld = WGPU_PIPELINE_LAYOUT_DESCRIPTOR_INIT;
    cutPld.bindGroupLayoutCount = 1;
    cutPld.bindGroupLayouts = &g.cutBgl;
    g.cutLayout = wgpuDeviceCreatePipelineLayout(device, &cutPld);

    GfxDeviceInfo info = GFX_DEVICE_INFO_INIT;
    if (svc_gfx->get_device_info(mod_ctx, &info) == MOD_OK) {
        g.reversedZ = info.uses_reversed_z;
    }

    GfxDrawTypeDesc mirror = GFX_DRAW_TYPE_DESC_INIT;
    mirror.label = "Dusklight VR mirror";
    mirror.draw = mirror_draw;
    GfxDrawTypeDesc restore = GFX_DRAW_TYPE_DESC_INIT;
    restore.label = "Dusklight VR scene restore";
    restore.draw = restore_draw;
    GfxDrawTypeDesc clear = GFX_DRAW_TYPE_DESC_INIT;
    clear.label = "Dusklight VR eye clear";
    clear.draw = clear_draw;
    GfxDrawTypeDesc cutDesc = GFX_DRAW_TYPE_DESC_INIT;
    cutDesc.label = "Dusklight VR tabletop cut";
    cutDesc.draw = cut_draw;
    if (svc_gfx->register_draw_type(mod_ctx, &mirror, &g.mirrorDraw) != MOD_OK ||
        svc_gfx->register_draw_type(mod_ctx, &restore, &g.restoreDraw) != MOD_OK ||
        svc_gfx->register_draw_type(mod_ctx, &clear, &g.clearDraw) != MOD_OK ||
        svc_gfx->register_draw_type(mod_ctx, &cutDesc, &g.cutDraw) != MOD_OK)
    {
        mods::log::error("failed to register VR draw types");
        return false;
    }
    return g.module != nullptr && g.sampler != nullptr && g.bgl != nullptr && g.layout != nullptr &&
           g.cutModule != nullptr && g.cutLayout != nullptr;
}

void shutdown() {
    for (GfxDrawTypeHandle* h : {&g.mirrorDraw, &g.restoreDraw, &g.clearDraw, &g.cutDraw}) {
        if (*h != 0) {
            svc_gfx->unregister_draw_type(mod_ctx, *h);
            *h = 0;
        }
    }
    std::lock_guard lock{g.mutex};
    for (auto& [k, p] : g.targetPipelines) {
        wgpuRenderPipelineRelease(p);
    }
    for (auto& [k, p] : g.scenePipelines) {
        wgpuRenderPipelineRelease(p);
    }
    g.targetPipelines.clear();
    g.scenePipelines.clear();
    if (g.dummyView) wgpuTextureViewRelease(g.dummyView);
    if (g.dummy) wgpuTextureRelease(g.dummy);
    if (g.keyView) wgpuTextureViewRelease(g.keyView);
    if (g.keyTex) wgpuTextureRelease(g.keyTex);
    if (g.queue) wgpuQueueRelease(g.queue);
    g.keyView = nullptr;
    g.keyTex = nullptr;
    g.queue = nullptr;
    if (g.cutLayout) wgpuPipelineLayoutRelease(g.cutLayout);
    if (g.cutBgl) wgpuBindGroupLayoutRelease(g.cutBgl);
    if (g.cutModule) wgpuShaderModuleRelease(g.cutModule);
    if (g.layout) wgpuPipelineLayoutRelease(g.layout);
    if (g.bgl) wgpuBindGroupLayoutRelease(g.bgl);
    if (g.sampler) wgpuSamplerRelease(g.sampler);
    if (g.module) wgpuShaderModuleRelease(g.module);
    g.dummyView = nullptr;
    g.dummy = nullptr;
    g.cutLayout = nullptr;
    g.cutBgl = nullptr;
    g.cutModule = nullptr;
    g.layout = nullptr;
    g.bgl = nullptr;
    g.sampler = nullptr;
    g.module = nullptr;
    g.device = nullptr;
}

bool reversed_z() { return g.reversedZ; }

void blit(WGPUCommandEncoder encoder, WGPUTextureView src, WGPUTextureView dst, WGPUTextureFormat dstFormat,
    BlitMode mode, uint32_t keyColor) {
    if (src == nullptr || dst == nullptr) {
        return;
    }
    WGPUTextureView second = nullptr;
    if (mode == BlitMode::Key && g.keyView != nullptr) {
        if (keyColor != g.keyColor) {
            // Ordered before this frame's submit, which is when the encoder runs.
            const uint8_t px[4] = {static_cast<uint8_t>(keyColor >> 16), static_cast<uint8_t>(keyColor >> 8),
                static_cast<uint8_t>(keyColor), 255};
            WGPUTexelCopyTextureInfo dstInfo = WGPU_TEXEL_COPY_TEXTURE_INFO_INIT;
            dstInfo.texture = g.keyTex;
            WGPUTexelCopyBufferLayout layout = WGPU_TEXEL_COPY_BUFFER_LAYOUT_INIT;
            layout.bytesPerRow = 256;
            layout.rowsPerImage = 1;
            const WGPUExtent3D size{1, 1, 1};
            wgpuQueueWriteTexture(g.queue, &dstInfo, px, sizeof(px), &layout, &size);
            g.keyColor = keyColor;
        }
        second = g.keyView;
    }
    const Kind kind = mode == BlitMode::KeepAlpha ? Kind::BlitAlpha : mode == BlitMode::Key ? Kind::Key : Kind::Blit;
    WGPUBindGroup bg = make_bind_group(src, second);
    encode_target_pass(encoder, dst, target_pipeline(dstFormat, kind), bg,
        WGPUColor{0, 0, 0, mode == BlitMode::KeepAlpha ? 0.0 : 1.0});
    wgpuBindGroupRelease(bg);
}

void combine_hud(WGPUCommandEncoder encoder, WGPUTextureView black, WGPUTextureView white, WGPUTextureView dst,
    WGPUTextureFormat dstFormat) {
    if (black == nullptr || white == nullptr || dst == nullptr) {
        return;
    }
    WGPUBindGroup bg = make_bind_group(black, white);
    encode_target_pass(encoder, dst, target_pipeline(dstFormat, Kind::Combine), bg, WGPUColor{0, 0, 0, 0});
    wgpuBindGroupRelease(bg);
}

void push_mirror(const MirrorPayload& payload) {
    if (g.mirrorDraw != 0) {
        svc_gfx->push_draw(mod_ctx, g.mirrorDraw, &payload, sizeof(payload));
    }
}

void push_restore(WGPUTextureView scene, bool keepAlpha) {
    if (g.restoreDraw != 0 && scene != nullptr) {
        const RestorePayload p{scene, keepAlpha};
        svc_gfx->push_draw(mod_ctx, g.restoreDraw, &p, sizeof(p));
    }
}

void push_clear(bool white) {
    if (g.clearDraw != 0) {
        svc_gfx->push_draw(mod_ctx, g.clearDraw, &white, sizeof(white));
    }
}

void push_cut(WGPUTextureView depth, const CutParams& params) {
    if (g.cutDraw == 0 || depth == nullptr) {
        return;
    }
    CutPayload p{depth, {}};
    if (svc_gfx->push_uniform(mod_ctx, &params, sizeof(params), &p.uniform) != MOD_OK) {
        return;
    }
    svc_gfx->push_draw(mod_ctx, g.cutDraw, &p, sizeof(p));
}

} // namespace vr::gpu
