#include "kspch.h"
#include "Kans3D/Renderer/Shading/Skinning.h"
#include <shaderc/shaderc.hpp>
#include <spirv_cross/spirv_glsl.hpp>
#include "Kans3D/Asset/Runtime/EngineShaderText.h"
#include "Kans3D/Renderer/Shading/DepthNormals.h"
#include "Kans3D/Renderer/Shading/SkinningSource.h"
namespace Kans
{
    std::string
    SkinningEvaluationGLSL(bool normal, bool tangent, bool smoothNormal, bool smoothNormalFallback, bool normalLines)
    {
        std::string source;
        // Palette matrices retain their A1 ABI. A tagged final matrix describes the active
        // list appended after the joints. Untagged legacy palettes have no morphs.
        source += ShaderText::Skinning_mat4_header_kans_Palette;
        source += normalLines ? ShaderText::Skinning_uint_vertex_kans_Deltas : ShaderText::Skinning_uint_vertex_uint_gl;
        source += ShaderText::Skinning_for_uint_i_i;
        // An absent stream aliases the ordinary normal, including its morph deltas.
        // Authored welded streams deliberately retain their bind-pose direction.
        if (smoothNormal && smoothNormalFallback)
            source += ShaderText::Skinning_if_kans_Deltas_key;
        if (normal)
            source += ShaderText::Skinning_if_kans_Deltas_key2;
        if (tangent)
            source += ShaderText::Skinning_if_kans_Deltas_key3;
        source += ShaderText::Skinning_mat4_skin_mat4_if;
        return source;
    }
    std::variant<ShaderData, AssetError> MakeSkinnedShader(const ShaderData& data, bool smoothNormalFallback)
    {
        try
        {
            ShaderData result  = data;
            if (!result.DebugName.empty())
                result.DebugName += smoothNormalFallback ? "/SkinnedNormalFallback" : "/Skinned";
            result.DebugSources.erase(ShaderProductStage::Vertex);
            auto convert = [smoothNormalFallback](ShaderProgramCode&                         code,
                                                  std::map<ShaderProductStage, std::string>& sources) {
                for (auto& module : code.Modules)
                    if (module.Stage == ShaderProductStage::Vertex)
                    {
                        spirv_cross::CompilerGLSL compiler(module.Words);
                        auto                      options = compiler.get_common_options();
                        options.version                   = 450;
                        options.vulkan_semantics          = true;
                        compiler.set_common_options(options);
                        bool position = false, normal = false, tangent = false, normalLines = false,
                             smoothNormal = false;
                        for (const auto& input : compiler.get_shader_resources().stage_inputs)
                        {
                            const auto location = compiler.get_decoration(input.id, spv::DecorationLocation);
                            // Engine debug-normal endpoints are indexed per triangle corner, rather than per mesh
                            // vertex.
                            if (location == 2 && compiler.get_name(input.id) == "a_End" &&
                                compiler.get_type(input.base_type_id).vecsize == 1)
                                normalLines = true;
                            if (location == 0)
                            {
                                compiler.set_name(input.id, ShaderText::Skinning_kans_Position);
                                position = true;
                            }
                            if (location == 1)
                            {
                                compiler.set_name(input.id, ShaderText::Skinning_kans_Normal);
                                normal = true;
                            }
                            if (location == 7)
                            {
                                compiler.set_name(input.id, ShaderText::Skinning_kans_SmoothNormal);
                                smoothNormal = true;
                            }
                            if (location == 4)
                            {
                                compiler.set_name(input.id, ShaderText::Skinning_kans_Tangent);
                                tangent = true;
                            }
                        }
                        if (!position)
                            throw std::runtime_error("Skinned vertex shader requires position at location 0");
                        std::string source = compiler.compile();
                        auto        input  = [&](const char* type, const char* name, const char* raw) {
                            const std::string declaration = std::string(ShaderText::Skinning_in) + type +
                                                            ShaderText::Space + name + ShaderText::Semicolon;
                            const auto        at          = source.find(declaration);
                            if (at == std::string::npos)
                                throw std::runtime_error("Cannot build skinning vertex prologue");
                            source.replace(at,
                                           declaration.size(),
                                           std::string(ShaderText::Skinning_in) + type + ShaderText::Space + raw +
                                               ShaderText::EndStatement + type + ShaderText::Space + name +
                                               ShaderText::Semicolon);
                        };
                        input(ShaderText::Vec3,
                              ShaderText::Skinning_kans_Position,
                              ShaderText::Skinning_kans_RawPosition);
                        if (normal)
                            input(ShaderText::Vec3,
                                  ShaderText::Skinning_kans_Normal,
                                  ShaderText::Skinning_kans_RawNormal);
                        if (smoothNormal)
                            input(ShaderText::Vec3,
                                  ShaderText::Skinning_kans_SmoothNormal,
                                  ShaderText::Skinning_kans_RawSmoothNormal);
                        if (tangent)
                            input(ShaderText::Vec4,
                                  ShaderText::Skinning_kans_Tangent,
                                  ShaderText::Skinning_kans_RawTangent);
                        // SPIRV-Cross lowers the vertex include's private initializer into main.
                        // Keep its source at the raw input, before both morph accumulation and skinning.
                        const std::string rest = ShaderText::Skinning_kans_RestPosition_kans_Position;
                        if (const auto at = source.find(rest); at != std::string::npos)
                            source.replace(at, rest.size(), ShaderText::Skinning_kans_RestPosition_kans_RawPosition);
                        const auto main = source.find("void main()");
                        if (main == std::string::npos)
                            throw std::runtime_error("Missing shader main");
                        source.replace(main, 11, ShaderText::Skinning_void_kans_MaterialMain);
                        source += ShaderText::Skinning_layout_location_in_uvec4;
                        source += SkinningMorphVectorGLSL;
                        source += ShaderText::Skinning_void_main_vec3_p;
                        if (smoothNormal)
                            source += ShaderText::Skinning_vec3_sn_kans_RawSmoothNormal;
                        if (normal)
                            source += ShaderText::Skinning_vec3_kans_RawNormal;
                        if (tangent)
                            source += ShaderText::Skinning_vec3_t_kans_RawTangent;
                        source +=
                            SkinningEvaluationGLSL(normal, tangent, smoothNormal, smoothNormalFallback, normalLines);
                        source += ShaderText::Skinning_kans_Position_kans_DeformedPosition;
                        if (smoothNormal)
                            source += ShaderText::Skinning_kans_SmoothNormal_normalize_transpose;
                        if (normal)
                            source += ShaderText::Skinning_kans_Normal_normalize_transpose;
                        if (tangent)
                            source += ShaderText::Skinning_kans_Tangent_vec4_normalize;
                        source += ShaderText::Skinning_kans_MaterialMain;
                        shaderc::Compiler       shaderCompiler;
                        shaderc::CompileOptions compileOptions;
                        compileOptions.SetTargetEnvironment(shaderc_target_env_vulkan, shaderc_env_version_vulkan_1_2);
                        sources[module.Stage] = source;
                        auto compiled = shaderCompiler.CompileGlslToSpv(
                            source, shaderc_vertex_shader, "KansSkinning.vert", compileOptions);
                        if (compiled.GetCompilationStatus() != shaderc_compilation_status_success)
                            throw std::runtime_error(compiled.GetErrorMessage());
                        module.Words.assign(compiled.cbegin(), compiled.cend());
                        module.EntryPoint = ShaderText::Main;
                    }
            };
            convert(result.Code, result.DebugSources);
            for (auto& pass : result.Passes)
                convert(pass.Code, pass.DebugSources);
            MakeSurfacePositionInvariant(result);
            return result;
        }
        catch (const std::exception& e)
        {
            return AssetError {AssetErrorCode::InvalidData, e.what()};
        }
    }
    RenderHardware::Handle<RenderHardware::BindGroupLayout> SkinningLayout(RenderHardware::Device& device)
    {
        return device.CreateBindGroupLayout({2,
                                             {{0, RenderHardware::BindingType::ReadOnlyStorageBuffer},
                                              {1, RenderHardware::BindingType::ReadOnlyStorageBuffer}}});
    }
    void BindSkinning(RenderHardware::Device&                                        device,
                      RenderHardware::CommandList&                                   commands,
                      const RenderHardware::Handle<RenderHardware::BindGroupLayout>& layout,
                      const RenderHardware::Handle<RenderHardware::Buffer>&          palette,
                      const RenderHardware::Handle<RenderHardware::Buffer>&          morphDeltas)
    {
        if (palette)
            commands.BindGroup(device.CreateBindGroup({layout,
                                                       {{0, palette, 0, palette->GetDesc().Bytes},
                                                        {1,
                                                         morphDeltas ? morphDeltas : palette,
                                                         0,
                                                         (morphDeltas ? morphDeltas : palette)->GetDesc().Bytes}}}));
    }
    Bounds DeformedBounds(const MeshData& mesh, const SkinningPose* pose)
    {
        glm::vec3 extent(0);
        for (const auto& target : mesh.MorphTargets)
        {
            float weight = target.DefaultWeight;
            if (pose)
                for (size_t k = 0; k < pose->ShapeKeys.size() && k < pose->MorphWeights.size(); ++k)
                    if (pose->ShapeKeys[k].Name == target.Name)
                    {
                        weight = pose->MorphWeights[k];
                        break;
                    }
            extent +=
                glm::max(glm::abs(target.PositionBounds.Min), glm::abs(target.PositionBounds.Max)) * std::abs(weight);
        }
        if (mesh.Skin.empty() || !pose)
        {
            auto result = mesh.LocalBounds;
            result.Min -= extent;
            result.Max += extent;
            return result;
        }
        Bounds result;
        for (size_t j = 0; j < mesh.JointBounds.size() && j < pose->Palette.size(); ++j)
        {
            if (!mesh.JointBounds[j].IsValid())
                continue;
            auto       box    = mesh.JointBounds[j].Transformed(pose->Palette[j]);
            const auto matrix = glm::mat3(pose->Palette[j] * mesh.SkinBindTransform);
            const auto e =
                glm::abs(matrix[0]) * extent.x + glm::abs(matrix[1]) * extent.y + glm::abs(matrix[2]) * extent.z;
            box.Min -= e;
            box.Max += e;
            result.Expand(box);
        }
        return result;
    }
    Bounds SkinnedBounds(const std::vector<Bounds>& boxes, const SkinningPose& pose)
    {
        Bounds bounds;
        for (size_t j = 0; j < boxes.size() && j < pose.Palette.size(); ++j)
            if (boxes[j].IsValid())
                bounds.Expand(boxes[j].Transformed(pose.Palette[j]));
        return bounds; // A convex weighted vertex lies inside the union AABB of all influenced transformed boxes.
    }
} // namespace Kans
