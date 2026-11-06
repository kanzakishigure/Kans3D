#pragma once
#include <filesystem>
#include <map>
#include <tuple>
#include "Kans3D/Core/Window.h"
#include "Kans3D/RHI/Backend.h"
#include "Kans3D/Renderer/Lighting/Environment.h"
#include "Kans3D/Renderer/Passes/AntiAliasingPass.h"
#include "Kans3D/Renderer/Passes/ToneMappingPass.h"
#include "Graph/RenderGraph.h"
#include "RenderView.h"
#include "Resource/SelectionTools.h"

namespace Kans
{
    class ViewTarget;
    class SpriteBatch;
    class TextRenderer;
    class UIRenderer;
    struct RenderResult
    {
        RenderView       View;
        RenderGraph      Graph;
        SceneRenderStats Stats;
        SpriteStatistics SpriteStats;
        FrameStatistics                           FrameStats;
        std::shared_ptr<const CapturedGraphFrame> Captures;
        bool             Complete = true;
    };
    struct EnvironmentPrecomputeSizes
    {
        uint32_t Cubemap = 512, Irradiance = 512, Prefilter = 128, BrdfLUT = 512;
    };
    struct EnvironmentLighting
    {
        Environment                                     Maps;
        RenderHardware::Handle<RenderHardware::Texture> Skybox, BrdfLUT;
    };

    // Render-thread module. Acquire returns the one live module for a device.
    // Consumers hold leases; the implementation keeps a weak registry (no ownership cycle).
    class Renderer : public std::enable_shared_from_this<Renderer>
    {
    public:
        static std::shared_ptr<Renderer> Acquire(std::shared_ptr<RenderHardware::Device> device = {});
        static void                      Init();
        static void                      Init(const Scope<Window>& window);
        static void Shutdown();
        static Ref<Texture2D> GetWhiteTexture();
        ~Renderer();
        RenderResult      Render(RenderWorld& world, const RenderView& view, ViewTarget& target);
        ScenePickHit      PickSceneSurface(RenderWorld& world, const RenderView& view, glm::uvec2 pixel);
        void              AppendEntityDraws(RenderWorld&           world,
                                            const RenderView&      view,
                                            UUID                   entity,
                                            uint32_t               id,
                                            std::vector<MeshDraw>& draws);
        SpriteBatch&      GetSpriteBatch();
        TextRenderer&     GetTextRenderer();
        UIRenderer&       GetUIRenderer();
        SelectionOutline& GetOutlineService() { return m_Outline; }
        Renderer(const Renderer&)            = delete;
        Renderer& operator=(const Renderer&) = delete;

        std::shared_ptr<RenderHardware::Device> GetDevice() const { return m_Device; }
        // Manager must outlive all its cache leases. Last lease releases the cache,
        // allowing a manager to be destroyed without a dangling module-owned cache.
        std::shared_ptr<RenderAssetCache>    AcquireAssetCache(AssetManager& assets);
        std::shared_ptr<EnvironmentLighting> PrepareEnvironment(const std::filesystem::path& path,
                                                                EnvironmentPrecomputeSizes   sizes   = {},
                                                                bool                         refresh = false);
        std::shared_ptr<EnvironmentLighting> UpdateEnvironment(const std::filesystem::path& path,
                                                               EnvironmentPrecomputeSizes   sizes = {});
        uint64_t GetEnvironmentPrecomputeCount() const { return m_EnvironmentPrecomputeCount; }
        std::array<RenderHardware::Handle<RenderHardware::Texture>, 3>
        GetMaterialEnvironment(const EnvironmentLighting& lighting);
        const RenderHardware::Handle<RenderHardware::Sampler>& GetMaterialEnvironmentSampler() const
        {
            return m_IBLSampler;
        }
        EntityPicker&    GetPickingService() { return m_Picker; }
        ToneMappingPass& GetToneMapping() { return m_ToneMapping; }
        AntiAliasingPass& GetAntiAliasing() { return m_AntiAliasing; }
        const GpuMesh&   GetCube();
        RenderHardware::Handle<RenderHardware::Shader>
        GetInternalProgram(const char* name, const char* vertex, const char* fragment);
        RenderHardware::Handle<RenderHardware::Pipeline> CachedPipeline(const RenderHardware::PipelineDesc& desc);
        RenderHardware::PipelineDesc
        ScenePipeline(const char* name, const char* vertex, const char* fragment, bool sampled);

    private:
        explicit Renderer(std::shared_ptr<RenderHardware::Device> device);
        std::shared_ptr<RenderHardware::Device> m_Device;
        Ref<Texture2D>                          m_White;
        struct AssetCacheEntry
        {
            Scope<RenderAssetCache>         Cache;
            std::weak_ptr<RenderAssetCache> Lease;
        };
        std::unordered_map<AssetManager*, AssetCacheEntry> m_AssetCaches;
        std::map<std::tuple<std::string, uint32_t, uint32_t, uint32_t, uint32_t>, std::shared_ptr<EnvironmentLighting>>
                 m_Environments;
        uint64_t m_EnvironmentPrecomputeCount = 0;
        std::map<std::tuple<std::string, std::string, std::string>, RenderHardware::Handle<RenderHardware::Shader>>
                                                                                          m_InternalPrograms;
        std::unordered_map<std::string, RenderHardware::Handle<RenderHardware::Pipeline>> m_InternalPipelines;
        RenderHardware::Handle<RenderHardware::Sampler>                                   m_IBLSampler;
        std::array<RenderHardware::Handle<RenderHardware::Texture>, 3>                    m_BlackEnvironment;
        std::optional<GpuMesh>                                                            m_Cube;
        EntityPicker                                                                      m_Picker;
        SelectionOutline                                                                  m_Outline;
        std::unique_ptr<UIRenderer>                                                       m_UI;
        std::unique_ptr<TextRenderer>                                                     m_Text;
        std::unique_ptr<SpriteBatch>                                                      m_Sprites;
        ToneMappingPass                                                                   m_ToneMapping;
        AntiAliasingPass                                                                  m_AntiAliasing;
    };
} // namespace Kans
