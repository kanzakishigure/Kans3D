#include "kspch.h"
#include "RenderTextureShader.h"
#include <shaderc/shaderc.hpp>
#include "Kans3D/Asset/Runtime/EngineShaderText.h"
#include "Kans3D/Core/Debug/ApplicationFrameProfile.h"
namespace Kans
{
    std::variant<GpuShader, AssetError> MakeRenderTextureShader(const ShaderData&                       source,
                                                                std::shared_ptr<RenderHardware::Device> device)
    {
        ShaderData data;
        data.DebugName   = source.DebugName + "/RenderTexture";
        data.OutputSpace = source.OutputSpace;

        const auto& code = source.RenderTextureCode.Modules.empty() ? source.Code : source.RenderTextureCode;
        for (const auto& m : code.Modules)
            if (m.Stage == ShaderProductStage::Fragment)
                data.Code.Modules.push_back(m);
        auto inspected = InspectShaderProgram(data.Code);
        if (auto* e = std::get_if<AssetError>(&inspected))
            return *e;
        const auto resources = MaterialShaderResources(std::get<std::vector<ShaderStageReflection>>(inspected));
        for (const auto& [name, role] : source.TextureFallbacks)
            if (std::any_of(resources.begin(), resources.end(), [&](const auto& b) { return b.Name == name; }))
                data.TextureFallbacks.emplace(name, role);
        std::string vertex = ShaderText::RenderTextureVertexPrefix;
        std::string assignments;
        for (const auto& stage : std::get<std::vector<ShaderStageReflection>>(inspected))
            for (const auto& input : stage.Inputs)
            {
                if (input.Columns != 1)
                    return AssetError {AssetErrorCode::NotImplemented, "RenderTexture matrix varyings are unsupported"};
                const auto type = input.Components == 1 ? (input.Scalar == ShaderInterfaceScalar::Float  ? "float" :
                                                           input.Scalar == ShaderInterfaceScalar::Signed ? "int" :
                                                                                                           "uint") :
                                                          (input.Scalar == ShaderInterfaceScalar::Float  ? "vec" :
                                                           input.Scalar == ShaderInterfaceScalar::Signed ? "ivec" :
                                                                                                           "uvec") +
                                                              std::to_string(input.Components);
                const auto name = "rt_" + std::to_string(input.Location);
                vertex += "layout(location=" + std::to_string(input.Location) + ") " +
                          (input.Scalar == ShaderInterfaceScalar::Float ? "" : "flat ") + "out " + type + " " + name +
                          ";\n";
                const auto value = input.Scalar != ShaderInterfaceScalar::Float ? type + "(0)" :
                                   input.Components == 2                        ? "uv" :
                                   input.Components == 3                        ? "vec3(uv,0)" :
                                   input.Components == 4                        ? "vec4(1)" :
                                                                                  "0.0";
                assignments += name + "=" + value + ";\n";
            }
        vertex += std::string(ShaderText::RenderTextureVertexMain) + assignments + ShaderText::RenderTextureVertexEnd;
        shaderc::Compiler       compiler;
        shaderc::CompileOptions options;
        options.SetTargetEnvironment(shaderc_target_env_vulkan, shaderc_env_version_vulkan_1_2);
        const auto compiled = ProfileShaderCompile("RenderTexture.vert", [&] {
            return compiler.CompileGlslToSpv(vertex, shaderc_vertex_shader, "RenderTexture.vert", options);
        });
        if (compiled.GetCompilationStatus() != shaderc_compilation_status_success)
            return AssetError {AssetErrorCode::InvalidData, compiled.GetErrorMessage()};
        data.Code.Modules.insert(data.Code.Modules.begin(),
                                 {ShaderProductStage::Vertex, "main", {compiled.cbegin(), compiled.cend()}});
        return GpuShader::Create(data, std::move(device));
    }
} // namespace Kans
