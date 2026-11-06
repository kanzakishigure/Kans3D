#pragma once
#include <array>
#include "Kans3D/Renderer/Passes/BloomSettings.h"
#include "Kans3D/Renderer/RenderFeature.h"
namespace Kans
{
    class BloomFeature final : public RenderFeature
    {
    public:
        void Prepare(RenderView& view, RenderFrame& frame) override;
        bool RunsAt(RenderExtensionPoint point) const override
        {
            return point == RenderExtensionPoint::BeforePostProcess;
        }
        void AddPasses(RenderExtensionPoint point, const RenderFeatureContext& context) override;
        // Half-resolution level zero, capped at ten levels and an approximately eight-pixel short side.
        static std::vector<glm::uvec2> Levels(glm::uvec2 viewport);
        // The same graph builder is usable with an already-linear HDR input (including GPU probes).
        // Returns the filtered half-resolution bloom. Disabled calls return hdr and add nothing.
        GraphTexture AddBloomPasses(RenderGraph&                                    graph,
                                    GraphTexture                                    hdr,
                                    glm::uvec2                                      viewport,
                                    const BloomSettings&                            settings,
                                    float                                           exposure,
                                    RenderHardware::Handle<RenderHardware::Texture> dirt = {});

    private:
        struct Parameters
        {
            glm::vec4 OutputSize {0}, Curve {0}, Options {0}, Tint {1};
        };
        void                                                            Record(RenderGraphContext& context,
                                                                               unsigned            stage,
                                                                               GraphTexture        source,
                                                                               GraphTexture        fine,
                                                                               GraphTexture        target,
                                                                               GraphTexture        dirt,
                                                                               const Parameters&   parameters);
        RenderHardware::Handle<RenderHardware::Shader>                  m_Shader;
        RenderHardware::Handle<RenderHardware::BindGroupLayout>         m_Layout;
        RenderHardware::Handle<RenderHardware::Sampler>                 m_Sampler;
        std::array<RenderHardware::Handle<RenderHardware::Pipeline>, 3> m_Pipelines;
    };
} // namespace Kans
