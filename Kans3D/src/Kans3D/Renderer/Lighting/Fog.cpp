#include "kspch.h"
#include "Fog.h"
#include <shaderc/shaderc.hpp>
#include <spirv_cross/spirv_glsl.hpp>
#include "Kans3D/Asset/Runtime/EngineShaderText.h"
#include "Kans3D/Renderer/RenderFrame.h"
#include "Kans3D/Renderer/Shading/InternalShader.h"
#include "AreaLights.h"
#include "FogShaders.h"
#include "ShadowMaps.h"

namespace Kans
{
    using namespace RenderHardware;
    std::vector<ViewFogVolume> SelectFogVolumes(std::vector<ViewFogVolume> volumes, glm::vec3 camera)
    {
        volumes.erase(std::remove_if(volumes.begin(),
                                     volumes.end(),
                                     [](const auto& v) {
                                         float d = glm::determinant(v.Transform);
                                         return !std::isfinite(d) || std::abs(d) < 1e-10f ||
                                                !std::isfinite(v.Medium.Density) || v.Medium.Density <= 0;
                                     }),
                      volumes.end());
        std::sort(volumes.begin(), volumes.end(), [&](const auto& a, const auto& b) {
            auto distance = [&](const auto& v) {
                auto d = glm::vec3(v.Transform[3]) - camera;
                return glm::dot(d, d);
            };
            auto da = distance(a), db = distance(b);
            return da != db ? da < db : uint64_t(a.EntityID) < uint64_t(b.EntityID);
        });
        if (volumes.size() > FogVolumeLimit)
            volumes.resize(FogVolumeLimit);
        return volumes;
    }
    namespace
    {
        struct Parameters
        {
            glm::mat4 InverseView, InverseProjection, PreviousVP, PreviousView;
            glm::vec4 Grid, Medium, AlbedoG, Temporal, Noise, Scroll, PreviousDepth;
        };
        struct SampleParameters
        {
            glm::mat4 InverseProjection;
            glm::vec4 RenderSizeJitter, Depth;
        };
        struct Volume
        {
            glm::mat4 WorldToLocal {1};
            glm::vec4 AlbedoDensity {0}, ShapeFalloff {0};
        };
        struct LightScales
        {
            glm::vec4 Points[4] {}, Spots[16] {}, Areas[4] {}, Directional {0};
        };
        template<class T>
        Handle<RenderHardware::Buffer> Upload(Device& device, const T& value)
        {
            auto buffer = device.CreateBuffer({sizeof(value), BufferUsage::Uniform, MemoryUsage::Upload});
            buffer->Write(0, &value, sizeof(value));
            return buffer;
        }
        TextureDesc GridDescription(glm::uvec3 size)
        {
            TextureDesc d;
            d.Dimension   = TextureDimension::Texture3D;
            d.Width       = size.x;
            d.Height      = size.y;
            d.Depth       = size.z;
            d.PixelFormat = Format::RGBA16Float;
            d.Usage       = RenderHardware::TextureUsage::Sampled | RenderHardware::TextureUsage::Storage |
                            RenderHardware::TextureUsage::CopySource;
            return d;
        }
        void Initialize(Device& device, FogState& state)
        {
            state.SampleLayout = device.CreateBindGroupLayout(
                {3, {{0, BindingType::UniformBuffer}, {1, BindingType::CombinedTextureSampler}}});
            SamplerDesc linear;
            linear.U = linear.V = linear.W = AddressMode::ClampToEdge;
            state.Linear                   = device.CreateSampler(linear);
            linear.U = linear.V = linear.W = AddressMode::Repeat;
            state.NoiseSampler             = device.CreateSampler(linear);
            BindGroupLayoutDesc engine {0,
                                        {{0, BindingType::UniformBuffer},
                                         {1, BindingType::UniformBuffer},
                                         {2, BindingType::CombinedTextureSampler}}};
            for (uint32_t slot : {5u, 6u, 7u, 8u, 9u, 10u, 15u, 16u, 17u})
                engine.Entries.push_back({slot, BindingType::CombinedTextureSampler});
            for (uint32_t slot :
                 {3u, 4u, 18u, 19u, 20u, 21u, 22u, 23u, 24u, 25u, 26u, 27u, 28u, 29u, 30u, 31u, 32u, 33u, 34u, 35u})
                engine.Entries.push_back({slot, BindingType::CombinedTextureSampler});
            state.EngineLayout   = device.CreateBindGroupLayout(engine);
            const char* stages[] = {
                FogShaders::Media, FogShaders::Lighting, FogShaders::Temporal, FogShaders::Integrate};
            for (unsigned i = 0; i < 4; ++i)
            {
                std::string source = "#version 450\n";
                if (i == 1)
                    source += "#include <Kans3D/Lighting.glsl>\n";
                source += FogShaders::Parameters;
                source += stages[i];
                const auto& data =
                    GetInternalComputeShaderData("Froxel fog " + std::to_string(i), source, GetEngineShaderIncludes());
                if (auto e = std::get_if<AssetError>(&data))
                    throw std::runtime_error(e->Message);
                auto                shader = device.CreateShader(std::get<ShaderData>(data));
                BindGroupLayoutDesc layout {1, {{0, BindingType::UniformBuffer}}};
                for (unsigned b = 1; b <= (i == 3 ? 2u : 3u); ++b)
                {
                    auto type = (i == 0 && b == 1) || (i == 1 && b == 2) || (i == 2 && b == 3) || (i == 3 && b == 2) ?
                                    BindingType::StorageTexture :
                                ((i == 0 || i == 1) && b == 3) ? BindingType::UniformBuffer :
                                                                 BindingType::CombinedTextureSampler;
                    layout.Entries.push_back({b, type});
                }
                state.ComputeLayouts[i] = device.CreateBindGroupLayout(layout);
                std::vector<Handle<BindGroupLayout>> groups {state.ComputeLayouts[i]};
                if (i == 1)
                    groups.insert(groups.begin(), state.EngineLayout);
                state.ComputePipelines[i] =
                    device.CreateComputePipeline({shader, groups, 0, "Froxel fog " + std::to_string(i)});
            }
            TextureDesc noise;
            noise.Dimension = TextureDimension::Texture3D;
            noise.Width = noise.Height = noise.Depth = 32;
            noise.PixelFormat                        = Format::R8Unorm;
            noise.Usage = RenderHardware::TextureUsage::Sampled | RenderHardware::TextureUsage::CopyDestination;
            state.Noise = device.CreateTexture(noise);
            std::array<uint8_t, 32 * 32 * 32> values;
            uint32_t                          h = 0x4f12e31a;
            for (auto& v : values)
            {
                h ^= h << 13;
                h ^= h >> 17;
                h ^= h << 5;
                v = uint8_t(h >> 24);
            }
            auto upload = device.CreateBuffer({values.size(), BufferUsage::CopySource, MemoryUsage::Upload});
            upload->Write(0, values.data(), values.size());
            auto commands = device.CreateCommandList();
            commands->CopyBufferToTexture(upload, 0, state.Noise, {});
            device.Submit(commands);
        }
        Handle<BindGroup> LightingGroup(RenderFrame& frame, FogState& state, const Parameters& p)
        {
            auto device           = frame.GetDevice();
            auto ef               = frame.View.LightingUniforms.value_or(MakeLightingUniforms(frame.View.Scene));
            ef.View               = glm::inverse(p.InverseView);
            ef.Projection         = glm::inverse(p.InverseProjection);
            ef.ViewProjection     = ef.Projection * ef.View;
            ef.InverseView        = p.InverseView;
            ef.InverseProjection  = p.InverseProjection;
            const float angle     = glm::radians(frame.View.Environment.Rotation);
            ef.EnvironmentInfo    = {frame.View.Environment.DiffuseIntensity,
                                     frame.View.Environment.SpecularIntensity,
                                     std::cos(angle),
                                     std::sin(angle)};
            auto          shadows = frame.View.Shadows ? frame.View.Shadows :
                                    frame.Assets()     ? frame.Assets()->GetShadowFallback() :
                                                         CreateShadowFallback(*device);
            auto          env     = frame.GetMaterialEnvironment();
            BindGroupDesc group;
            group.Layout  = state.EngineLayout;
            group.Entries = {{0, Upload(*device, ef)},
                             {1, Upload(*device, EngineObjectUniforms {})},
                             {2, {}, 0, 0, env[0], state.Linear}};
            for (uint32_t slot : {5u, 6u, 7u, 8u, 9u, 10u, 15u, 16u, 17u})
                group.Entries.push_back({slot,
                                         {},
                                         0,
                                         0,
                                         slot == 16 ? shadows->Cookies :
                                         slot >= 15 ? shadows->Spots :
                                         slot <= 6  ? shadows->Directional :
                                                      shadows->Points[slot - 7],
                                         slot == 16              ? shadows->CookieSampler :
                                         slot == 6 || slot == 17 ? shadows->Raw :
                                                                   shadows->Compare});
            group.Entries.push_back({3, {}, 0, 0, env[1], state.Linear});
            group.Entries.push_back({4, {}, 0, 0, env[2], state.Linear});
            auto area = frame.View.AreaLighting ? frame.View.AreaLighting :
                        frame.Assets()          ? frame.Assets()->GetAreaLightFallback() :
                                                  CreateAreaLightFallback(*device);
            group.Entries.push_back({18, {}, 0, 0, area->Matrix, area->Sampler});
            group.Entries.push_back({19, {}, 0, 0, area->Amplitude, area->Sampler});
            for (uint32_t i = 0; i < EngineAreaLightLimit; ++i)
                group.Entries.push_back({20 + i, {}, 0, 0, area->Sources[i], area->Sampler});
            return device->CreateBindGroup(group);
        }
    } // namespace
    void BuildFogPasses(RenderGraph& graph, RenderFrame& frame)
    {
        auto&      storage  = frame.Output.GetFogStorage();
        const auto settings = frame.View.Environment.Fog.Validated();
        if (!settings.Enabled)
        {
            if (storage)
                storage->Valid = false;
            frame.Fog.reset();
            return;
        }
        auto device = frame.GetDevice();
        if (!storage)
        {
            auto fresh = std::make_shared<FogState>();
            Initialize(*device, *fresh);
            storage = std::move(fresh);
        }
        auto state            = storage;
        frame.Fog             = state;
        auto       size       = FogGridSize(frame.View.Viewport, settings);
        const auto projection = DeviceProjection(frame.View.Scene.sceneCamera.camera.GetProjectionMatrix(),
                                                 device->GetCoordinateConventions());
        const auto view       = frame.View.Scene.sceneCamera.viewMatrix;
        bool       valid      = state->Valid && state->Size == size && state->RenderSize == frame.View.Viewport &&
                                state->Settings == settings && state->World == &frame.World &&
                                state->LastFrame + 1 == frame.Output.GetFrameNumber() && !frame.View.CameraCut &&
                                state->PreviousProjection == projection;
        auto       desc       = GridDescription(size);
        if (state->Size != size)
        {
            for (auto& h : state->History)
                h = device->CreateTexture(desc);
            state->Integrated = device->CreateTexture(desc);
            state->Size       = size;
        }
        Parameters p;
        p.InverseView       = glm::inverse(view);
        p.InverseProjection = glm::inverse(projection);
        p.PreviousVP        = state->PreviousProjection * state->PreviousView;
        p.PreviousView      = state->PreviousView;
        p.Grid              = {glm::vec3(size), settings.VolumetricFogDistance};
        p.Medium            = {settings.Density, settings.HeightFalloff, settings.BaseHeight, settings.Extinction};
        p.AlbedoG           = {settings.Albedo, settings.Anisotropy};
        p.Temporal          = {
            settings.HistoryWeight, valid ? 1 : 0, frame.View.Time, float(frame.Output.GetFrameNumber() % 1024)};
        auto volumes = SelectFogVolumes(frame.View.FogVolumes, frame.View.Scene.sceneCamera.Position);
        std::array<Volume, FogVolumeLimit> packed;
        for (size_t i = 0; i < volumes.size(); ++i)
        {
            const auto& v           = volumes[i];
            packed[i].WorldToLocal  = glm::inverse(v.Transform);
            packed[i].AlbedoDensity = {glm::clamp(v.Medium.Albedo, glm::vec3(0), glm::vec3(1)), v.Medium.Density};
            packed[i].ShapeFalloff  = {
                v.Medium.Shape == FogVolumeShape::Sphere ? 1 : 0, std::clamp(v.Medium.EdgeFalloff, 0.f, 1.f), 0, 0};
        }
        p.Noise         = {settings.NoiseScale, settings.NoiseAmount, settings.Noise ? 1 : 0, float(volumes.size())};
        p.Scroll        = glm::vec4(settings.NoiseScroll, 0);
        p.PreviousDepth = {state->Settings.VolumetricFogDistance, 0, 0, 0};
        auto        uniform = Upload(*device, p), volumeBuffer = Upload(*device, packed);
        LightScales scales;
        auto        positive = [](float v) { return std::isfinite(v) ? std::max(0.f, v) : 0.f; };
        const auto& scene    = frame.View.Scene;
        for (size_t i = 0; i < std::min(scene.pointLights.size(), size_t(EnginePointLightLimit)); ++i)
            scales.Points[i / 4][i % 4] = positive(scene.pointLights[i].VolumetricScattering);
        if (scene.pointLights.empty())
            scales.Points[0][0] = positive(scene.pointLight.VolumetricScattering);
        for (size_t i = 0; i < std::min(scene.spotLights.size(), size_t(EngineSpotLightLimit)); ++i)
            scales.Spots[i / 4][i % 4] = positive(scene.spotLights[i].VolumetricScattering);
        for (size_t i = 0; i < std::min(scene.areaLights.size(), size_t(EngineAreaLightLimit)); ++i)
            scales.Areas[i / 4][i % 4] = positive(scene.areaLights[i].VolumetricScattering);
        scales.Directional.x   = positive(scene.dirLight.VolumetricScattering);
        auto       lightBuffer = Upload(*device, scales);
        auto       engine      = LightingGroup(frame, *state, p);
        const auto media       = graph.CreateTexture("Fog media sigma_s sigma_t", desc),
                   raw         = graph.CreateTexture("Fog lit scattering", desc);
        unsigned   parity      = unsigned(frame.Output.GetFrameNumber() & 1);
        const auto history     = graph.ImportTexture("Fog previous scattering", state->History[1 - parity]);
        const auto filtered    = graph.ImportTexture("Fog temporal scattering", state->History[parity]);
        const auto integrated  = graph.ImportTexture("Fog integrated scattering transmittance", state->Integrated);
        const auto noise       = graph.ImportTexture("Fog tiled density noise", state->Noise);
        // The first frame has no sampled history. Bind current raw data in its place to avoid reading an undefined
        // image.
        auto add = [&](unsigned                                       stage,
                       std::string                                    name,
                       std::vector<GraphUse>                          uses,
                       std::vector<std::pair<unsigned, GraphTexture>> images) {
            uses.push_back({graph.ImportBuffer("Fog parameters", uniform), GraphAccess::UniformRead});
            const auto pass =
                graph.AddPass(std::move(name),
                              GraphPassKind::Compute,
                              std::move(uses),
                              [state, uniform, volumeBuffer, lightBuffer, engine, stage, images, size](auto& c) {
                                  BindGroupDesc group;
                                  group.Layout  = state->ComputeLayouts[stage];
                                  group.Entries = {{0, uniform}};
                                  for (auto [binding, id] : images)
                                      group.Entries.push_back({binding,
                                                               {},
                                                               0,
                                                               0,
                                                               c.Texture(id),
                                                               binding == (stage == 0 ? 1u :
                                                                           stage == 2 ? 3u :
                                                                                        2u) ?
                                                                   Handle<Sampler> {} :
                                                               stage == 0 ? state->NoiseSampler :
                                                                            state->Linear});
                                  if (stage < 2)
                                      group.Entries.push_back({3, stage == 0 ? volumeBuffer : lightBuffer});
                                  c.Commands.BindComputePipeline(state->ComputePipelines[stage]);
                                  if (stage == 1)
                                      c.Commands.BindGroup(engine);
                                  c.Commands.BindGroup(c.Device.CreateBindGroup(group));
                                  const uint32_t tile = stage == 3 ? 8 : 4;
                                  c.Commands.Dispatch((size.x + tile - 1) / tile,
                                                      (size.y + tile - 1) / tile,
                                                      stage == 3 ? 1 : (size.z + 3) / 4);
                              });
            if (pass)
                graph.OrderAfter(pass, pass - 1);
        };
        add(0,
            "Fog media",
            {{media, GraphAccess::StorageWrite},
             {noise, GraphAccess::SampledRead},
             {graph.ImportBuffer("Fog volumes", volumeBuffer), GraphAccess::UniformRead}},
            {{1, media}, {2, noise}});
        frame.FogGrid = {media,
                         raw,
                         filtered,
                         integrated,
                         size,
                         p.InverseView,
                         p.InverseProjection,
                         settings.VolumetricFogDistance,
                         uint32_t(p.Temporal.w)};
        if (frame.View.InjectFogDensity)
            frame.View.InjectFogDensity(graph, frame.FogGrid);
        std::vector<GraphUse> lightingUses {
            {media, GraphAccess::SampledRead},
            {raw, GraphAccess::StorageWrite},
            {graph.ImportBuffer("Fog light scales", lightBuffer), GraphAccess::UniformRead}};
        const auto lightingImage = [&](const Handle<RenderHardware::Texture>& image) {
            if (image)
            {
                const auto id = graph.ImportTexture("Fog light input", image);
                if (std::none_of(lightingUses.begin(), lightingUses.end(), [&](const auto& use) {
                        return !use.IsBuffer && use.Texture == id;
                    }))
                    lightingUses.push_back({id, GraphAccess::SampledRead});
            }
        };
        for (auto image : frame.GetMaterialEnvironment())
            lightingImage(image);
        if (frame.View.Shadows)
        {
            const auto& s = *frame.View.Shadows;
            for (auto image : {s.Directional, s.Spots, s.Cookies, s.Points[0], s.Points[1], s.Points[2], s.Points[3]})
                lightingImage(image);
        }
        add(1, "Fog lighting", std::move(lightingUses), {{1, media}, {2, raw}});
        std::vector<GraphUse> temporalUses {{raw, GraphAccess::SampledRead}, {filtered, GraphAccess::StorageWrite}};
        if (valid)
            temporalUses.push_back({history, GraphAccess::SampledRead});
        add(2, "Fog temporal", std::move(temporalUses), {{1, raw}, {2, valid ? history : raw}, {3, filtered}});
        add(3,
            "Fog integrate",
            {{filtered, GraphAccess::SampledRead}, {integrated, GraphAccess::StorageWrite}},
            {{1, filtered}, {2, integrated}});
        graph.ExportTexture(filtered);
        graph.ExportTexture(integrated);
        SampleParameters sample {glm::inverse(frame.SceneProjection()),
                                 {glm::vec2(frame.View.Viewport), frame.Output.GetJitter()},
                                 {settings.VolumetricFogDistance,
                                  float(size.z),
                                  ToneMappingPass::EffectiveExposure(frame.View.GetPostProcessSettings().Exposure),
                                  float(frame.View.GetPostProcessSettings().ToneMapping)}};
        state->SampleUniforms = Upload(*device, sample);
        state->SampleGroup    = device->CreateBindGroup(
            {state->SampleLayout, {{0, state->SampleUniforms}, {1, {}, 0, 0, state->Integrated, state->Linear}}});
        state->World              = &frame.World;
        state->Settings           = settings;
        state->PreviousView       = view;
        state->PreviousProjection = projection;
        state->RenderSize         = frame.View.Viewport;
        state->LastFrame          = frame.Output.GetFrameNumber();
        state->Valid              = true;
    }
    void AddFogBackground(RenderGraph& graph, RenderFrame& frame)
    {
        if (!frame.Fog)
            return;
        auto       state = frame.Fog;
        const auto hdr   = frame.Samples > 1 && !frame.MSAAResolved ?
                               frame.MSHDR :
                               graph.ImportTexture("SceneHDR", frame.Output.GetHDR());
        if (!frame.PostProcessHDR)
        {
            const auto display = frame.Samples > 1 && !frame.MSAAResolved ?
                                     frame.MSColors[0] :
                                     graph.ImportTexture("SceneColor0", frame.Output.GetColors()[0]);
            auto       pass    = graph.AddPass("Fog HDR prefill",
                                               {{display, GraphAccess::SampledRead}, {hdr, GraphAccess::ColorWrite}},
                                               [&frame](auto& c) {
                                          frame.Services.GetToneMapping().Record(
                                              c.Device,
                                              c.Commands,
                                              frame.Colors()[0],
                                              {frame.HDR()},
                                              frame.View.GetPostProcessSettings().Exposure,
                                              true,
                                              frame.View.GetPostProcessSettings().ToneMapping);
                                               });
            if (pass)
                graph.OrderAfter(pass, pass - 1);
        }
        const auto integrated = graph.ImportTexture("Fog integrated scattering transmittance", state->Integrated);
        auto       pass       = graph.AddPass(
            "Fog background",
            {{integrated, GraphAccess::SampledRead}, {hdr, GraphAccess::ColorReadWrite}},
            [state, &frame, hdr](auto& c) {
                const auto source =
                    std::string("#version 450\n") + ShaderText::FogSampling +
                    "\nlayout(location=0) out vec4 color; void main(){ color=texture(kans_FogIntegrated,"
                    "vec3((gl_FragCoord.xy-kans_Fog.renderSizeJitter.zw)/kans_Fog.renderSizeJitter.xy,1)); }\n";
                static std::map<const Device*, std::weak_ptr<Shader>> cache;
                auto                                                  program = cache[&c.Device].lock();
                if (!program)
                {
                    auto shader = CreateInternalShader(
                        c.Device,
                        "Fog background",
                        "#version 450\nvoid main(){vec2 "
                        "p=vec2((gl_VertexIndex<<1)&2,gl_VertexIndex&2);gl_Position=vec4(p*2-1,0,1);}",
                        source);
                    if (auto e = std::get_if<AssetError>(&shader))
                        throw std::runtime_error(e->Message);
                    program          = std::get<Handle<Shader>>(shader);
                    cache[&c.Device] = program;
                }
                PipelineDesc d;
                d.Program = program;
                // Explicit set numbers allow the RHI to supply empty layouts for 0..2.
                d.GroupLayouts    = {state->SampleLayout};
                d.Samples         = frame.Samples;
                d.ColorFormats    = {Format::RGBA16Float};
                d.Rasterizer.Cull = CullMode::None;
                d.Blends.resize(1);
                auto& b                = d.Blends[0];
                b.Enabled              = true;
                b.SourceColor          = BlendFactor::One;
                b.DestinationColor     = BlendFactor::SourceAlpha;
                b.WriteMask            = 7;
                frame.DeferToneMapping = frame.PostProcessHDR = true;
                RenderingDesc render;
                render.Colors.push_back({c.Texture(hdr), {}, LoadOp::Load});
                c.Commands.BeginRendering(render);
                c.Commands.SetViewport({0, 0, float(frame.View.Viewport.x), float(frame.View.Viewport.y)});
                c.Commands.SetScissor({0, 0, frame.View.Viewport.x, frame.View.Viewport.y});
                c.Commands.BindPipeline(frame.Services.CachedPipeline(d));
                c.Commands.BindGroup(state->SampleGroup);
                c.Commands.Draw(3);
                c.Commands.EndRendering();
            });
        if (pass)
            graph.OrderAfter(pass, pass - 1);
        frame.DeferToneMapping = frame.PostProcessHDR = true;
    }
    std::variant<ShaderData, AssetError> MakeFogShader(const ShaderProgramCode& code, uint32_t alphaMode)
    {
        try
        {
            const bool display = (alphaMode & 256) != 0;
            alphaMode &= 255;
            ShaderData result;
            result.Code      = code;
            result.DebugName = "Fog surface";
            for (auto& module : result.Code.Modules)
            {
                if (module.Stage != ShaderProductStage::Fragment)
                    continue;
                spirv_cross::CompilerGLSL cross(module.Words);
                auto                      options = cross.get_common_options();
                options.version                   = 450;
                options.vulkan_semantics          = true;
                cross.set_common_options(options);
                std::string output;
                for (const auto& r : cross.get_shader_resources().stage_outputs)
                    if (cross.get_decoration(r.id, spv::DecorationLocation) == 0 &&
                        cross.get_type(r.base_type_id).vecsize == 4)
                    {
                        cross.set_name(r.id, "kans_FogSurface");
                        output = "kans_FogSurface";
                    }
                if (output.empty())
                    throw std::runtime_error("Fog material has no vec4 colour output");
                auto source = cross.compile();
                auto at     = source.find("void main()");
                if (at == std::string::npos)
                    throw std::runtime_error("Fog material has no main");
                source.replace(at, 11, "void kans_FogMaterialMain()");
                source += ShaderText::FogSampling;
                source += "\nvoid main(){kans_FogMaterialMain();";
                if (display)
                    source += alphaMode == 7 ? "kans_FogSurface.rgb=KansFogDecode(kans_FogSurface.rgb/"
                                               "max(kans_FogSurface.a,1e-8))*kans_FogSurface.a;" :
                                               "kans_FogSurface.rgb=KansFogDecode(kans_FogSurface.rgb);";
                source += "vec4 fog=KansFogAt(gl_FragCoord.z);kans_FogSurface.rgb*=fog.a;";
                if (alphaMode != 6)
                    source +=
                        std::string("kans_FogSurface.rgb+=fog.rgb*") + (alphaMode == 7 ? "kans_FogSurface.a;" : "1.0;");
                source += "}\n";
                shaderc::Compiler       compiler;
                shaderc::CompileOptions compile;
                compile.SetTargetEnvironment(shaderc_target_env_vulkan, shaderc_env_version_vulkan_1_2);
                auto compiled = compiler.CompileGlslToSpv(source, shaderc_fragment_shader, "FogSurface.frag", compile);
                if (compiled.GetCompilationStatus() != shaderc_compilation_status_success)
                    throw std::runtime_error(compiled.GetErrorMessage());
                module.Words = {compiled.cbegin(), compiled.cend()};
            }
            auto reflection = InspectShaderProgram(result.Code);
            if (auto e = std::get_if<AssetError>(&reflection))
                return *e;
            result.Reflection = std::get<std::vector<ShaderStageReflection>>(std::move(reflection));
            return result;
        }
        catch (const std::exception& e)
        {
            return AssetError {AssetErrorCode::InvalidData, e.what()};
        }
    }
} // namespace Kans
