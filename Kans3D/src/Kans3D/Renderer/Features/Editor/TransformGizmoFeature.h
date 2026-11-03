#pragma once
#include <array>
#include "Kans3D/Renderer/GizmoDraw.h"
#include "Kans3D/Renderer/RenderFeature.h"

namespace Kans
{
    class Renderer;
    // Editor transform handles (RenderView::Gizmo) as solid, lightly shaded meshes over the
    // display image, after tone mapping. The part behind scene geometry is drawn first at
    // OccludedAlpha, then the visible part depth-tested. The unit meshes are built once per
    // feature; each frame writes one small uniform block taken from a reused set.
    class TransformGizmoFeature final : public RenderFeature
    {
    public:
        static constexpr uint32_t MaxDraws      = 64;
        static constexpr float    OccludedAlpha = 0.25f;

        bool RunsAt(RenderExtensionPoint point) const override { return point == RenderExtensionPoint::Overlay; }
        void AddPasses(RenderExtensionPoint point, const RenderFeatureContext& context) override;
        // Records the handles into `target` (loaded, not cleared).
        void Record(Renderer&                            renderer,
                    RenderHardware::CommandList&         commands,
                    const RenderView&                    view,
                    const RenderHardware::RenderingDesc& target);

    private:
        struct ShapeRange
        {
            uint32_t First = 0, Count = 0;
        };
        void Build(RenderHardware::Device& device);
        RenderHardware::Handle<RenderHardware::Buffer> TakeUniform(RenderHardware::Device& device);

        RenderHardware::Handle<RenderHardware::Buffer>              m_Vertices, m_Indices;
        std::array<ShapeRange, GizmoShapeCount>                     m_Shapes {};
        std::vector<RenderHardware::Handle<RenderHardware::Buffer>> m_Uniforms;
    };
} // namespace Kans
