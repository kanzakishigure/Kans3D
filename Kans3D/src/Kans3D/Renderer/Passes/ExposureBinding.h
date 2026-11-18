#pragma once
#include "Kans3D/Asset/Runtime/MaterialShaderInterface.h"
#include "Kans3D/Renderer/Passes/AutoExposurePass.h"
#include "Kans3D/Renderer/RenderFrame.h"
namespace Kans
{
    // EngineFrameUniforms::LightingInfo.y holds the exposure. A named constant, because MSVC rejects
    // offsetof inside a default argument (C2587/C2618).
    inline constexpr uint32_t FrameExposureOffset =
        uint32_t(offsetof(EngineFrameUniforms, LightingInfo) + sizeof(float));
    inline RenderHardware::BufferUsage ExposureUniformUsage(const RenderView& view)
    {
        return view.GPUExposure ? RenderHardware::BufferUsage::Uniform | RenderHardware::BufferUsage::Storage :
                                  RenderHardware::BufferUsage::Uniform;
    }
    // Populate the existing frame ABI on the GPU. Custom HDR/post shaders and VFX
    // therefore observe exactly the same value as bloom/tonemapping without a CPU readback.
    inline void PatchFrameExposure(RenderFrame&                                          frame,
                                   RenderHardware::CommandList&                          commands,
                                   const RenderHardware::Handle<RenderHardware::Buffer>& destination,
                                   uint32_t                                              count  = 1,
                                   uint32_t                                              stride = 0,
                                   uint32_t                                              offset = FrameExposureOffset)
    {
        auto& owner = (frame.DisplayFrame ? frame.DisplayFrame->Output : frame.Output).GetAutoExposure();
        if (owner && frame.View.GPUExposure)
            owner->PatchUniforms(
                *frame.GetDevice(), commands, frame.View.GPUExposure, destination, offset, stride, count);
    }
} // namespace Kans
