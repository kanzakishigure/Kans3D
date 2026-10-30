#pragma once
#include "Kans3D/Renderer/DebugDraw.h"
#include "Kans3D/Renderer/RenderFeature.h"
namespace Kans
{
    class Renderer;
    class DebugDrawFeature final : public RenderFeature
    {
    public:
        bool        RunsAt(RenderExtensionPoint p) const override { return p == RenderExtensionPoint::Overlay; }
        void        AddPasses(RenderExtensionPoint, const RenderFeatureContext&) override;
        static void Record(Renderer&,
                           RenderHardware::CommandList&,
                           const RenderView&,
                           const DebugDraw&,
                           DebugDepth,
                           const RenderHardware::RenderingDesc&);
    };
} // namespace Kans
