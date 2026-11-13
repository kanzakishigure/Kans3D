#pragma once
#include <string>
#include <variant>
#include <glm/glm.hpp>
#include "Kans3D/Asset/AssetError.h"
#include "Kans3D/Asset/Runtime/ShaderData.h"
#include "Kans3D/RHI/RHI.h"

namespace Kans
{
    // CPU products (including failures) are cached by name AND source for the process.
    // Sources are embedded beside their renderer, independent of the working directory.
    using InternalShaderData = std::variant<ShaderData, AssetError>;
    const InternalShaderData&
    GetInternalShaderData(const std::string& name, const std::string& vertex, const std::string& fragment);
    std::variant<RenderHardware::Handle<RenderHardware::Shader>, AssetError>
              CreateInternalShader(RenderHardware::Device& device,
                                   const std::string&      name,
                                   const std::string&      vertex,
                                   const std::string&      fragment);
    RenderHardware::Handle<RenderHardware::Shader> GetSkinnedInternalProgram(RenderHardware::Device&,
                                                                             const std::string& name,
                                                                             const std::string& vertex,
                                                                             const std::string& fragment);
    const InternalShaderData& GetInternalComputeShaderData(const std::string&                        name,
                                                           const std::string&                        source,
                                                           const std::map<std::string, std::string>& includes = {});
    const InternalShaderData& GetBuiltinComputeShaderData(const std::string& name);
    RenderHardware::Handle<RenderHardware::Shader> CreateBuiltinComputeShader(RenderHardware::Device&,
                                                                              const std::string& name);
    glm::mat4 DeviceProjection(const glm::mat4& projection, RenderHardware::CoordinateConventions coordinates);
    // Render-to-texture projection: canonical UV V=0 maps to stored row zero,
    // independently of the display framebuffer origin.
    glm::mat4 DeviceTextureProjection(const glm::mat4& projection, RenderHardware::CoordinateConventions coordinates);
    uint32_t  DeviceRow(uint32_t bottomLeftRow, uint32_t height, RenderHardware::CoordinateConventions coordinates);
} // namespace Kans
