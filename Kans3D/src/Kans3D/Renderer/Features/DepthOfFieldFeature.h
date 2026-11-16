#pragma once
#include <array>
#include "Kans3D/Renderer/RenderFeature.h"
namespace Kans
{
    class DepthOfFieldFeature final : public RenderFeature
    {
    public:
        void Prepare(RenderView& view, RenderFrame& frame) override;
        bool RunsAt(RenderExtensionPoint point) const override
        {
            return point == RenderExtensionPoint::BeforePostProcess;
        }
        void AddPasses(RenderExtensionPoint, const RenderFeatureContext& context) override;
        // Depth is device depth. Jitter is in depth-texture pixels; zero for the unjittered display depth.
        GraphTexture AddDepthOfFieldPasses(RenderGraph&               graph,
                                           GraphTexture               hdr,
                                           GraphTexture               depth,
                                           glm::uvec2                 display,
                                           const PostProcessSettings& settings,
                                           const glm::mat4&           deviceProjection,
                                           glm::vec2                  jitter = {});

    private:
        struct Parameters
        {
            glm::vec4 Size, Lens, Ranges, Options, Projection, Jitter;
        };
        void
        Record(RenderGraphContext& c, GraphTexture target, std::array<GraphTexture, 4> inputs, const Parameters& p);
        RenderHardware::Handle<RenderHardware::Shader>          m_Shader;
        RenderHardware::Handle<RenderHardware::BindGroupLayout> m_Layout;
        RenderHardware::Handle<RenderHardware::Sampler>         m_Sampler, m_PointSampler;
        RenderHardware::Handle<RenderHardware::Pipeline>        m_Pipeline;
    };
} // namespace Kans
