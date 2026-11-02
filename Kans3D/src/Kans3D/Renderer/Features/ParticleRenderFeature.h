#pragma once
#include "Kans3D/Asset/Runtime/MaterialShaderInterface.h"
#include "Kans3D/Renderer/RenderFeature.h"
namespace Kans
{
    class ParticleRenderFeature final : public RenderFeature
    {
    public:
        void        Prepare(RenderView&, RenderFrame&) override;
        static void Simulate(RenderGraph&, RenderFrame&);
        static void RecordMeshShadows(RenderGraph&,
                                      RenderFrame&,
                                      RenderHardware::CommandList&,
                                      const EngineFrameUniforms&,
                                      const ShaderData&,
                                      std::vector<GraphUse>&,
                                      bool  point,
                                      float constantBias,
                                      float slopeBias);
        bool        RunsAt(RenderExtensionPoint p) const override
        {
            return p == RenderExtensionPoint::BeforeOpaque || p == RenderExtensionPoint::AfterTransparent;
        }
        void AddPasses(RenderExtensionPoint, const RenderFeatureContext&) override;

    private:
        RenderHardware::Handle<RenderHardware::Texture> m_EmptyDepth;
    };
} // namespace Kans
