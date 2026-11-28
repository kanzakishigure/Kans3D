#include "KansEditorCore/Profiling/BenchmarkRun.h"
#include <cstring>
#include <fstream>
#include <set>
#include <glm/gtc/matrix_transform.hpp>
#include "Kans3D/Asset/Runtime/MaterialParameterCollection.h"
#include "Kans3D/Asset/Runtime/StandardShader.h"
#include "Kans3D/Core/Debug/ApplicationFrameProfile.h"
#include "Kans3D/Project/Project.h"
#include "Kans3D/RHI/Vulkan/VulkanDevice.h"
#include "Kans3D/Renderer/RenderWorld.h"
#include "Kans3D/Renderer/Renderer.h"
#include "Kans3D/Renderer/ViewTarget.h"
#include "Kans3D/Scene/SceneSerializer.h"
#include "Kans3D/Text/FontAsset.h"
#include "Kans3D/UI/UIAsset.h"
#include "Kans3D/VFX/VfxSystemAsset.h"
#include "KansEditorCore/EditorAssetImportService.h"
#include "KansEditorCore/Profiling/BenchmarkScene.h"
#include "KansEditorCore/Profiling/FrameProfilerModel.h"

#ifdef _WIN32
#include <Windows.h>
#include <intrin.h>
#elif defined(__linux__)
#include <sys/utsname.h>
#endif

namespace Kans::Benchmark
{
    namespace fs = std::filesystem;
    namespace RH = RenderHardware;
    namespace
    {
        template<class T>
        T Checked(std::variant<T, AssetError> value)
        {
            if (const auto* error = std::get_if<AssetError>(&value))
                throw std::runtime_error(error->Message);
            return std::get<T>(std::move(value));
        }

        std::string CpuName()
        {
#if defined(_WIN32) && (defined(_M_X64) || defined(_M_IX86))
            int registers[4] {};
            __cpuid(registers, int(0x80000000u));
            if (uint32_t(registers[0]) >= 0x80000004u)
            {
                char brand[49] {};
                for (int i = 0; i < 3; ++i)
                {
                    __cpuid(registers, int(0x80000002u) + i);
                    std::memcpy(brand + i * sizeof(registers), registers, sizeof(registers));
                }
                std::string name(brand);
                const auto  start = name.find_first_not_of(' ');
                if (start != std::string::npos)
                    return name.substr(start, name.find_last_not_of(' ') - start + 1);
            }
#elif defined(__linux__)
            std::ifstream cpu("/proc/cpuinfo");
            for (std::string line; std::getline(cpu, line);)
                if (line.rfind("model name", 0) == 0 || line.rfind("Hardware", 0) == 0)
                {
                    const auto colon = line.find(':');
                    const auto start = line.find_first_not_of(" \t", colon + 1);
                    if (colon != std::string::npos && start != std::string::npos)
                        return line.substr(start);
                }
#endif
            return "unknown";
        }

        std::string OsName()
        {
#ifdef _WIN32
            const auto module     = GetModuleHandleW(L"ntdll.dll");
            using VersionFunction = LONG(WINAPI*)(OSVERSIONINFOW*);
            const auto version =
                module ? reinterpret_cast<VersionFunction>(GetProcAddress(module, "RtlGetVersion")) : nullptr;
            OSVERSIONINFOW info {};
            info.dwOSVersionInfoSize = sizeof(info);
            if (version && version(&info) == 0)
                return "Windows " + std::to_string(info.dwMajorVersion) + "." + std::to_string(info.dwMinorVersion) +
                       "." + std::to_string(info.dwBuildNumber);
            return "Windows";
#elif defined(__linux__)
            utsname info {};
            if (uname(&info) == 0)
                return std::string(info.sysname) + " " + info.release + " " + info.machine;
            return "Linux";
#else
            return "unknown";
#endif
        }

