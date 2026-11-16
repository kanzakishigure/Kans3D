#pragma once
#include "GpuShader.h"
namespace Kans
{
    std::variant<GpuShader, AssetError> MakeRenderTextureShader(const ShaderData&,
                                                                std::shared_ptr<RenderHardware::Device>);
}
