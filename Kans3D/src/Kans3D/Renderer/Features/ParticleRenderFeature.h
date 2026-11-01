#pragma once
#include "Kans3D/Renderer/RenderFeature.h"
namespace Kans
{
    class ParticleRenderFeature final : public RenderFeature
    {
    public:
        void Prepare(RenderView&, RenderFrame&) override;
        bool RunsAt(RenderExtensionPoint p) const override
        {
            return p == RenderExtensionPoint::BeforeOpaque || p == RenderExtensionPoint::AfterTransparent;
        }
        void AddPasses(RenderExtensionPoint, const RenderFeatureContext&) override;
    };
} // namespace Kans