        YAML::Node Machine(const RH::Device& device)
        {
            const auto vulkan = RH::GetVulkanDeviceIdentity(device);
            YAML::Node node;
            node["gpu"]           = JsonString(vulkan.GPU);
            node["driver"]        = JsonString(vulkan.Driver);
            node["api"]           = JsonString("Vulkan");
            node["api_version"]   = JsonString(vulkan.ApiVersion);
            node["cpu"]           = JsonString(CpuName());
            node["os"]            = JsonString(OsName());
            node["build_config"]  = JsonString(KANS_BENCHMARK_BUILD);
            node["engine_commit"] = std::string_view(KANS_BENCHMARK_COMMIT).empty() ? YAML::Node(YAML::NodeType::Null) :
                                                                                      JsonString(KANS_BENCHMARK_COMMIT);
            node["vulkan_validation"] = vulkan.Validation;
            node["tracy_build"] =
#ifdef TRACY_ENABLE
                true;
#else
                false;
#endif
            return node;
        }

        std::vector<Entity> Entities(Scene& scene)
        {
            auto entities = scene.GetRootEntities();
            for (size_t i = 0; i < entities.size(); ++i)
            {
                auto children = scene.GetChildren(entities[i]);
                entities.insert(entities.end(), children.begin(), children.end());
            }
            return entities;
        }

        Entity FindCamera(const std::vector<Entity>& entities)
        {
            Entity camera;
            for (auto entity : entities)
                if (entity.HasComponent<CameraComponent, TagComponent>() &&
                    entity.GetComponent<TagComponent>().Tag == "BenchmarkCamera" &&
                    (!camera || uint64_t(entity.GetUUID()) < uint64_t(camera.GetUUID())))
                    camera = entity;
            return camera;
        }

        void StartTimelines(Scene& scene, const std::vector<Entity>& entities)
        {
            for (auto entity : entities)
                if (entity.HasComponent<TimelinePlayerComponent>() &&
                    entity.GetComponent<TimelinePlayerComponent>().PlayOnStart)
                {
                    auto& timeline       = entity.GetComponent<TimelinePlayerComponent>();
                    timeline.ClockSource = TimelineClockSource::Timeline;
                    if (!scene.PrepareTimeline(entity) || !timeline.Player->Play())
                        throw std::runtime_error("Cannot start benchmark timeline on entity " +
                                                 std::to_string(uint64_t(entity.GetUUID())));
                }
        }

        struct CameraPath
        {
            Entity Authored;
            Camera Lens;
            Bounds WorldBounds;

            void Apply(Scene& scene, RenderView& view, double phase) const
            {
                auto& camera = view.Scene.sceneCamera;
                if (Authored)
                {
                    const auto transform = scene.GetWorldTransform(Authored);
                    camera.camera        = Authored.GetComponent<CameraComponent>().SceneCamera;
                    camera.Position      = glm::vec3(transform[3]);
                    camera.viewMatrix    = glm::inverse(transform);
                    return;
                }
                camera.camera     = Lens;
                camera.Position   = OrbitPosition(WorldBounds, phase);
                const auto center = WorldBounds.IsValid() ? WorldBounds.Center() : glm::vec3(0);
                camera.viewMatrix = glm::lookAt(camera.Position, center, glm::vec3(0, 1, 0));
            }
        };

        CameraPath MakeCameraPath(Scene& scene, glm::uvec2 size, const std::vector<Entity>& entities)
        {
            CameraPath path;
            path.Authored = FindCamera(entities);
            if (path.Authored)
                return path;
            if (const auto primary = scene.GetCameraEntity())
                path.Lens = primary.GetComponent<CameraComponent>().SceneCamera;
            for (auto entity : scene.GetRootEntities())
                path.WorldBounds.Expand(scene.GetWorldBounds(entity));
            const float radius = path.WorldBounds.IsValid() ? (std::max)(path.WorldBounds.Radius(), 2.f) : 2.f;
            path.Lens.SetProjectionMatirx(glm::perspective(
                glm::radians(55.f), float(size.x) / float(size.y), (std::max)(.025f, radius / 5000.f), radius * 8.f));
            return path;
        }

        const char* AaName(AntiAliasing aa)
        {
            switch (aa)
            {
                case AntiAliasing::None:
                    return "None";
                case AntiAliasing::FXAA:
                    return "FXAA";
                case AntiAliasing::MSAA2:
                    return "MSAA2";
                case AntiAliasing::MSAA4:
                    return "MSAA4";
                case AntiAliasing::MSAA8:
                    return "MSAA8";
                case AntiAliasing::TAA:
                    return "TAA";
            }
            throw std::logic_error("Invalid anti-aliasing mode");
        }

