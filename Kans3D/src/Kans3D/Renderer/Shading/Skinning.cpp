#include "kspch.h"
#include "Kans3D/Renderer/Shading/Skinning.h"
#include <shaderc/shaderc.hpp>
#include <spirv_cross/spirv_glsl.hpp>
#include "Kans3D/Renderer/Shading/DepthNormals.h"
namespace Kans
{
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
                                compiler.set_name(input.id, "kans_Position");
                                position = true;
                            }
                            if (location == 1)
                            {
                                compiler.set_name(input.id, "kans_Normal");
                                normal = true;
                            }
                            if (location == 7)
                            {
                                compiler.set_name(input.id, "kans_SmoothNormal");
                                smoothNormal = true;
                            }
                            if (location == 4)
                            {
                                compiler.set_name(input.id, "kans_Tangent");
                                tangent = true;
                            }
                        }
                        if (!position)
                            throw std::runtime_error("Skinned vertex shader requires position at location 0");
                        std::string source = compiler.compile();
                        auto        input  = [&](const char* type, const char* name, const char* raw) {
                            const std::string declaration = std::string("in ") + type + " " + name + ";";
                            const auto        at          = source.find(declaration);
                            if (at == std::string::npos)
                                throw std::runtime_error("Cannot build skinning vertex prologue");
                            source.replace(at,
                                           declaration.size(),
                                           std::string("in ") + type + " " + raw + ";\n" + type + " " + name + ";");
                        };
                        input("vec3", "kans_Position", "kans_RawPosition");
                        if (normal)
                            input("vec3", "kans_Normal", "kans_RawNormal");
                        if (smoothNormal)
                            input("vec3", "kans_SmoothNormal", "kans_RawSmoothNormal");
                        if (tangent)
                            input("vec4", "kans_Tangent", "kans_RawTangent");
                        // SPIRV-Cross lowers the vertex include's private initializer into main.
                        // Keep its source at the raw input, before both morph accumulation and skinning.
                        const std::string rest = "kans_RestPosition = kans_Position;";
                        if (const auto at = source.find(rest); at != std::string::npos)
                            source.replace(at, rest.size(), "kans_RestPosition = kans_RawPosition;");
                        const auto main = source.find("void main()");
                        if (main == std::string::npos)
                            throw std::runtime_error("Missing shader main");
                        source.replace(main, 11, "void kans_MaterialMain()");
                        source += R"(
layout(location=5) in uvec4 kans_Joints;
layout(location=6) in vec4 kans_Weights;
layout(set=2,binding=0,std430) readonly buffer KansSkinningPalette { mat4 kans_Palette[]; };
layout(set=2,binding=1,std430) readonly buffer KansMorphDeltas { uint kans_Deltas[]; };
vec3 kans_MorphVector(uint offset) {
    return vec3(uintBitsToFloat(kans_Deltas[offset]),uintBitsToFloat(kans_Deltas[offset+1]),
                uintBitsToFloat(kans_Deltas[offset+2]));
}
void main() {
    vec3 p=kans_RawPosition;
)";
                        if (smoothNormal)
                            source += "vec3 sn=kans_RawSmoothNormal;\n";
                        if (normal)
                            source += "vec3 n=kans_RawNormal;\n";
                        if (tangent)
                            source += "vec3 t=kans_RawTangent.xyz;\n";
                        // Palette matrices retain their A1 ABI. A tagged final matrix describes the active
                        // list appended after the joints. Untagged legacy palettes have no morphs.
                        source += R"(
    mat4 header=kans_Palette[kans_Palette.length()-1];
    if(header[3][3]==-2718.0) {
        uint first=uint(header[0][0]), count=uint(header[0][1]);
 )";
                        source += normalLines ? "uint "
                                                "vertex=kans_Deltas[kans_Deltas[0]+uint(gl_VertexIndex)/2];\n" :
                                                "uint vertex=uint(gl_VertexIndex);\n";
                        source += R"(
        for(uint i=0; i<count; ++i) {
            vec4 kans_Active=kans_Palette[first+i][0];
            uint key=1+uint(kans_Active.y)*5, size=kans_Deltas[key], indices=kans_Deltas[key+1];
            uint lo=0, hi=size;
            while(lo<hi) {
                uint mid=lo+(hi-lo)/2;
                if(kans_Deltas[indices+mid]<vertex) lo=mid+1;
                else hi=mid;
            }
            if(lo==size || kans_Deltas[indices+lo]!=vertex) continue;
            uint base=lo*3;
            p+=kans_Active.x*kans_MorphVector(kans_Deltas[key+2]+base);
)";
                        // An absent stream aliases the ordinary normal, including its morph deltas.
                        // Authored welded streams deliberately retain their bind-pose direction.
                        if (smoothNormal && smoothNormalFallback)
                            source += "if(kans_Deltas[key+3]!=0) "
                                      "sn+=kans_Active.x*kans_MorphVector(kans_Deltas[key+3]+base);\n";
                        if (normal)
                            source += "if(kans_Deltas[key+3]!=0) "
                                      "n+=kans_Active.x*kans_MorphVector(kans_Deltas[key+3]+base);\n";
                        if (tangent)
                            source += "if(kans_Deltas[key+4]!=0) "
                                      "t+=kans_Active.x*kans_MorphVector(kans_Deltas[key+4]+base);\n";
                        source += R"(
        }
    }
    mat4 skin=mat4(1);
    if(dot(kans_Weights,vec4(1))>0)
        skin=kans_Palette[kans_Joints.x]*kans_Weights.x+kans_Palette[kans_Joints.y]*kans_Weights.y+
             kans_Palette[kans_Joints.z]*kans_Weights.z+kans_Palette[kans_Joints.w]*kans_Weights.w;
    kans_Position=(skin*vec4(p,1)).xyz;
)";
                        if (smoothNormal)
                            source += "kans_SmoothNormal=normalize(transpose(inverse(mat3(skin)))*sn);\n";
                        if (normal)
                            source += "kans_Normal=normalize(transpose(inverse(mat3(skin)))*n);\n";
                        if (tangent)
                            source += "kans_Tangent=vec4(normalize(mat3(skin)*t),kans_RawTangent.w*(determinant(mat3("
                                      "skin))<0 ? -1 : 1));\n";
                        source += "kans_MaterialMain();\n}\n";
                        shaderc::Compiler       shaderCompiler;
                        shaderc::CompileOptions compileOptions;
                        compileOptions.SetTargetEnvironment(shaderc_target_env_vulkan, shaderc_env_version_vulkan_1_2);
                        sources[module.Stage] = source;
                        auto compiled = shaderCompiler.CompileGlslToSpv(
                            source, shaderc_vertex_shader, "KansSkinning.vert", compileOptions);
                        if (compiled.GetCompilationStatus() != shaderc_compilation_status_success)
                            throw std::runtime_error(compiled.GetErrorMessage());
                        module.Words.assign(compiled.cbegin(), compiled.cend());
                        module.EntryPoint = "main";
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
