#pragma once
#include "RenderView.h"
#include "RenderWorld.h"
#include "Renderer.h"
#include "ViewTarget.h"

namespace Kans
{
    // One synchronous pipeline invocation. No viewport configuration or persistent scene ownership.
    struct RenderFrame
    {
        RenderWorld&                  World;
        RenderView&                   View;
        ViewTarget&                   Output;
        Renderer&                     Services;
        SceneRenderStats&             Stats;
        SpriteStatistics&             SpriteStats;
        bool&                         Complete;
        std::optional<double>         FinalPostRecordMilliseconds;
        bool                          DeferToneMapping = false, PostProcessHDR = false;
        uint32_t                                        Samples    = 1;
        RenderGraph*                                    SceneGraph = nullptr;
        std::vector<GraphTexture>                       MSColors;
        GraphTexture                                    MSHDR = 0, MSDepth = 0;
        bool                                            MSAAResolved = false;
        struct PrepassDraw
        {
            RenderHardware::Handle<RenderHardware::BindGroup>            Material;
            std::vector<RenderHardware::Handle<RenderHardware::Texture>> Images;
        };
        std::unordered_map<size_t, PrepassDraw>         PrepassDraws, VelocitySurfaceDraws;
        void                                            BuildDepthPrepass(RenderGraph& graph);
        bool                                            TemporalActive = false, VelocityActive = false;
        void                                            PrepareTemporal();
        void                                            ApplyTemporalUniforms(EngineFrameUniforms& frame) const;
        glm::mat4                                       SceneProjection() const;
        Camera                                          SceneCamera() const;
        void                                            BuildVelocity(RenderGraph& graph);
        void                                            BuildDebugBuffer(RenderGraph& graph);
        bool                                            ReconstructsHDR() const
        {
            return View.GetPostProcessSettings().FilterSize >= 1 || View.EffectiveAA == AntiAliasing::TAA;
        }
        bool                                            HasBackgroundColor() const
        {
            return View.DrawEnvironment && View.Environment.BackgroundColor != glm::vec3(0);
        }
        void ClearBackground(RenderHardware::CommandList& commands)
        {
            using namespace RenderHardware;
            const auto    c = View.Environment.BackgroundColor;
            RenderingDesc background;
            background.Colors.push_back({HDR(), {}, LoadOp::Clear, StoreOp::Store, {c.r, c.g, c.b, 1}});
            commands.BeginRendering(background);
            commands.EndRendering();
            // Immediate display-space submissions need the same background. Keep the raw
            // HDR image for materials/bloom until an immediate submission replaces it.
            Services.GetToneMapping().Record(*GetDevice(),
                                             commands,
                                             HDR(),
                                             Colors(),
                                             View.GetPostProcessSettings().Exposure,
                                             false,
                                             View.GetPostProcessSettings().ToneMapping);
        }
        RenderHardware::Handle<RenderHardware::Texture> HDR() const
        {
            return Samples > 1 && !MSAAResolved ? SceneGraph->Texture(MSHDR) : Output.GetHDR();
        }
        std::vector<RenderHardware::Handle<RenderHardware::Texture>> Colors() const
        {
            if (Samples == 1 || MSAAResolved)
                return Output.GetColors();
            std::vector<RenderHardware::Handle<RenderHardware::Texture>> colors;
            for (auto id : MSColors)
                colors.push_back(SceneGraph->Texture(id));
            return colors;
        }
        const GpuMesh*                GetMesh(const MeshKey& id) const;
        const GpuMaterialBindings*    GetBindings(AssetID id, bool debug) const;
        const GpuShader*              GetShader(AssetID material, AssetID shader) const;
        RenderAssetCache*             Assets() const { return View.Assets.get(); }
        RenderHardware::RenderingDesc SceneTarget(RenderHardware::LoadOp load, bool hdr = false) const
        {
            auto target = Output.Target(load, hdr);
            if (Samples > 1 && !MSAAResolved)
            {
                const auto colors = Colors();
                for (size_t i = 0; i < colors.size(); ++i)
                    target.Colors[i].Target = i == 0 && hdr ? HDR() : colors[i];
                target.Depth.Target = SceneGraph->Texture(MSDepth);
            }
            return target;
        }
        std::array<RenderHardware::Handle<RenderHardware::Texture>, 3> GetMaterialEnvironment();
        std::shared_ptr<RenderHardware::Device> GetDevice() const { return Services.GetDevice(); }
        bool IncludesSection(UUID entity, UUID instanceRoot, const GpuMesh* mesh, uint32_t section) const;
        void                                    PrepareDrawLists();
        bool                                    HasMaterialPass(const RenderView& view, const std::string& tag);
        void                                    DrawShaderMaterials(
            RenderHardware::CommandList&                                                       commands,
            const std::function<void(const RenderHardware::Handle<RenderHardware::Texture>&)>& sampled   = {},
            const std::string&                                                                 tag       = {},
            const RenderView*                                                                  view      = nullptr,
            RenderHardware::Handle<RenderHardware::CommandList>*                               finalPost = nullptr);
        void SubmitPrefabPreview(const PrefabPreview& preview);
        void AppendPrefabPreviewDraws(const PrefabPreview& preview, std::vector<MeshDraw>& draws);
        void AppendEntityDraws(UUID entity, uint32_t id, std::vector<MeshDraw>& draws);
        void DrawModelGeometry(const ViewGeometryDraw& draw, RenderHardware::CommandList& commands);
        void DrawSkybox(const ViewSkybox& sky, RenderHardware::CommandList& commands);
    };
} // namespace Kans