        YAML::Node Resolution(glm::uvec2 size)
        {
            YAML::Node node;
            node["width"]  = size.x;
            node["height"] = size.y;
            return node;
        }

        YAML::Node ShadowProfiles(Scene& scene)
        {
            using Profile = std::tuple<std::string_view, uint32_t, uint32_t, uint32_t>;
            std::set<Profile> profiles;
            const auto        add = [&](std::string_view type, const ShadowSettings& input) {
                const auto shadow = input.Validated();
                if (shadow.Enabled)
                    profiles.emplace(
                        type, shadow.Resolution, uint32_t(shadow.Filter), type == "directional" ? shadow.Cascades : 0u);
            };
            for (auto entity : Entities(scene))
            {
                if (entity.HasComponent<DirLightComponent>())
                    add("directional", entity.GetComponent<DirLightComponent>().Shadows);
                if (entity.HasComponent<PointLightComponent>())
                    add("point", entity.GetComponent<PointLightComponent>().Shadows);
                if (entity.HasComponent<SpotLightComponent>())
                    add("spot", entity.GetComponent<SpotLightComponent>().Shadows);
            }
            YAML::Node nodes(YAML::NodeType::Sequence);
            for (const auto& [type, resolution, filter, cascades] : profiles)
            {
                YAML::Node node;
                node["type"]       = JsonString(type);
                node["resolution"] = resolution;
                node["filter"]     = filter;
                if (cascades)
                    node["cascades"] = cascades;
                nodes.push_back(node);
            }
            return nodes;
        }

        YAML::Node Settings(const Options&         options,
                            const CameraPath&      path,
                            const RenderView&      view,
                            Scene&                 scene,
                            const FrameStatistics& effective)
        {
            YAML::Node node;
            node["scene"]                  = JsonString(options.Scene);
            node["resolution"]             = Resolution(view.Viewport);
            node["render_resolution"]      = Resolution(effective.RenderSize);
            node["frames"]                 = options.Frames;
            node["warmup"]                 = options.Warmup;
            node["fixed_timestep_seconds"] = FixedStepSeconds;
            node["mode"]                   = JsonString("offscreen_render_only");
            node["vsync"]                  = false;
            node["camera_path"]            = JsonString(path.Authored ? "BenchmarkCamera" : "bounds_orbit");
            if (path.Authored)
                node["camera_entity"] = uint64_t(path.Authored.GetUUID());
            const auto& lens                   = view.Scene.sceneCamera.camera;
            node["anti_aliasing"]["requested"] = JsonString(AaName(lens.GetAntiAliasing()));
            node["anti_aliasing"]["effective"] = JsonString(AaName(effective.EffectiveAA));
            node["anti_aliasing"]["samples"]   = effective.EffectiveSamples;
            constexpr const char* modes[] {
                "Native", "Quality", "Balanced", "Performance", "UltraPerformance", "Custom"};
            const auto upscale                      = lens.GetUpscaling();
            const bool taa                          = effective.EffectiveAA == AntiAliasing::TAA;
            node["upscaler"]["name"]                = JsonString(taa ? "FSR3" : "None");
            node["upscaler"]["requested_mode"]      = JsonString(modes[size_t(upscale.Mode)]);
            node["upscaler"]["effective_ratio"]     = taa ? upscale.Ratio() : 1.f;
            const auto& environment                 = scene.GetEnvironmentSettings();
            const auto  post                        = view.GetPostProcessSettings();
            auto        quality                     = node["quality"];
            quality["pipeline"]                     = JsonString(view.Pipeline);
            quality["shadow_profiles"]              = ShadowProfiles(scene);
            quality["depth_prepass"]                = view.DepthPrepass;
            quality["cache_static_shadows"]         = view.CacheStaticShadows;
            quality["batch_static_meshes"]          = view.BatchStaticMeshes;
            quality["spot_shadow_atlas_size"]       = view.SpotShadowAtlasSize;
            quality["max_shadowed_point_lights"]    = view.MaxShadowedPointLights;
            quality["max_shadowed_spot_lights"]     = view.MaxShadowedSpotLights;
            quality["beam_half_resolution"]         = view.BeamHalfResolution;
            quality["environment_map"]              = JsonString(environment.Map.generic_u8string());
            quality["skybox"]                       = environment.ShowSkybox;
            quality["fog"]                          = environment.Fog.Enabled;
            quality["fog_grid_pixel_size"]          = environment.Fog.GridPixelSize;
            quality["fog_depth_slices"]             = environment.Fog.DepthSlices;
            quality["gtao"]                         = environment.AmbientOcclusion.Enabled;
            quality["gtao_full_resolution"]         = environment.AmbientOcclusion.FullResolution;
            quality["gtao_slices"]                  = environment.AmbientOcclusion.Slices;
            quality["gtao_steps"]                   = environment.AmbientOcclusion.Steps;
            quality["ssr"]                          = environment.ScreenSpaceReflections.Enabled;
            quality["ssr_half_resolution"]          = environment.ScreenSpaceReflections.HalfResolution;
            quality["ssr_steps"]                    = environment.ScreenSpaceReflections.MaxSteps;
            quality["bloom"]                        = post.Bloom.Enabled;
            quality["bloom_mip_count"]              = effective.BloomMipCount;
            quality["bloom_high_quality_filtering"] = post.Bloom.HighQualityFiltering;
            quality["depth_of_field"]               = post.DepthOfField.Active();
            quality["depth_of_field_samples"]       = post.DepthOfField.Samples;
            quality["tone_mapping"]                 = uint32_t(post.ToneMapping);
            quality["exposure_mode"]                = uint32_t(post.Exposure.Mode);
            return node;
        }

