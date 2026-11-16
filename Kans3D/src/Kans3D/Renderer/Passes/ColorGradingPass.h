#pragma once
#include "Kans3D/RHI/RHI.h"
#include "Kans3D/Renderer/Passes/ColorGradingSettings.h"
namespace Kans
{
    class ColorGradingPass
    {
    public:
        RenderHardware::Handle<RenderHardware::Texture>
        Record(RenderHardware::Device&,
               RenderHardware::CommandList&,
               const RenderHardware::Handle<RenderHardware::Texture>& view,
               const ColorGradingSettings&,
               const RenderHardware::Handle<RenderHardware::Texture>& userLut);

    private:
        struct Cached
        {
            std::weak_ptr<RenderHardware::Texture>          View;
            uint64_t                                        Hash = 0;
            std::weak_ptr<RenderHardware::Texture>          User;
            RenderHardware::Handle<RenderHardware::Texture> Lut;
        };
        std::vector<Cached>                                     m_Cache;
        RenderHardware::Handle<RenderHardware::Shader>          m_Shader;
        RenderHardware::Handle<RenderHardware::ComputePipeline> m_Pipeline;
        RenderHardware::Handle<RenderHardware::BindGroupLayout> m_Layout;
        RenderHardware::Handle<RenderHardware::Sampler>         m_Sampler;
    };
} // namespace Kans
