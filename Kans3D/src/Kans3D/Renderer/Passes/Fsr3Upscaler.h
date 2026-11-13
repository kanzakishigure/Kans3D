#pragma once
#include <map>
#include <glm/glm.hpp>
#include "Kans3D/Asset/Runtime/ShaderData.h"
#include "Kans3D/Core/TimeStep.h"
#include "Kans3D/Renderer/Graph/RenderGraph.h"

namespace Kans
{
    // std140 cbFSR3UPSCALER, SDK 1.1.4. Padding belongs to the final 16-byte register.
    struct alignas(16) Fsr3Constants
    {
        glm::ivec2 RenderSize {}, PreviousRenderSize {}, DisplaySize {}, PreviousDisplaySize {};
        glm::ivec2 MaxRenderSize {}, MaxDisplaySize {};
        glm::vec4  DeviceToViewDepth {};
        glm::vec2  Jitter {}, PreviousJitter {}, MotionVectorScale {1}, DownscaleFactor {1};
        glm::vec2  MotionVectorJitterCancellation {};
        float      TanHalfFov = 0, JitterPhaseCount = 8;
        float      DeltaTime = 0, DeltaPreExposure = 1, ViewSpaceToMeters = 1, FrameIndex = 0;
        float      VelocityFactor = 1, ReactivenessScale = 1, ShadingChangeScale = 1;
        float      AccumulationAddedPerFrame = 1.f / 3, MinDisocclusionAccumulation = -1.f / 3;
    };
    static_assert(sizeof(Fsr3Constants) == 160);
    // Inputs and constants remain independently testable without a GPU or ViewTarget.
    Fsr3Constants MakeFsr3Constants(glm::uvec2           renderSize,
                                    glm::uvec2           displaySize,
                                    const glm::mat4&     projection,
                                    glm::vec2            jitter,
                                    TimeStep             delta,
                                    bool                 reset,
                                    const Fsr3Constants& previous);

    class Fsr3Upscaler
    {
    public:
        struct Inputs
        {
            glm::uvec2                                      RenderSize {}, DisplaySize {};
            RenderHardware::Handle<RenderHardware::Texture> Color, Depth, MotionVectors;
            // Engine projection before DeviceProjection, finite conventional Z (near=0, far=1 on Vulkan).
            glm::mat4 Projection {1};
            glm::vec2 Jitter {};
            TimeStep  FrameTimeDelta;
            bool      Reset = false;
        };
        explicit Fsr3Upscaler(RenderHardware::Device& device);
        GraphTexture         AddPasses(RenderGraph& graph, const Inputs& inputs);
        const Fsr3Constants& GetConstants() const { return m_Constants; }
        // Retained handles permit lifetime/persistence inspection without readback.
        const std::map<std::string, RenderHardware::Handle<RenderHardware::Texture>>& GetResources() const
        {
            return m_Resources;
        }

    private:
        struct Pass
        {
            RenderHardware::Handle<RenderHardware::BindGroupLayout> Layout;
            RenderHardware::Handle<RenderHardware::ComputePipeline> Pipeline;
            std::vector<ShaderResourceBinding>                      Bindings;
        };
        struct UniformBuffers
        {
            RenderHardware::Handle<RenderHardware::Buffer> Constants, Spd;
        };
        std::vector<UniformBuffers>                                            m_Uniforms;
        RenderHardware::Device&                                                m_Device;
        std::vector<Pass>                                                      m_Passes;
        RenderHardware::Handle<RenderHardware::Sampler>                        m_Point, m_Linear;
        std::map<std::string, RenderHardware::Handle<RenderHardware::Texture>> m_Resources;
        glm::uvec2                                                             m_RenderSize {}, m_DisplaySize {};
        Fsr3Constants                                                          m_Constants;
        uint64_t                                                               m_Frame = 0;
    };
} // namespace Kans