        YAML::Node Run(Scene&                             scene,
                       AssetManager&                      assets,
                       const Options&                     options,
                       const std::shared_ptr<RH::Device>& device,
                       const ComplexSceneData*            complex)
        {
            const glm::uvec2 resolution {options.Width, options.Height};
            scene.SetPrefabAssetManager(&assets);
            scene.UpdatePrefabs();
            scene.OnViewportResize(resolution.x, resolution.y);
            const auto entities = Entities(scene);
            StartTimelines(scene, entities);
            scene.UpdateAnimations(0);
            scene.UpdateTransformConstraints(0);
            scene.UpdateBounds();
            const auto path                              = MakeCameraPath(scene, resolution, entities);
            auto&      world                             = scene.GetRenderWorld();
            world.Settings.Piplinestate.EnableGridShader = false;
            auto       renderer                          = Renderer::Acquire(device);
            ViewTarget target(device, 1);
            RenderView view;
            view.Viewport                             = resolution;
            view.FrameTimeDelta                       = TimeStep(FixedStepSeconds);
            view.Assets                               = renderer->AcquireAssetCache(assets);
            view.SpotShadowAtlasSize                  = complex ? 1024 : view.SpotShadowAtlasSize;
            constexpr size_t             PassCapacity = 512;
            FrameProfiling::FrameHistory history(options.Frames, PassCapacity, 256 * 1024);
            FrameStatistics              effective;

            const uint64_t end = uint64_t(options.Warmup) + options.Frames;
            for (uint64_t i = 0; i < end; ++i)
            {
                KANS_PROFILE_SCOPE("Benchmark frame");
                const bool measured    = i >= options.Warmup;
                const auto frame       = i + 1;
                const auto before      = device->GetFrameCounters();
                uint64_t   shaderStart = 0;
                KANS_FRAME_STAT(shaderStart = ProfileShaderCompiles;)
                const auto   start = RenderClockMilliseconds();
                const double time  = double(frame) * FixedStepSeconds;
                if (complex)
                    complex->Advance(time);
                else
                {
                    scene.UpdatePrefabs();
                    scene.UpdateTimelines(FixedStepSeconds);
                    scene.UpdateAnimations(FixedStepSeconds);
                    scene.UpdateTransformConstraints(FixedStepSeconds);
                    scene.UpdateMaterialParameterDrivers();
                }
                const double phase =
                    measured ? double(i - options.Warmup) / double(options.Frames) : double(i) / double(options.Warmup);
                path.Apply(scene, view, phase);
                if (!complex)
                    scene.UpdateVfxPreview(float(FixedStepSeconds), view.Scene.sceneCamera.Position);
                view.Scene.PostProcess  = scene.GetPostProcessSettings();
                view.Time               = float(time);
                view.RenderTextureFrame = frame;
                const auto result       = renderer->Render(world, view, target);
                const auto cpu          = RenderClockMilliseconds() - start;
                if (measured)
                {
                    if (!result.Complete)
                        throw std::runtime_error("Scene render incomplete during measurement");
                    if (result.FrameStats.Passes.size() > PassCapacity)
                        throw std::runtime_error("Benchmark exceeded frame history pass capacity");
                    uint64_t shaderCompiles = 0;
                    KANS_FRAME_STAT(shaderCompiles = ProfileShaderCompiles - shaderStart;)
                    (void)shaderStart;
                    history.Record(frame, &result.FrameStats);
                    history.AttachApplication(
                        {frame, cpu, 1, shaderCompiles, device->GetFrameCounters().Since(before)});
                    if (i == options.Warmup)
                    {
                        effective.RenderSize       = result.FrameStats.RenderSize;
                        effective.EffectiveAA      = result.FrameStats.EffectiveAA;
                        effective.EffectiveSamples = result.FrameStats.EffectiveSamples;
                        effective.BloomMipCount    = result.FrameStats.BloomMipCount;
                    }
                }
                KANS_PROFILE_FRAME();
            }

            // EndFrame retires the headless submission. Poll the final sample without rendering a tail frame.
            auto& profiler = target.GetFrameProfiler();
            profiler.Poll(target.GetFrameNumber() + 1);
            if (profiler.Latest())
                history.AttachGpu(*profiler.Latest());
            return MakeReport(history, Machine(*device), Settings(options, path, view, scene, effective));
        }

