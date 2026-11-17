#include "kspch.h"
#include "Fog.h"
#include <shaderc/shaderc.hpp>
#include <spirv_cross/spirv_glsl.hpp>
#include "Kans3D/Asset/Runtime/EngineShaderText.h"
#include "Kans3D/Renderer/Passes/ExposureBinding.h"
#include "Kans3D/Renderer/RenderFrame.h"
#include "Kans3D/Renderer/Shading/InternalShader.h"
#include "AreaLights.h"
#include "BeamShaders.h"
#include "FogInjection.h"
#include "FogShaders.h"
#include "ShadowMaps.h"

namespace Kans
{
    using namespace RenderHardware;
    glm::vec3 FogWindDisplacement(const std::vector<FieldRecord>& fields, double time)
    {
        glm::dvec3 displacement(0);
        if (!std::isfinite(time))
            return {};
        for (const auto& r : fields)
        {
            if (r.Kind.x != uint32_t(FieldType::Wind) || r.Kind.y != uint32_t(FieldShape::Infinite) ||
                !(r.Filter.y & uint32_t(FieldSystem::Particles)))
                continue;
            const auto  direction = glm::vec3(r.DirectionStrength);
            const float length    = glm::length(direction);
            const auto  axis      = glm::quat(r.Rotation.w, r.Rotation.x, r.Rotation.y, r.Rotation.z) *
                                    (length > 1e-6f ? direction / length : glm::vec3(0));
            auto        integral  = [time](double frequency, double phase) {
                const double w = frequency * 6.283185307179586;
                return std::abs(w) < 1e-8 ? time * std::sin(phase) : (std::cos(phase) - std::cos(w * time + phase)) / w;
            };
            displacement += glm::dvec3(axis) *
                            (double(r.DirectionStrength.w) * time + r.NoisePulse.z * integral(r.NoisePulse.w, 0));
            for (int i = 0; i < 3; ++i)
                displacement[i] += r.NoisePulse.x * integral(r.NoisePulse.y, i * 2.094395102393195);
        }
        return glm::vec3(displacement);
    }

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
    std::vector<uint32_t> SelectSpotBeams(const std::vector<SpotLight>& lights, glm::vec3 camera)
    {
        std::vector<uint32_t> result;
        for (uint32_t i = 0; i < std::min(lights.size(), size_t(EngineSpotLightLimit)); ++i)
        {
            const auto& l = lights[i];
            const auto  b = l.Beam.Validated();
            if (b.Enabled && b.EffectiveLength(l.Range) > 0 &&
                std::isfinite(glm::dot(l.Position - camera, l.Position - camera)))
                result.push_back(i);
        }
        std::sort(result.begin(), result.end(), [&](uint32_t a, uint32_t b) {
            auto da = glm::dot(lights[a].Position - camera, lights[a].Position - camera);
            auto db = glm::dot(lights[b].Position - camera, lights[b].Position - camera);
            if (da != db)
                return da < db;
            if (lights[a].EntityID != lights[b].EntityID)
                return uint64_t(lights[a].EntityID) < uint64_t(lights[b].EntityID);
            return a < b;
        });
        if (result.size() > SpotBeamLimit)
            result.resize(SpotBeamLimit);
        return result;
    }
    bool HasSpotBeams(const RenderFrame& frame)
    {
        return frame.View.Environment.Fog.Enabled &&
               !SelectSpotBeams(frame.View.Scene.spotLights, frame.View.Scene.sceneCamera.Position).empty();
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
            glm::vec4 AlbedoDensity {0}, ShapeFalloff {0}, Noise {0}, Scroll {0};
        };
        struct LightScales
        {
            glm::vec4 Points[4] {}, Spots[16] {}, Areas[4] {}, Directional {0};
        };
        template<class T>
        Handle<RenderHardware::Buffer> Upload(Device& device, const T& value, bool writable = false)
        {
            auto buffer =
                device.CreateBuffer({sizeof(value),
                                     writable ? BufferUsage::Uniform | BufferUsage::Storage : BufferUsage::Uniform,
                                     MemoryUsage::Upload});
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
                if (i == 0)
                    source += FogShaders::MediumSampling;
                if (i == 1)
                    source += FogShaders::LightSampling;
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
        Handle<BindGroupLayout> BeamLightingLayout(Device& device)
        {
            static std::map<const Device*, std::weak_ptr<BindGroupLayout>> cache;
            if (auto layout = cache[&device].lock())
                return layout;
            auto layout    = device.CreateBindGroupLayout({0,
                                                           {{0, BindingType::UniformBuffer},
                                                            {15, BindingType::CombinedTextureSampler},
                                                            {16, BindingType::CombinedTextureSampler}}});
            cache[&device] = layout;
            return layout;
        }
        Handle<Shader> BeamMarchShader(Device& device, const std::string& source)
        {
            const auto& product = GetInternalShaderData("Spot beam march", BeamShaders::Vertex, source);
            if (auto error = std::get_if<AssetError>(&product))
                throw std::runtime_error(error->Message);
            for (const auto& module : std::get<ShaderData>(product).Code.Modules)
                if (module.Stage == ShaderProductStage::Fragment)
                {
                    spirv_cross::CompilerGLSL cross(module.Words);
                    auto                      options = cross.get_common_options();
                    options.version                   = 450;
                    options.vulkan_semantics          = true;
                    cross.set_common_options(options);
                    // Retain the shared spot functions, but omit unused environment/area/point descriptors.
                    // The full lighting set plus three beam inputs would exceed a 32-sampler stage budget.
                    cross.set_enabled_interface_variables(cross.get_active_interface_variables());
                    auto result = CreateInternalShader(
                        device, "Spot beam march active bindings", BeamShaders::Vertex, cross.compile());
                    if (auto error = std::get_if<AssetError>(&result))
                        throw std::runtime_error(error->Message);
                    return std::get<Handle<Shader>>(result);
                }
            throw std::runtime_error("Spot beam march has no fragment stage");
        }
        Handle<BindGroup> LightingGroup(RenderFrame& frame, FogState& state, const Parameters& p, bool beam = false)
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
            if (beam)
                return device->CreateBindGroup({BeamLightingLayout(*device),
                                                {{0, Upload(*device, ef)},
                                                 {15, {}, 0, 0, shadows->Spots, shadows->Compare},
                                                 {16, {}, 0, 0, shadows->Cookies, shadows->CookieSampler}}});
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
        const auto                         wind = FogWindDisplacement(frame.View.FogWind, frame.View.Time);
        for (size_t i = 0; i < volumes.size(); ++i)
        {
            const auto& v           = volumes[i];
            packed[i].WorldToLocal  = glm::inverse(v.Transform);
            packed[i].AlbedoDensity = {glm::clamp(v.Medium.Albedo, glm::vec3(0), glm::vec3(1)), v.Medium.Density};
            const auto& m           = v.Medium;
            const float scale       = m.Noise ? m.NoiseScale : settings.NoiseScale;
            packed[i].Noise         = {
                scale, m.Noise ? m.NoiseAmount : settings.NoiseAmount, m.Noise || settings.Noise ? 1 : 0, 0};
            // Reuse the global lookup for inherited noise with the same advection settings.
            if (!m.Noise && m.FollowWind == settings.FollowWind)
                packed[i].Noise.z = -1;
            packed[i].Scroll        = glm::vec4(m.Noise || settings.Noise ?
                                                    (m.Noise ? m.NoiseScroll : settings.NoiseScroll) * frame.View.Time -
                                                        (m.FollowWind ? wind * scale : glm::vec3(0)) :
                                                    glm::vec3(0),
                                                0);
            packed[i].ShapeFalloff  = {
                v.Medium.Shape == FogVolumeShape::Sphere ? 1 : 0, std::clamp(v.Medium.EdgeFalloff, 0.f, 1.f), 0, 0};
        }
        p.Noise         = {settings.NoiseScale, settings.NoiseAmount, settings.Noise ? 1 : 0, float(volumes.size())};
        // Unused offsets must stay constant: they are part of the stationary beam history signature.
        p.Scroll = glm::vec4(settings.Noise ? settings.NoiseScroll * frame.View.Time -
                                                  (settings.FollowWind ? wind * settings.NoiseScale : glm::vec3(0)) :
                                              glm::vec3(0),
                             0);
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
        // Selected spots are replaced completely, including Intensity=0; overflow stays in froxels.
        // Extinction is never removed. Beam intensity scales only the replacement in-scattering.
        for (auto i : SelectSpotBeams(scene.spotLights, frame.View.Scene.sceneCamera.Position))
            scales.Spots[i / 4][i % 4] = 0;
        scales.Directional.x   = positive(scene.dirLight.VolumetricScattering);
        auto       lightBuffer = Upload(*device, scales);
        auto       engine      = LightingGroup(frame, *state, p);
        if (HasSpotBeams(frame))
        {
            state->BeamMediumSignature.clear();
            auto signature = [&](const auto& value) {
                state->BeamMediumSignature.append(reinterpret_cast<const char*>(&value), sizeof(value));
            };
            signature(p.Grid);
            signature(p.Medium);
            signature(p.AlbedoG);
            signature(p.Noise);
            signature(p.Scroll);
            signature(packed);
            signature(settings.VolumetricFogDistance);
            const void* world = &frame.World;
            signature(world);
            state->VolumeUniforms = volumeBuffer;
            state->BeamLighting   = LightingGroup(frame, *state, p, true);
        }
        else if (state->Beams)
            state->Beams->Valid = false;
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
        const auto sources = CollectFogInjectionSources(frame);
        if (!sources.empty())
            frame.FogGrid.Injected = AddFogInjection(graph, *device, frame.FogGrid, sources, state->Injection);
        if (HasSpotBeams(frame))
        {
            auto beamParameters              = p;
            beamParameters.InverseProjection = glm::inverse(frame.SceneProjection());
            const auto jitter                = frame.Output.GetJitter() / glm::vec2(frame.View.Viewport);
            beamParameters.PreviousDepth.y   = jitter.x;
            beamParameters.PreviousDepth.z   = jitter.y;
            beamParameters.PreviousDepth.w   = frame.FogGrid.Injected != uint32_t(-1) ? 1 : 0;
            state->BeamParameters            = Upload(*device, beamParameters);
            if (beamParameters.PreviousDepth.w > 0 && state->Beams)
                state->Beams->Valid = false; // Animated particle/grid density must not retain stale beams.
        }
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
        state->SampleUniforms = Upload(*device, sample, bool(frame.View.GPUExposure));
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
    void BuildSpotBeamPasses(RenderGraph& graph, RenderFrame& frame)
    {
        if (!frame.Fog || !HasSpotBeams(frame))
            return;
        auto fog    = frame.Fog;
        auto device = frame.GetDevice();
        if (!fog->Beams)
            fog->Beams = std::make_shared<BeamState>();
        auto       state    = fog->Beams;
        const auto selected = SelectSpotBeams(frame.View.Scene.spotLights, frame.View.Scene.sceneCamera.Position);
        const auto full     = frame.View.Viewport;
        const auto size     = frame.View.BeamHalfResolution ? glm::max((full + 1u) / 2u, glm::uvec2(1)) : full;
        struct PackedBeam
        {
            glm::vec4 IndexLengthScaleSamples {0}, FadesFlags {0};
        };
        struct BeamParameters
        {
            std::array<PackedBeam, SpotBeamLimit> Lights;
            glm::vec4                             RenderSize;
        };
        BeamParameters                     packed {};
        std::array<Scissor, SpotBeamLimit> bounds;
        auto       lighting = frame.View.LightingUniforms.value_or(MakeLightingUniforms(frame.View.Scene));
        const auto vp       = frame.SceneProjection() * frame.View.Scene.sceneCamera.viewMatrix;
        packed.RenderSize   = {glm::vec2(size), glm::vec2(full)};
        for (size_t n = 0; n < selected.size(); ++n)
        {
            const auto  i     = selected[n];
            const auto& l     = frame.View.Scene.spotLights[i];
            const auto  b     = l.Beam.Validated();
            const float scale = std::isfinite(l.VolumetricScattering) ? std::max(0.f, l.VolumetricScattering) : 0;
            packed.Lights[n].IndexLengthScaleSamples = {
                float(i), b.EffectiveLength(l.Range), b.Intensity * scale, float(b.Samples)};
            packed.Lights[n].FadesFlags = {b.StartFade, b.EndFade, b.UseShadow ? 1 : 0, b.UseCookie ? 1 : 0};
            bounds[n]                   = {0, 0, size.x, size.y};
            if (n > 0)
            {
                const float length = b.EffectiveLength(l.Range);
                const auto  axis   = glm::vec3(lighting.SpotDirectionOuter[i]);
                const float radius = length * std::tan(std::acos(lighting.SpotDirectionOuter[i].w));
                const auto  end    = l.Position + axis * length;
                const auto  lower  = glm::min(l.Position, end - glm::vec3(radius));
                const auto  upper  = glm::max(l.Position, end + glm::vec3(radius));
                glm::vec2   lo(1), hi(0);
                bool        crossesCamera = false;
                for (unsigned corner = 0; corner < 8; ++corner)
                {
                    const glm::vec3 p(
                        corner & 1 ? upper.x : lower.x, corner & 2 ? upper.y : lower.y, corner & 4 ? upper.z : lower.z);
                    const auto clip = vp * glm::vec4(p, 1);
                    if (clip.w <= 1e-5f)
                    {
                        crossesCamera = true;
                        break;
                    }
                    const auto uv = glm::vec2(clip) / clip.w * .5f + .5f;
                    lo            = glm::min(lo, uv);
                    hi            = glm::max(hi, uv);
                }
                if (!crossesCamera)
                {
                    const auto first = glm::clamp(glm::floor(lo * glm::vec2(size)), glm::vec2(0), glm::vec2(size));
                    const auto last  = glm::clamp(glm::ceil(hi * glm::vec2(size)), glm::vec2(0), glm::vec2(size));
                    bounds[n]        = {
                        int32_t(first.x), int32_t(first.y), uint32_t(last.x - first.x), uint32_t(last.y - first.y)};
                }
            }
        }
        const unsigned ms           = frame.Samples > 1 ? 1 : 0;
        const auto     shadows      = frame.View.Shadows ? frame.View.Shadows :
                                      frame.Assets()     ? frame.Assets()->GetShadowFallback() :
                                                           CreateShadowFallback(*device);
        const auto     depthSampler = shadows->Raw;
        if (!state->MarchLayout)
        {
            state->MarchLayout   = device->CreateBindGroupLayout({1,
                                                                  {{0, BindingType::UniformBuffer},
                                                                   {2, BindingType::CombinedTextureSampler},
                                                                   {3, BindingType::UniformBuffer},
                                                                   {4, BindingType::UniformBuffer},
                                                                   {5, BindingType::CombinedTextureSampler},
                                                                   {6, BindingType::CombinedTextureSampler},
                                                                   {7, BindingType::CombinedTextureSampler}}});
            state->ResolveLayout = device->CreateBindGroupLayout({1,
                                                                  {{0, BindingType::UniformBuffer},
                                                                   {1, BindingType::CombinedTextureSampler},
                                                                   {2, BindingType::CombinedTextureSampler},
                                                                   {3, BindingType::CombinedTextureSampler}}});
        }
        auto shader = [&](const char* name, const std::string& source) {
            auto result = CreateInternalShader(*device, name, BeamShaders::Vertex, source);
            if (auto e = std::get_if<AssetError>(&result))
                throw std::runtime_error(e->Message);
            return std::get<Handle<Shader>>(result);
        };
        if (!state->MarchShader[ms])
            state->MarchShader[ms] =
                BeamMarchShader(*device,
                                std::string("#version 450\n") + (ms ? "#define MULTISAMPLE_DEPTH\n" : "") +
                                    "#include <Kans3D/Lighting.glsl>\n" + FogShaders::Parameters +
                                    FogShaders::MediumSampling + FogShaders::LightSampling + BeamShaders::March);
        if (!state->TemporalShader)
            state->TemporalShader =
                shader("Spot beam temporal",
                       std::string("#version 450\n") + BeamShaders::ResolveParameters + BeamShaders::Temporal);
        if (!state->CompositeShader[ms])
            state->CompositeShader[ms] =
                shader("Spot beam bilateral composite",
                       std::string("#version 450\n") + (ms ? "#define MULTISAMPLE_DEPTH\n" : "") +
                           BeamShaders::ResolveParameters + BeamShaders::Composite);
        TextureDesc desc;
        desc.Width       = size.x;
        desc.Height      = size.y;
        desc.PixelFormat = Format::RGBA16Float;
        desc.Usage       = RenderHardware::TextureUsage::RenderTarget | RenderHardware::TextureUsage::Sampled |
                           RenderHardware::TextureUsage::CopySource;
        if (state->Size != size)
        {
            state->Valid = false;
            for (auto& h : state->History)
                h = device->CreateTexture(desc);
            state->Size = size;
        }
        // Stationary-view accumulation when TAA is off. Camera, lights, settings and media edits reset it.
        // Per-pixel depth rejection and tight current bounds also reject moving occluders and shadows.
        std::string signature       = fog->BeamMediumSignature;
        auto        appendSignature = [&](const auto& value) {
            signature.append(reinterpret_cast<const char*>(&value), sizeof(value));
        };
        appendSignature(packed);
        appendSignature(frame.View.Scene.sceneCamera.viewMatrix);
        appendSignature(frame.View.Scene.sceneCamera.camera.GetProjectionMatrix());

        for (auto i : selected)
        {
            appendSignature(lighting.SpotPositionRange[i]);
            appendSignature(lighting.SpotDirectionOuter[i]);
            appendSignature(lighting.SpotRadianceInner[i]);
            appendSignature(lighting.SpotViewProjection[i]);
            appendSignature(lighting.SpotCookieRect[i]);
            appendSignature(lighting.SpotShadowRect[i]);
            const auto& l = frame.View.Scene.spotLights[i];
            signature += l.Cookie.ToString();
            appendSignature(l.CookieRotation);
            appendSignature(l.CookieScale);
        }
        struct ResolveParameters
        {
            glm::mat4 InverseProjection;
            glm::vec4 Sizes, Temporal;
        };
        const bool        valid = state->Valid && state->LastFrame + 1 == frame.Output.GetFrameNumber() &&
                                  state->Signature == signature && !frame.View.CameraCut &&
                                  frame.View.EffectiveAA != AntiAliasing::TAA;
        ResolveParameters resolve {glm::inverse(frame.SceneProjection()),
                                   {glm::vec2(size), glm::vec2(full)},
                                   {valid ? 1 : 0, .85f, fog->Settings.VolumetricFogDistance, 0}};
        auto              beamBuffer = Upload(*device, packed), resolveBuffer = Upload(*device, resolve);
        const auto        current  = graph.CreateTexture("Beam current scattering depth", desc);
        const unsigned    parity   = unsigned(frame.Output.GetFrameNumber() & 1);
        const auto        filtered = graph.ImportTexture("Beam temporal scattering depth", state->History[parity]);
        const auto        history =
            valid ? graph.ImportTexture("Beam previous scattering depth", state->History[1 - parity]) : current;
        const auto depth      = ms ? frame.MSDepth : graph.ImportTexture("SceneDepthStencil", frame.Output.GetDepth());
        const auto hdr        = ms ? frame.MSHDR : graph.ImportTexture("SceneHDR", frame.Output.GetHDR());
        const auto noise      = graph.ImportTexture("Fog tiled density noise", fog->Noise);
        const auto integrated = graph.ImportTexture("Fog integrated scattering transmittance", fog->Integrated);
        const auto injected   = frame.FogGrid.Injected != uint32_t(-1) ? frame.FogGrid.Injected : integrated;
        auto       add        = [&](const char* name, std::vector<GraphUse> uses, RenderGraph::Record record) {
            auto pass = graph.AddPass(name, std::move(uses), std::move(record));
            if (pass)
                graph.OrderAfter(pass, pass - 1);
        };
        std::vector<GraphUse> uses {
            {current, GraphAccess::ColorWrite},
            {depth, GraphAccess::SampledRead},
            {noise, GraphAccess::SampledRead},
            {integrated, GraphAccess::SampledRead},
            {graph.ImportBuffer("Beam lights", beamBuffer), GraphAccess::UniformRead},
            {graph.ImportBuffer("Beam fog parameters", fog->BeamParameters), GraphAccess::UniformRead},
            {graph.ImportBuffer("Beam volumes", fog->VolumeUniforms), GraphAccess::UniformRead}};
        if (injected != integrated)
            uses.push_back({injected, GraphAccess::SampledRead});
        if (frame.View.Shadows)
            for (auto image : {frame.View.Shadows->Spots, frame.View.Shadows->Cookies})
                if (image)
                    uses.push_back({graph.ImportTexture("Beam lighting input", image), GraphAccess::SampledRead});
        add("Beam march", std::move(uses), [=, &frame](auto& c) {
            PipelineDesc pipeline;
            pipeline.Program           = state->MarchShader[ms];
            pipeline.GroupLayouts      = {BeamLightingLayout(c.Device), state->MarchLayout};
            pipeline.ColorFormats      = {Format::RGBA16Float};
            pipeline.Rasterizer.Cull   = CullMode::None;
            pipeline.PushConstantBytes = 4;
            pipeline.Blends.resize(1);
            auto& blend            = pipeline.Blends[0];
            blend.Enabled          = true;
            blend.SourceColor      = BlendFactor::One;
            blend.DestinationColor = BlendFactor::One;
            blend.SourceAlpha      = BlendFactor::One;
            blend.DestinationAlpha = BlendFactor::Zero;
            BindGroupDesc group {state->MarchLayout,
                                 {{0, fog->BeamParameters},
                                  {2, {}, 0, 0, c.Texture(noise), fog->NoiseSampler},
                                  {3, fog->VolumeUniforms},
                                  {4, beamBuffer},
                                  {5, {}, 0, 0, c.Texture(depth), depthSampler},
                                  {6, {}, 0, 0, c.Texture(integrated), fog->Linear},
                                  {7, {}, 0, 0, c.Texture(injected), fog->Linear}}};
            RenderingDesc render;
            render.Colors.push_back({c.Texture(current), {}, LoadOp::Clear});
            c.Commands.BeginRendering(render);
            c.Commands.SetViewport({0, 0, float(size.x), float(size.y)});
            c.Commands.SetScissor({0, 0, size.x, size.y});
            c.Commands.BindPipeline(frame.Services.CachedPipeline(pipeline));
            c.Commands.BindGroup(fog->BeamLighting);
            c.Commands.BindGroup(c.Device.CreateBindGroup(group));
            for (uint32_t i = 0; i < selected.size(); ++i)
            {
                if (!bounds[i].Width || !bounds[i].Height)
                    continue;
                c.Commands.SetScissor(bounds[i]);
                c.Commands.PushConstants(0, &i, sizeof(i));
                c.Commands.Draw(3);
            }
            c.Commands.EndRendering();
        });
        std::vector<GraphUse> temporalUses {
            {current, GraphAccess::SampledRead},
            {filtered, GraphAccess::ColorWrite},
            {graph.ImportBuffer("Beam resolve parameters", resolveBuffer), GraphAccess::UniformRead}};
        if (valid)
            temporalUses.push_back({history, GraphAccess::SampledRead});
        auto recordResolve = [=, &frame](bool composite, auto& c) {
            PipelineDesc pipeline;
            pipeline.Program         = composite ? state->CompositeShader[ms] : state->TemporalShader;
            pipeline.GroupLayouts    = {state->ResolveLayout};
            pipeline.ColorFormats    = {Format::RGBA16Float};
            pipeline.Rasterizer.Cull = CullMode::None;
            pipeline.Samples         = composite ? frame.Samples : 1;
            pipeline.Blends.resize(1);
            if (composite)
            {
                auto& blend            = pipeline.Blends[0];
                blend.Enabled          = true;
                blend.SourceColor      = BlendFactor::One;
                blend.DestinationColor = BlendFactor::One;
                blend.WriteMask        = 7;
            }
            BindGroupDesc group {state->ResolveLayout,
                                 {{0, resolveBuffer},
                                  {1, {}, 0, 0, c.Texture(composite ? filtered : current), fog->Linear},
                                  {2, {}, 0, 0, c.Texture(composite ? filtered : history), fog->Linear},
                                  {3, {}, 0, 0, c.Texture(depth), depthSampler}}};
            RenderingDesc render;
            render.Colors.push_back(
                {c.Texture(composite ? hdr : filtered), {}, composite ? LoadOp::Load : LoadOp::Clear});
            c.Commands.BeginRendering(render);
            auto extent = composite ? full : size;
            c.Commands.SetViewport({0, 0, float(extent.x), float(extent.y)});
            c.Commands.SetScissor({0, 0, extent.x, extent.y});
            c.Commands.BindPipeline(frame.Services.CachedPipeline(pipeline));
            c.Commands.BindGroup(c.Device.CreateBindGroup(group));
            c.Commands.Draw(3);
            c.Commands.EndRendering();
        };
        add("Beam temporal", std::move(temporalUses), [recordResolve](auto& c) { recordResolve(false, c); });
        add("Beam bilateral composite",
            {{filtered, GraphAccess::SampledRead},
             {depth, GraphAccess::SampledRead},
             {hdr, GraphAccess::ColorReadWrite},
             {graph.ImportBuffer("Beam resolve parameters", resolveBuffer), GraphAccess::UniformRead}},
            [recordResolve](auto& c) { recordResolve(true, c); });
        graph.ExportTexture(filtered);
        state->Signature       = std::move(signature);
        state->Valid           = true;
        state->LastFrame       = frame.Output.GetFrameNumber();
        frame.DeferToneMapping = frame.PostProcessHDR = true;
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
                                              frame.View.GetPostProcessSettings().ToneMapping,
                                              frame.View.GetPostProcessSettings().ColorGrading,
                                              frame.View.Assets &&
                                                      frame.View.GetPostProcessSettings().ColorGrading.LUT.IsValid() ?
                                                  frame.View.Assets->GetTextureResource(
                                                      frame.View.GetPostProcessSettings().ColorGrading.LUT) :
                                                  RenderHardware::Handle<RenderHardware::Texture> {},
                                              frame.View.GPUExposure);
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
                PatchFrameExposure(frame,
                                   c.Commands,
                                   state->SampleUniforms,
                                   1,
                                   0,
                                   offsetof(SampleParameters, Depth) + 2 * sizeof(float));
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
