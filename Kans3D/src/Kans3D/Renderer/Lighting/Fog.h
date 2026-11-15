#pragma once
#include <functional>
#include "Kans3D/Asset/Runtime/ShaderData.h"
#include "Kans3D/Core/UUID.h"
#include "Kans3D/RHI/RHI.h"
#include "FogSettings.h"

namespace Kans
{
    class RenderGraph;
    struct RenderFrame;
    constexpr uint32_t FogVolumeLimit = 32;
    struct ViewFogVolume
    {
        UUID               EntityID = 0;
        glm::mat4          Transform {1};
        FogVolumeComponent Medium;
    };
    // Nearest centre, then UUID: stable under registry insertion/removal order.
    std::vector<ViewFogVolume> SelectFogVolumes(std::vector<ViewFogVolume> volumes, glm::vec3 camera);
    struct FogState
    {
        glm::uvec3                                                             Size {0};
        glm::uvec2                                                             RenderSize {0};
        FogSettings                                                            Settings;
        const void*                                                            World     = nullptr;
        uint64_t                                                               LastFrame = 0;
        bool                                                                   Valid     = false;
        glm::mat4                                                              PreviousView {1}, PreviousProjection {1};
        RenderHardware::Handle<RenderHardware::Texture>                        History[2], Integrated, Noise;
        RenderHardware::Handle<RenderHardware::Sampler>                        Linear, NoiseSampler;
        RenderHardware::Handle<RenderHardware::BindGroupLayout>                SampleLayout;
        RenderHardware::Handle<RenderHardware::BindGroup>                      SampleGroup;
        RenderHardware::Handle<RenderHardware::BindGroupLayout>                EngineLayout;
        std::array<RenderHardware::Handle<RenderHardware::BindGroupLayout>, 4> ComputeLayouts;
        std::array<RenderHardware::Handle<RenderHardware::ComputePipeline>, 4> ComputePipelines;
        RenderHardware::Handle<RenderHardware::Buffer>                         SampleUniforms;
    };
    struct FogFrameResources
    {
        uint32_t   Media = uint32_t(-1), LitScattering = uint32_t(-1), Scattering = uint32_t(-1),
                   Integrated = uint32_t(-1);
        glm::uvec3 Size {0};
        glm::mat4  InverseView {1}, InverseProjection {1};
        float      Distance    = 64;
        uint32_t   JitterFrame = 0;
    };
    // media: RGBA16F sigma_s.rgb, sigma_t.a in inverse metres, additive in world space.
    // Grid3D and R4b can declare graph writes here before lighting without owning the fog renderer.
    using FogDensityInjector = std::function<void(RenderGraph&, const FogFrameResources&)>;
    void                                 BuildFogPasses(RenderGraph& graph, RenderFrame& frame);
    void                                 AddFogBackground(RenderGraph& graph, RenderFrame& frame);
    std::variant<ShaderData, AssetError> MakeFogShader(const ShaderProgramCode&, uint32_t alphaMode);
} // namespace Kans
