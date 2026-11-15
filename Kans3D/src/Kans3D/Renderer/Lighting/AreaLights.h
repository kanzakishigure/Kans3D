#pragma once
#include "Kans3D/Renderer/RenderView.h"
namespace Kans
{
    struct RenderFrame;
    struct AreaLightResources
    {
        RenderHardware::Handle<RenderHardware::Texture>                                   Matrix, Amplitude;
        std::array<RenderHardware::Handle<RenderHardware::Texture>, EngineAreaLightLimit> Sources;
        RenderHardware::Handle<RenderHardware::Sampler>                                   Sampler;
    };
    std::shared_ptr<AreaLightResources> CreateAreaLightFallback(RenderHardware::Device& device);
    std::vector<RenderHardware::Handle<RenderHardware::Texture>> AreaLightImages(const AreaLightResources&);
    void                                                         PrepareAreaLights(RenderFrame&);
} // namespace Kans
