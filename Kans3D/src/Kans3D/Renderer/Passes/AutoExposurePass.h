#pragma once
#include <array>
#include <optional>
#include "Kans3D/RHI/RHI.h"
#include "Kans3D/Renderer/Passes/AutoExposureSettings.h"
namespace Kans
{
    class AutoExposurePass
    {
    public:
        bool NeedsReset(uint64_t world, bool cut) const { return !m_State[0] || cut || m_World != world; }
        void Prepare(RenderHardware::Device&, uint64_t world, bool reset, float initialEV, float compensation);
        // Previous frame's exposure; RG = multiplier, uncompensated EV100.
        const RenderHardware::Handle<RenderHardware::Texture>& Input() const { return m_State[m_Current]; }
        std::optional<float>                                   CurrentEV() const { return m_EV; }
        const RenderHardware::Handle<RenderHardware::Texture>& Output() const { return m_State[1 - m_Current]; }
        void PatchUniforms(RenderHardware::Device&,
                           RenderHardware::CommandList&,
                           const RenderHardware::Handle<RenderHardware::Texture>&,
                           const RenderHardware::Handle<RenderHardware::Buffer>&,
                           uint32_t offset,
                           uint32_t stride,
                           uint32_t count);
        void Record(RenderHardware::Device&,
                    RenderHardware::CommandList&,
                    const RenderHardware::Handle<RenderHardware::Texture>& hdr,
                    const AutoExposureSettings&,
                    float compensation,
                    float delta);

    private:
        std::array<RenderHardware::Handle<RenderHardware::Texture>, 2>         m_State;
        RenderHardware::Handle<RenderHardware::Buffer>                         m_Histogram;
        RenderHardware::Handle<RenderHardware::BindGroupLayout>                m_Layout;
        std::array<RenderHardware::Handle<RenderHardware::ComputePipeline>, 2> m_Pipelines;
        RenderHardware::Handle<RenderHardware::Sampler>                        m_Sampler;
        RenderHardware::Handle<RenderHardware::BindGroupLayout>                m_PatchLayout;
        RenderHardware::Handle<RenderHardware::ComputePipeline>                m_PatchPipeline;
        RenderHardware::Handle<RenderHardware::Sampler>                        m_PatchSampler;
        struct Readback
        {
            RenderHardware::Handle<RenderHardware::Buffer> Buffer;
            bool                                           Pending  = false;
            uint64_t                                       Sequence = 0;
        };
        std::array<Readback, 3> m_Readbacks;
        unsigned                m_Current = 0;
        uint64_t                m_World = 0, m_Sequence = 0, m_Published = 0;
        bool                    m_Reset = true;
        std::optional<float>    m_EV;
    };
} // namespace Kans
