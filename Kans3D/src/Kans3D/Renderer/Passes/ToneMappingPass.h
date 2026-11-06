#pragma once
#include <map>
#include <glm/glm.hpp>
#include "Kans3D/RHI/RHI.h"
#include "Kans3D/Renderer/Passes/PostProcessSettings.h"
namespace Kans
{
    // ACES fitted (Narkowicz) + IEC sRGB. Inverse is used only at ordered
    // display -> HDR batch boundaries, never on material output bytecode.
    class ToneMappingPass
    {
    public:
        void         Record(RenderHardware::Device&                                             device,
                            RenderHardware::CommandList&                                        commands,
                            const RenderHardware::Handle<RenderHardware::Texture>&              source,
                            const std::vector<RenderHardware::Handle<RenderHardware::Texture>>& targets,
                            float                                                               exposure,
                            bool                                                                inverse = false,
                            ToneMappingMode                                                     mode = ToneMappingMode::ACES);
        static float EffectiveExposure(float exposure);

    private:
        // Matching settings are immutable and can be shared across submitted command lists.
        std::vector<std::pair<glm::vec4, RenderHardware::Handle<RenderHardware::Buffer>>> m_Uniforms;
        RenderHardware::Handle<RenderHardware::Shader>                                    m_Programs[2];
        RenderHardware::Handle<RenderHardware::BindGroupLayout>                           m_Layout;
        RenderHardware::Handle<RenderHardware::Sampler>                                   m_Sampler;
        std::map<std::pair<std::vector<RenderHardware::Format>, uint32_t>,
                 RenderHardware::Handle<RenderHardware::Pipeline>>
            m_Pipelines;
    };
} // namespace Kans