        void BuildReferencedSources(Project& project, const std::vector<AssetID>& references)
        {
            std::set<SourceAssetID> pending;
            const auto              registry = project.GetRuntimeAssetRegistry();
            for (auto id : references)
            {
                const auto source = project.GetSourceAssetDatabase().FindByID(id.GetSourceID());
                if (source.Exists && !registry->Find(id, EditorAssetTarget))
                    pending.insert(id.GetSourceID());
            }
            AssetBuildOptions build;
            build.Target = EditorAssetTarget;
            for (const auto id : pending)
                Checked(project.BuildAsset(id, build));
        }

        void Preload(AssetManager& assets, AssetID id, std::set<AssetID>& seen)
        {
            if (!seen.insert(id).second || IsStandardShader(id))
                return;
            auto record = assets.GetRegistry()->Find(id, assets.GetTarget());
            if (!record) // Legacy mesh part IDs are resolved via their model when the prefab expands.
                return;
            for (auto dependency : record->Metadata.Dependencies)
                Preload(assets, dependency, seen);
            switch (record->Metadata.Type)
            {
                case AssetType::Model:
                    Checked(assets.Load<ModelAsset>(id));
                    break;
                case AssetType::StaticMesh:
                    Checked(assets.Load<MeshAsset>(id));
                    break;
                case AssetType::Material:
                    Checked(assets.Load<MaterialDataAsset>(id));
                    break;
                case AssetType::Shader:
                    Checked(assets.Load<ShaderDataAsset>(id));
                    break;
                case AssetType::Texture:
                    Checked(assets.Load<TextureAsset>(id));
                    break;
                case AssetType::Skeleton:
                    Checked(assets.Load<SkeletonAsset>(id));
                    break;
                case AssetType::AnimationClip:
                    Checked(assets.Load<AnimationClipAsset>(id));
                    break;
                case AssetType::AnimationController:
                    Checked(assets.Load<AnimationControllerAsset>(id));
                    break;
                case AssetType::Timeline:
                    Checked(assets.Load<TimelineAsset>(id));
                    break;
                case AssetType::Font:
                    Checked(assets.Load<FontAsset>(id));
                    break;
                case AssetType::MaterialParameterCollection:
                    Checked(assets.Load<MaterialParameterCollectionAsset>(id));
                    break;
                case AssetType::UI:
                    Checked(assets.Load<UIAsset>(id));
                    break;
                case AssetType::VfxSystem:
                    Checked(assets.Load<VfxSystemAsset>(id));
                    break;
                default:
                    break;
            }
        }
    } // namespace

