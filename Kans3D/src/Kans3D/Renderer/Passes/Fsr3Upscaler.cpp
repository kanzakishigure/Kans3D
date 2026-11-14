#include "kspch.h"
#include "Fsr3Upscaler.h"
#include <glm/gtc/packing.hpp>
#include "Kans3D/Renderer/Shading/FsrShaderSources.h"
#include "Kans3D/Renderer/Shading/InternalShader.h"
#include "Kans3D/Renderer/Temporal.h"

namespace Kans
{
    namespace
    {
        using namespace RenderHardware;
        constexpr const char* Entries[] = {"prepare_inputs",
                                           "luma_pyramid",
                                           "shading_change_pyramid",
                                           "shading_change",
                                           "prepare_reactivity",
                                           "luma_instability",
                                           "accumulate",
                                           "autogen_reactive",
                                           "combine_masks",
                                           "rcas"};
        constexpr const char* Names[]   = {"FSR prepare inputs",
                                           "FSR luma pyramid",
                                           "FSR shading change pyramid",
                                           "FSR shading change",
                                           "FSR prepare reactivity",
                                           "FSR luma instability",
                                           "FSR accumulate",
                                           "FSR generate reactive",
                                           "FSR combine masks",
                                           "FSR RCAS"};
        TextureDesc           Description(glm::uvec2 size, Format format)
        {
            TextureDesc d;
            d.Width       = std::max(1u, size.x);
            d.Height      = std::max(1u, size.y);
            d.PixelFormat = format;
            d.Usage       = RenderHardware::TextureUsage::Sampled | RenderHardware::TextureUsage::Storage |
                            RenderHardware::TextureUsage::CopySource | RenderHardware::TextureUsage::CopyDestination;
            return d;
        }
        BindingType Type(ShaderBindingKind kind)
        {
            switch (kind)
            {
                case ShaderBindingKind::UniformBuffer:
                    return BindingType::UniformBuffer;
                case ShaderBindingKind::SampledImage:
                    return BindingType::SampledTexture;
                case ShaderBindingKind::Sampler:
                    return BindingType::Sampler;
                case ShaderBindingKind::StorageImage:
                    return BindingType::StorageTexture;
                default:
                    throw std::logic_error("Unexpected FSR binding type");
            }
        }
    } // namespace
    Fsr3Constants MakeFsr3Constants(glm::uvec2           renderSize,
                                    glm::uvec2           displaySize,
                                    const glm::mat4&     projection,
                                    glm::vec2            jitter,
                                    TimeStep             delta,
                                    bool                 reset,
                                    const Fsr3Constants& previous)
    {
        if (!renderSize.x || !renderSize.y || !displaySize.x || !displaySize.y)
            throw std::invalid_argument("FSR requires nonzero render and display sizes");
        reset =
            reset || previous.RenderSize != glm::ivec2(renderSize) || previous.DisplaySize != glm::ivec2(displaySize);
        Fsr3Constants c;
        c.RenderSize = c.MaxRenderSize = renderSize;
        c.DisplaySize = c.MaxDisplaySize = displaySize;
        c.PreviousRenderSize             = reset ? c.RenderSize : previous.RenderSize;
        c.PreviousDisplaySize            = reset ? c.DisplaySize : previous.DisplaySize;
        // JitterProjection translates clip XY by +2*jitter/size. In Vulkan's top-left UVs
        // the raster moves +jitter pixels; FSR's unjittered sample = pixel - Jitter().
        c.Jitter         = jitter;
        c.PreviousJitter = reset ? jitter : previous.Jitter;
        // SceneVelocity is previousUV-currentUV, already UV units and unjittered.
        // This is the FINAL shader scale, after the SDK host's pixel-scale/size division.
        c.MotionVectorScale = {1, 1};
        c.DownscaleFactor   = glm::vec2(renderSize) / glm::vec2(displaySize);
        // DeviceProjection maps OpenGL [-1,1] depth to Vulkan [0,1]. These are exactly
        // reference setupDeviceDepthToViewSpaceDepthParams for finite, non-reversed Z:
        // c=f/(f-n), e=-fn/(f-n); viewDepth=e/(deviceDepth-c).
        c.DeviceToViewDepth = {
            (.5f - .5f * projection[2][2]), .5f * projection[3][2], 1.f / projection[0][0], 1.f / projection[1][1]};
        c.TanHalfFov       = c.DeviceToViewDepth.z;
        c.JitterPhaseCount = float(JitterPhaseCount(displaySize.x, renderSize.x));
        c.DeltaTime        = std::clamp(delta.GetSeconds(), 0.f, 1.f);
        // SceneHDR has no pre-exposure. User exposure is applied later by tone mapping;
        // FSR input exposure and deltaPreExposure are therefore both 1, independent of it.
        c.FrameIndex = reset ? 0 : previous.FrameIndex + 1;
        return c;
    }
    glm::uvec4 MakeFsr3RcasConstants(float sharpness)
    {
        // SDK FsrRcasCon: exp2(-((-2 * sharpness) + 2)); remaining words are FP16/unused.
        const float value = std::exp2(2.f * std::clamp(sharpness, 0.f, 1.f) - 2.f);
        uint32_t    bits;
        std::memcpy(&bits, &value, sizeof(bits));
        return {bits, glm::packHalf2x16(glm::vec2(value)), 0, 0};
    }
    Fsr3Upscaler::Fsr3Upscaler(RenderHardware::Device& device) : m_Device(device)
    {
        using namespace RenderHardware;
        // Bloom allocates immutable uploads per dispatch; there is no shared uniform arena.
        // Keep a frames-in-flight ring, but use recorded GPU updates (copied by UpdateBuffer)
        // instead of mapped writes. RHI transfer/read barriers and queue order protect reuse
        // even if this view is rendered at a different cadence than the device frame ring.
        m_Uniforms.resize(device.GetDesc().FramesInFlight);
        for (auto& uniform : m_Uniforms)
        {
            uniform.Constants = device.CreateBuffer(
                {sizeof(Fsr3Constants), BufferUsage::Uniform | BufferUsage::CopyDestination, MemoryUsage::DeviceLocal});
            uniform.Reactive = device.CreateBuffer(
                {16, BufferUsage::Uniform | BufferUsage::CopyDestination, MemoryUsage::DeviceLocal});
            uniform.Rcas = device.CreateBuffer(
                {16, BufferUsage::Uniform | BufferUsage::CopyDestination, MemoryUsage::DeviceLocal});
            uniform.Spd = device.CreateBuffer(
                {32, BufferUsage::Uniform | BufferUsage::CopyDestination, MemoryUsage::DeviceLocal});
        }
        SamplerDesc sampler;
        sampler.Min = sampler.Mag = Filter::Nearest;
        sampler.U = sampler.V = AddressMode::ClampToEdge;
        m_Point               = device.CreateSampler(sampler);
        sampler.Min = sampler.Mag                  = Filter::Linear;
        m_Linear                                   = device.CreateSampler(sampler);
        const auto&                        sources = GetEmbeddedFsrSources();
        std::map<std::string, std::string> includes;
        for (const auto& [path, source] : sources)
            if (path.rfind("gpu/", 0) == 0)
                includes.emplace(path.substr(4), source);
        // RCAS reads a cross around each pixel; samplerless loads need an explicit edge clamp.
        auto&             callbacks = includes.at("fsr3upscaler/ffx_fsr3upscaler_callbacks_glsl.h");
        const std::string rcasLoad  = "texelFetch(r_rcas_input, iPxPos, 0)";
        callbacks.replace(
            callbacks.find(rcasLoad),
            rcasLoad.size(),
            "texelFetch(r_rcas_input, clamp(iPxPos, FfxInt32x2(0), textureSize(r_rcas_input, 0) - 1), 0)");
        for (const auto* entry : Entries)
        {
            auto source = std::string(entry) == "combine_masks" ?
                              R"(#version 450
layout(local_size_x=8,local_size_y=8) in;
layout(set=0,binding=0) uniform texture2D r_auto_reactive;
layout(set=0,binding=1) uniform texture2D r_material_masks;
layout(set=0,binding=2,r8) writeonly uniform image2D rw_reactive_mask;
layout(set=0,binding=3,r8) writeonly uniform image2D rw_transparency_and_composition_mask;
void main(){ ivec2 p=ivec2(gl_GlobalInvocationID.xy); if(any(greaterThanEqual(p,imageSize(rw_reactive_mask))))return;
vec2 material=texelFetch(r_material_masks,p,0).rg;
// Composition changes invalidate accumulation as well as locks (the SDK T&C input controls locks).
imageStore(rw_reactive_mask,p,vec4(max(texelFetch(r_auto_reactive,p,0).r,max(material.r,material.g))));
imageStore(rw_transparency_and_composition_mask,p,vec4(material.g)); }
)" :
                              sources.at(std::string("shaders/ffx_fsr3upscaler_") + entry + "_pass.glsl");
            if (std::string(entry) == "combine_masks")
                source.insert(source.find('\n') + 1, "#extension GL_EXT_samplerless_texture_functions : require\n");
            if (std::string(entry) == "autogen_reactive")
                source.insert(source.find("    FfxFloat32x3 ColorPreAlpha"),
                              "    if (any(greaterThanEqual(uDispatchThreadId, FfxUInt32x2(RenderSize())))) return;\n");
            // FP32, no wave/subgroup requirements: lavapipe and portable Vulkan RHI.
            // FFX_WAVE must be UNDEFINED (the headers use defined(), not its value).
            // SPD uses shared-memory reductions. Analytic Lanczos avoids a lookup texture.
            // HDR input, render-resolution unjittered UV motion, conventional finite depth,
            // separate optional RCAS. Samplerless texture functions are core SPIR-V sampled images.
            source.insert(source.find('\n', source.find("#version")) + 1, R"(
#define FFX_GPU 1
#define FFX_GLSL 1
#define FFX_HALF 0
#define FFX_SPD_NO_WAVE_OPERATIONS 1
#define FFX_FSR3UPSCALER_OPTION_HDR_COLOR_INPUT 1
#define FFX_FSR3UPSCALER_OPTION_LOW_RESOLUTION_MOTION_VECTORS 1
#define FFX_FSR3UPSCALER_OPTION_JITTERED_MOTION_VECTORS 0
#define FFX_FSR3UPSCALER_OPTION_INVERTED_DEPTH 0
#define FFX_FSR3UPSCALER_OPTION_APPLY_SHARPENING 0
#define FFX_FSR3UPSCALER_OPTION_UPSAMPLE_USE_LANCZOS_TYPE 0
#define FFX_FSR3UPSCALER_OPTION_REPROJECT_USE_LANCZOS_TYPE 0
)");
            const auto& product = GetInternalComputeShaderData(std::string("FSR ") + entry, source, includes);
            if (auto* error = std::get_if<AssetError>(&product))
                throw std::runtime_error(error->Message);
            const auto& data = std::get<ShaderData>(product);
            Pass        pass;
            pass.Bindings = data.Reflection.at(0).Bindings;
            BindGroupLayoutDesc layout;
            for (const auto& binding : pass.Bindings)
                layout.Entries.push_back({binding.Binding, Type(binding.Kind)});
            pass.Layout   = device.CreateBindGroupLayout(layout);
            auto shader   = device.CreateShader(data);
            pass.Pipeline = device.CreateComputePipeline({shader, {pass.Layout}, 0, data.DebugName});
            m_Passes.push_back(std::move(pass));
        }
    }
    GraphTexture Fsr3Upscaler::AddPasses(RenderGraph& graph, const Inputs& in)
    {
        using namespace RenderHardware;
        const bool resized = m_RenderSize != in.RenderSize || m_DisplaySize != in.DisplaySize;
        m_Constants        = MakeFsr3Constants(
            in.RenderSize, in.DisplaySize, in.Projection, in.Jitter, in.FrameTimeDelta, in.Reset, m_Constants);
        const bool reset = m_Constants.FrameIndex == 0;
        if (resized)
        {
            m_Resources.clear();
            m_RenderSize  = in.RenderSize;
            m_DisplaySize = in.DisplaySize;
            for (int i = 0; i < 2; ++i)
            {
                const auto suffix             = std::to_string(i);
                m_Resources["accum" + suffix] = m_Device.CreateTexture(Description(in.RenderSize, Format::R8Unorm));
                m_Resources["luma" + suffix]  = m_Device.CreateTexture(Description(in.RenderSize, Format::R16Float));
                // The vendored GLSL callback explicitly declares rgba8, unlike the HLSL
                // host resource table's rgba16f. Match the GLSL image format exactly.
                m_Resources["lumaHistory" + suffix] =
                    m_Device.CreateTexture(Description(in.RenderSize, Format::RGBA8Unorm));
                m_Resources["history" + suffix] =
                    m_Device.CreateTexture(Description(in.DisplaySize, Format::RGBA16Float));
            }
            m_Resources["frameInfo"] = m_Device.CreateTexture(Description({1, 1}, Format::RGBA32Float));
            m_Resources["atomic"]    = m_Device.CreateTexture(Description({1, 1}, Format::R32Uint));
            m_Resources["black"]     = m_Device.CreateTexture(Description(in.RenderSize, Format::R8Unorm));
            m_Resources["exposure"]  = m_Device.CreateTexture(Description({1, 1}, Format::R32Float));
        }
        const auto& uniform   = m_Uniforms.at(m_Frame % m_Uniforms.size());
        const auto  constants = uniform.Constants;
        // ffxSpdSetup: ceil(render/64) groups, floor(log2(max(render))) mips, capped at 12.
        const glm::uvec2 spdGroups = (in.RenderSize + 63u) / 64u;
        struct alignas(16) SpdConstants
        {
            uint32_t   Mips, Groups;
            glm::uvec2 Offset;
            glm::uvec2 Size;
        };
        const SpdConstants spd {
            std::min(12u, uint32_t(std::floor(std::log2(std::max(in.RenderSize.x, in.RenderSize.y))))),
            spdGroups.x * spdGroups.y,
            {},
            in.RenderSize};
        const auto                  settings = in.Settings.Validated();
        const Fsr3ReactiveConstants reactive {
            settings.ReactiveScale, settings.ReactiveThreshold, settings.ReactiveBinaryValue, 13};
        const auto rcas           = MakeFsr3RcasConstants(settings.Sharpness);
        const auto reactiveBuffer = uniform.Reactive, rcasBuffer = uniform.Rcas;
        const auto graphReactive = graph.ImportBuffer("FSR reactive constants", reactiveBuffer);
        const auto graphRcas     = graph.ImportBuffer("FSR RCAS constants", rcasBuffer);
        const auto                          spdBuffer      = uniform.Spd;
        const auto                          graphConstants = graph.ImportBuffer("FSR constants", constants);
        const auto                          graphSpd       = graph.ImportBuffer("FSR SPD constants", spdBuffer);
        std::map<std::string, GraphTexture> images;
        for (const auto& [name, texture] : m_Resources)
            images[name] = graph.ImportTexture("FSR " + name, texture);
        const auto current = std::to_string(m_Frame % 2), previous = std::to_string((m_Frame + 1) % 2);
        images["r_accumulation"]  = images.at("accum" + previous);
        images["rw_accumulation"] = images.at("accum" + current);
        images["r_current_luma"] = images["rw_current_luma"] = images.at("luma" + current);
        images["r_previous_luma"]                            = images.at("luma" + previous);
        images["r_luma_history"]                             = images.at("lumaHistory" + previous);
        images["rw_luma_history"]                            = images.at("lumaHistory" + current);
        images["r_internal_upscaled_color"]                  = images.at("history" + previous);
        images["rw_internal_upscaled_color"]                 = images.at("history" + current);
        images["r_frame_info"] = images["rw_frame_info"] = images.at("frameInfo");
        images["rw_spd_global_atomic"]                   = images.at("atomic");
        images["r_auto_reactive"] = images["r_material_masks"] = images.at("black");
        const bool generateReactive                            = in.OpaqueOnly != uint32_t(-1);
        const bool combineMasks                                = generateReactive || in.MaterialMasks != uint32_t(-1);
        if (in.MaterialMasks != uint32_t(-1))
            images["r_material_masks"] = in.MaterialMasks;
        if (generateReactive)
            images["r_input_opaque_only"] = in.OpaqueOnly;
        images["r_reactive_mask"] = images["r_transparency_and_composition_mask"] = images.at("black");
        images["r_input_exposure"]                                                = images.at("exposure");
        images["r_lanczos_lut"]                                                   = images.at("black");
        images["r_input_color_jittered"] = graph.ImportTexture("SceneHDR", in.Color);
        images["r_input_depth"]          = graph.ImportTexture("SceneDepth", in.Depth);
        images["r_input_motion_vectors"] = graph.ImportTexture("SceneVelocity", in.MotionVectors);
        const auto transient             = [&](const std::string& name, glm::uvec2 size, Format format) {
            auto id             = graph.CreateTexture("FSR " + name, Description(size, format));
            images["r_" + name] = images["rw_" + name] = id;
            return id;
        };
        if (generateReactive)
        {
            const auto automatic      = transient("output_autoreactive", in.RenderSize, Format::R32Float);
            images["r_auto_reactive"] = automatic;
            // The SDK declares this unused UAV alongside autoreactive; the entry writes only autoreactive.
            images["rw_output_autocomposition"] = automatic;
        }
        if (combineMasks)
        {
            transient("reactive_mask", in.RenderSize, Format::R8Unorm);
            transient("transparency_and_composition_mask", in.RenderSize, Format::R8Unorm);
        }
        transient("reconstructed_previous_nearest_depth", in.RenderSize, Format::R32Uint);
        transient("dilated_motion_vectors", in.RenderSize, Format::RG16Float);
        transient("dilated_depth", in.RenderSize, Format::R32Float);
        transient("farthest_depth", in.RenderSize, Format::R16Float);
        transient("farthest_depth_mip1", in.RenderSize / 2u, Format::R16Float);
        transient("shading_change", in.RenderSize / 2u, Format::R8Unorm);
        transient("new_locks", in.DisplaySize, Format::R8Unorm);
        transient("dilated_reactive_masks", in.RenderSize, Format::RGBA8Unorm);
        transient("luma_instability", in.RenderSize, Format::R16Float);
        const auto output      = transient("upscaled_output", in.DisplaySize, Format::RGBA16Float);
        images["r_rcas_input"] = output;
        const auto sharpened =
            settings.Sharpness > 0 ?
                graph.CreateTexture("FSR sharpened output", Description(in.DisplaySize, Format::RGBA16Float)) :
                output;
        auto       pyramidDesc = Description(glm::max(in.RenderSize / 2u, glm::uvec2(1)), Format::RG16Float);
        pyramidDesc.MipLevels =
            std::min(6u, uint32_t(std::floor(std::log2(std::max(pyramidDesc.Width, pyramidDesc.Height)))) + 1);
        const auto pyramid   = graph.CreateTexture("FSR SPD mips", pyramidDesc);
        images["r_spd_mips"] = pyramid;
        for (int i = 0; i < 6; ++i)
            images["rw_spd_mip" + std::to_string(i)] = pyramid;
        // Clear persistent histories on reset as well as all atomic/discarded surfaces before
        // their first read. Explicit graph declarations keep the clears and histories alive.
        std::vector<GraphUse>     clearUses {{graphConstants, GraphAccess::TransferWrite},
                                             {graphSpd, GraphAccess::TransferWrite},
                                             {graphReactive, GraphAccess::TransferWrite},
                                             {graphRcas, GraphAccess::TransferWrite}};
        std::vector<GraphTexture> zeros;
        if (reset)
            for (const auto& [name, texture] : m_Resources)
                if (name != "frameInfo" && name != "exposure")
                    zeros.push_back(images.at(name));
        zeros.push_back(images.at("atomic"));
        zeros.push_back(pyramid);
        zeros.push_back(images.at("rw_new_locks"));
        std::sort(zeros.begin(), zeros.end());
        zeros.erase(std::unique(zeros.begin(), zeros.end()), zeros.end());
        for (auto id : zeros)
            clearUses.push_back({id, GraphAccess::StorageWrite});
        const auto reconstructed = images.at("rw_reconstructed_previous_nearest_depth");
        clearUses.push_back({reconstructed, GraphAccess::StorageWrite});
        if (reset)
        {
            clearUses.push_back({images.at("frameInfo"), GraphAccess::StorageWrite});
            clearUses.push_back({images.at("exposure"), GraphAccess::StorageWrite});
        }
        auto first =
            graph.AddPass("FSR clear",
                          GraphPassKind::Compute,
                          clearUses,
                          [zeros,
                           reconstructed,
                           reset,
                           frameInfo = images.at("frameInfo"),
                           exposure  = images.at("exposure"),
                           constants,
                           reactiveBuffer,
                           rcasBuffer,
                           reactive,
                           rcas,
                           spdBuffer,
                           parameters = m_Constants,
                           spd](auto& c) {
                              c.Commands.UpdateBuffer(reactiveBuffer, 0, &reactive, sizeof(reactive));
                              c.Commands.UpdateBuffer(rcasBuffer, 0, &rcas, sizeof(rcas));
                              c.Commands.UpdateBuffer(constants, 0, &parameters, sizeof(parameters));
                              c.Commands.UpdateBuffer(spdBuffer, 0, &spd, sizeof(spd));
                              for (auto id : zeros)
                                  c.Commands.ClearStorageTexture(c.Texture(id));
                              c.Commands.ClearStorageTexture(
                                  c.Texture(reconstructed), {}, 0x3f800000u); // floatBitsToUint(1), conventional depth
                              if (reset)
                              {
                                  c.Commands.ClearStorageTexture(c.Texture(frameInfo), {-1, 1, 0, 0});
                                  c.Commands.ClearStorageTexture(c.Texture(exposure), {1, 1, 1, 1});
                              }
                          });
        if (first)
            graph.OrderAfter(first, first - 1);
        for (size_t i : {7u, 8u, 0u, 1u, 2u, 3u, 4u, 5u, 6u, 9u})
        {
            if ((i == 7 && !generateReactive) || (i == 8 && !combineMasks) || (i == 9 && settings.Sharpness == 0))
                continue;
            if (i == 9)
                images["rw_upscaled_output"] = sharpened;
            const auto&           pass = m_Passes[i];
            std::vector<GraphUse> uses;
            for (const auto& binding : pass.Bindings)
                if (binding.Kind == ShaderBindingKind::SampledImage || binding.Kind == ShaderBindingKind::StorageImage)
                {
                    const bool readWrite = binding.Name == "rw_new_locks" || binding.Name == "rw_frame_info" ||
                                           binding.Name == "rw_spd_global_atomic" ||
                                           binding.Name.rfind("rw_spd_mip", 0) == 0 ||
                                           binding.Name == "rw_reconstructed_previous_nearest_depth";
                    uses.push_back({images.at(binding.Name),
                                    binding.Kind == ShaderBindingKind::SampledImage ? GraphAccess::SampledRead :
                                    readWrite                                       ? GraphAccess::StorageReadWrite :
                                                                                      GraphAccess::StorageWrite});
                }
            std::sort(uses.begin(), uses.end(), [](const auto& a, const auto& b) { return a.Texture < b.Texture; });
            uses.erase(std::unique(uses.begin(),
                                   uses.end(),
                                   [](const auto& a, const auto& b) { return a.Texture == b.Texture; }),
                       uses.end());
            uses.push_back({graphConstants, GraphAccess::UniformRead});
            if (i == 1 || i == 2)
                uses.push_back({graphSpd, GraphAccess::UniformRead});
            if (i == 7)
                uses.push_back({graphReactive, GraphAccess::UniformRead});
            if (i == 9)
                uses.push_back({graphRcas, GraphAccess::UniformRead});
            // History writes are observable next frame even if this frame's display is not exported.
            const auto groups = i == 9             ? (in.DisplaySize + 15u) / 16u :
                                (i == 1 || i == 2) ? spdGroups :
                                                     ((i == 3 ? glm::max(in.RenderSize / 2u, glm::uvec2(1)) :
                                                       i == 6 ? in.DisplaySize :
                                                                in.RenderSize) +
                                                      7u) /
                                                         8u;
            auto       id     = graph.AddPass(
                Names[i],
                GraphPassKind::Compute,
                std::move(uses),
                [this,
                 i,
                 images,
                 constants,
                 reactiveBuffer,
                 rcasBuffer,
                 spdBuffer,
                 groups,
                 mips = pyramidDesc.MipLevels](auto& c) {
                    const auto&   p = m_Passes[i];
                    BindGroupDesc group;
                    group.Layout = p.Layout;
                    for (const auto& binding : p.Bindings)
                    {
                        BindGroupEntry e;
                        e.Slot = binding.Binding;
                        if (binding.Kind == ShaderBindingKind::UniformBuffer)
                        {
                            if (binding.Name == "cbGenerateReactive_t")
                                e.BufferResource = reactiveBuffer;
                            else if (binding.Name == "cbRCAS_t")
                                e.BufferResource = rcasBuffer;
                            else if (binding.Name == "cbSPD_t")
                                e.BufferResource = spdBuffer;
                            else if (binding.Name == "cbFSR3UPSCALER_t")
                                e.BufferResource = constants;
                            else
                                throw std::logic_error("Unknown FSR uniform block: " + binding.Name);
                        }
                        else if (binding.Kind == ShaderBindingKind::Sampler)
                            e.SamplerResource = binding.Name == "s_PointClamp" ? m_Point : m_Linear;
                        else
                        {
                            e.TextureResource = c.Texture(images.at(binding.Name));
                            if (binding.Kind == ShaderBindingKind::StorageImage)
                                e.Subresource =
                                    TextureSubresource {binding.Name.rfind("rw_spd_mip", 0) == 0 ?
                                                            std::min(uint32_t(binding.Name.back() - '0'), mips - 1) :
                                                            0u,
                                                        0};
                        }
                        group.Entries.push_back(std::move(e));
                    }
                    c.Commands.BindComputePipeline(p.Pipeline);
                    c.Commands.BindGroup(c.Device.CreateBindGroup(group));
                    c.Commands.Dispatch(groups.x, groups.y, 1);
                },
                true);
            graph.OrderAfter(id, id - 1);
        }
        ++m_Frame;
        return sharpened;
    }
} // namespace Kans