    YAML::Node RunComplex(const Options& options, const std::shared_ptr<RH::Device>& device)
    {
        auto assets                 = PrepareComplexAssets();
        auto scene                  = BuildComplex(options.Complex);
        auto report                 = Run(*scene.World, *assets, options, device, &scene);
        auto settings               = report["settings"]["complex"];
        settings["instances"]       = options.Complex.Instances;
        settings["lights"]          = options.Complex.Lights;
        settings["shadowed_lights"] = options.Complex.ShadowedLights;
        settings["seed"]            = options.Complex.Seed;
        return report;
    }

    YAML::Node RunProjectScene(Project&                           project,
                               const fs::path&                    sceneFile,
                               const Options&                     options,
                               const std::shared_ptr<RH::Device>& device)
    {
        std::optional<AssetManager> assets;
        auto                        scene      = Checked(SceneSerializer::DeserializeChecked(sceneFile));
        const auto                  references = scene->GetReferencedAssets();
        BuildReferencedSources(project, references);
        AssetManagerSpecification spec;
        spec.Target = EditorAssetTarget;
        assets.emplace(project.CreateAssetProvider(), project.GetRuntimeAssetRegistry(), spec);
        std::set<AssetID> loaded;
        for (auto id : references)
            Preload(*assets, id, loaded);
        scene->GetRenderWorld().EnvironmentRoot = project.GetConfig().RootDirectory;
        auto       reportOptions                = options;
        const auto relative                     = sceneFile.lexically_relative(project.GetConfig().RootDirectory);
        reportOptions.Scene                     = (relative.empty() ? sceneFile : relative).generic_u8string();
        FileDigestCache digests;
        const auto      digest             = Checked(digests.Digest(sceneFile));
        auto            report             = Run(*scene, *assets, reportOptions, device, nullptr);
        report["settings"]["scene_digest"] = JsonString(digest);
        return report;
    }

    int Finish(const Options& options, YAML::Node report)
    {
        int code = 0;
        if (!options.Baseline.empty())
        {
            const auto result = Compare(ReadReport(options.Baseline), report, options.ThresholdPercent);
            for (const auto& mismatch : result.Mismatches)
                CORE_WARN("BENCHMARK COMPARISON IS NOT LIKE FOR LIKE: {} differs", mismatch);
            CORE_INFO("{:<38} {:<15} {:>10} {:>10} {:>11} {:>10}  {}",
                      "Frame/pass",
                      "Mean metric",
                      "Baseline",
                      "Current",
                      "Delta ms",
                      "Delta %",
                      "Status");
            const auto format = [](std::optional<double> value) {
                return value ? fmt::format("{:+.3f}", *value) : std::string("-");
            };
            for (const auto& row : result.Rows)
                CORE_INFO("{:<38} {:<15} {:>10} {:>10} {:>11} {:>10}  {}",
                          row.Name,
                          row.Metric,
                          format(row.BaselineMs),
                          format(row.CurrentMs),
                          format(row.DeltaMs),
                          format(row.DeltaPercent),
                          row.Status);
            report["comparison"] = ComparisonJson(result, options.ThresholdPercent);
            code                 = result.ExitCode();
        }
        WriteReport(options.Output, report);
        const auto compiles = report["compiles"];
        if (compiles["shaders"].as<uint64_t>() || compiles["pipelines"].as<uint64_t>())
            CORE_WARN("Benchmark measured {} shader-stage and {} pipeline compiles",
                      compiles["shaders"].as<uint64_t>(),
                      compiles["pipelines"].as<uint64_t>());
        CORE_INFO("Benchmark report written to {}", options.Output.u8string());
        return code;
    }
} // namespace Kans::Benchmark
