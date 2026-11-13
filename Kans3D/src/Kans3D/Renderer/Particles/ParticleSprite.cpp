#include "kspch.h"
#include "Kans3D/Renderer/Particles/ParticleSprite.h"
#include <shaderc/shaderc.hpp>
#include <spirv_cross/spirv_glsl.hpp>
#include "Kans3D/Asset/Runtime/EngineShaderText.h"
#include "Kans3D/Asset/Runtime/StandardShader.h"
namespace Kans
{
    namespace
    {
        const char* Uniforms = ShaderText::Particle_layout_set_binding_std140;
    }
    std::variant<ShaderData, AssetError> MakeParticleSpriteShader(const ShaderData& data, bool mesh, bool ribbon)
    {
        try
        {
            auto result = data;
            if (!result.DebugName.empty())
                result.DebugName += ribbon ? "/ParticleRibbon" : mesh ? "/ParticleMesh" : "/ParticleSprite";
            result.DebugSources.clear();
            // Built-in particle lighting is absent from their ordinary mesh SPIR-V.
            // Select the matching source in the shared sprite/mesh/ribbon path.
            const auto matches = [&](const ShaderData& shader) {
                const auto& modules = shader.Code.Modules;
                return data.Code.Modules.size() == modules.size() &&
                       std::equal(data.Code.Modules.begin(),
                                  data.Code.Modules.end(),
                                  modules.begin(),
                                  [](const auto& a, const auto& b) {
                                      return a.Stage == b.Stage && a.EntryPoint == b.EntryPoint && a.Words == b.Words;
                                  });
            };
            if (matches(GetStandardShader()))
                result.Code = GetStandardShader(true).Code;
            else if (matches(GetSixWaySmokeShader()))
                result.Code = GetSixWaySmokeShader(true).Code;
            auto convert = [&](ShaderProgramCode& code, std::map<ShaderProductStage, std::string>& sources) {
                for (auto& module : code.Modules)
                {
                    spirv_cross::CompilerGLSL cross(module.Words);
                    auto                      options = cross.get_common_options();
                    options.version                   = 450;
                    options.vulkan_semantics          = true;
                    cross.set_common_options(options);
                    std::map<uint32_t, std::string> inputs;
                    std::string                     colorOutput, uvInput;
                    if (module.Stage == ShaderProductStage::Vertex)
                    {
                        for (const auto& v : cross.get_shader_resources().stage_inputs)
                        {
                            auto loc  = cross.get_decoration(v.id, spv::DecorationLocation);
                            auto name = ShaderText::Particle_kans_SpriteInput + std::to_string(loc);
                            cross.set_name(v.id, name);
                            inputs[loc] = name;
                        }
                    }
                    else if (module.Stage == ShaderProductStage::Fragment)
                    {
                        for (const auto& v : cross.get_shader_resources().stage_inputs)
                            if (cross.get_decoration(v.id, spv::DecorationLocation) == 0 &&
                                cross.get_type(v.base_type_id).vecsize == 2)
                            {
                                cross.set_name(v.id, ShaderText::Particle_kans_MaterialUV);
                                uvInput = ShaderText::Particle_kans_MaterialUV;
                            }
                        for (const auto& v : cross.get_shader_resources().stage_outputs)
                            if (cross.get_decoration(v.id, spv::DecorationLocation) == 0)
                            {
                                cross.set_name(v.id, ShaderText::Particle_kans_SpriteOutput);
                                colorOutput = ShaderText::Particle_kans_SpriteOutput;
                            }
                    }
                    else
                        continue;
                    auto source = cross.compile();
                    auto at     = source.find("void main()");
                    if (at == std::string::npos)
                        throw std::invalid_argument("Particle sprite material has no main");
                    source.replace(at, 11, ShaderText::Particle_void_kans_SpriteMaterialMain);
                    source.insert(at, Uniforms);
                    if (module.Stage == ShaderProductStage::Vertex)
                    {
                        if (!ribbon)
                            source.insert(source.find("void kans_SpriteMaterialMain()"),
                                          ShaderText::Particle_layout_set_binding_std430);
                        auto transmittance = source.find("KansParticleTransmittance = 1.0;");
                        if (!ribbon && transmittance != std::string::npos)
                            // SPIRV-Cross places private-variable initializers inside the material main.
                            // Load here so that main cannot overwrite the particle value with the default.
                            source.replace(transmittance,
                                           std::string(ShaderText::Particle_KansParticleTransmittance).size(),
                                           ShaderText::Particle_KansParticleTransmittance_sprite_smokeShadow_x);
                        auto evaluation = source.find("KansParticleEvaluation = 0.0;");
                        if (evaluation != std::string::npos)
                            source.replace(evaluation,
                                           std::string(ShaderText::Particle_KansParticleEvaluation).size(),
                                           ShaderText::Particle_KansParticleEvaluation_sprite_lighting_z);
                    }
                    if (module.Stage == ShaderProductStage::Vertex)
                    {
                        source += ShaderText::Particle_Kans_particle_geometry;
                        if (!mesh)
                            for (const auto& [loc, name] : inputs)
                            {
                                std::string type = loc == 0 || loc == 1 || loc == 7 ? ShaderText::Vec3 :
                                                   loc == 2                         ? ShaderText::Vec2 :
                                                                                      ShaderText::Vec4;
                                const auto  declaration =
                                    ShaderText::Skinning_in + type + ShaderText::Space + name + ShaderText::Semicolon;
                                auto        pos         = source.find(declaration);
                                if (pos == std::string::npos)
                                    throw std::invalid_argument("Cannot derive particle sprite vertex input: " + name);
                                auto end  = pos + declaration.size();
                                auto line = source.rfind('\n', pos);
                                source.replace(
                                    line + 1, end - line - 1, type + ShaderText::Space + name + ShaderText::Semicolon);
                            }
                        if (!mesh && !ribbon)
                            source += ShaderText::Particle_layout_location_out_vec4;
                        if (ribbon)
                            source += ShaderText::Particle_layout_set_binding_std4302;
                        if (mesh)
                        {
                            source += ShaderText::Particle_layout_location_out_vec42;
                            auto pos = source.find("KANS_POSITION");
                            source.replace(pos, 13, inputs.count(0) ? inputs.at(0) : ShaderText::Particle_vec3_2);
                            // Keep vertex attributes as inputs; feed the original material through a transformed
                            // private copy.
                            for (const auto& [loc, name] : inputs)
                            {
                                std::string type        = loc == 0 || loc == 1 || loc == 7 ? ShaderText::Vec3 :
                                                          loc == 2                         ? ShaderText::Vec2 :
                                                                                             ShaderText::Vec4;
                                std::string privateName = name + ShaderText::Particle_Value;
                                auto        main        = source.find("void kans_SpriteMaterialMain()");
                                auto        end         = source.find("// Kans particle geometry", main);
                                auto        body        = source.substr(main, end - main);
                                size_t      at          = 0;
                                while ((at = body.find(name, at)) != std::string::npos)
                                {
                                    body.replace(at, name.size(), privateName);
                                    at += privateName.size();
                                }
                                source.replace(main, end - main, body);
                                source.insert(source.find("void kans_SpriteMaterialMain()"),
                                              type + ShaderText::Space + privateName + ShaderText::EndStatement);
                                if (loc == 0)
                                    source += privateName + ShaderText::Particle_position;
                                else if (loc == 1 || loc == 7)
                                    source += privateName + ShaderText::Particle_normalize_normalTransform + name +
                                              ShaderText::Particle_Punctuation_4;
                                else if (loc == 3)
                                    source += privateName + ShaderText::Assign + name +
                                              ShaderText::Particle_kans_ParticleColor;
                                else if (loc == 4)
                                    source += privateName + ShaderText::Particle_vec4_normalize_transform + name +
                                              ShaderText::Particle_xyz + name +
                                              ShaderText::Particle_w_sign_determinant_transform;
                                else
                                    source += privateName + ShaderText::Assign + name + ShaderText::EndStatement;
                            }
                        }
                        if (!mesh)
                            for (const auto& [loc, name] : inputs)
                            {
                                if (loc == 0)
                                    source += name + ShaderText::Particle_position;
                                else if (loc == 1 || loc == 7)
                                    source += name + ShaderText::Particle_normal;
                                else if (loc == 2)
                                    source += name + ShaderText::Particle_atlasUV;
                                else if (loc == 3)
                                    source += name + (ribbon ? ShaderText::Particle_kans_ParticleColor_2 :
                                                               ShaderText::Particle_particle_sprite_offsets0_y);
                                else if (loc == 4)
                                    source += name + ShaderText::Particle_vec4_right;
                                else
                                    throw std::invalid_argument("Unsupported particle material vertex input");
                            }
                        source += ShaderText::Particle_kans_SpriteMaterialMain;
                    }
                    else
                    {
                        const auto mainAt = source.find("void kans_SpriteMaterialMain()");
                        source.insert(mainAt, ShaderText::Particle_layout_location_in_vec4);
                        if (ribbon)
                            source.insert(source.find("void kans_SpriteMaterialMain()"),
                                          ShaderText::Particle_layout_location_in_vec2);
                        if (!uvInput.empty())
                        {
                            auto declaration = std::string(ShaderText::Particle_in_vec2_kans_MaterialUV);
                            auto pos         = source.find(declaration);
                            if (pos != std::string::npos)
                                source.replace(
                                    pos, declaration.size(), ShaderText::Particle_in_vec2_kans_RawMaterialUV);
                        }
                        for (const auto& pair : std::vector<std::pair<std::string, std::string>> {
                                 {ShaderText::Particle_KansVertexColor, ShaderText::Particle_kans_ParticleColor_3},
                                 {ShaderText::Particle_KansVertexDynamicMaterialParameter,
                                  ShaderText::Particle_kans_ParticleDynamic},
                                 {ShaderText::Particle_KansVertexSubUV, ShaderText::Particle_kans_ParticleSubUV}})
                        {
                            auto pos = source.find(pair.first + ShaderText::Particle_Punctuation_6);
                            if (pos != std::string::npos)
                            {
                                auto end = source.find(';', pos);
                                source.replace(
                                    pos, end - pos, pair.first + ShaderText::Particle_Punctuation_6 + pair.second);
                            }
                        }
                        source += ShaderText::Particle_layout_set_binding_uniform;
                        if (!uvInput.empty())
                            source += ShaderText::Particle_kans_MaterialUV_kans_RawMaterialUV;
                        if (ribbon)
                        {
                            auto pos = source.find("KansVertexUV1 = ");
                            if (pos != std::string::npos)
                                source.replace(pos,
                                               source.find(';', pos) - pos,
                                               ShaderText::Particle_KansVertexUV1_kans_RibbonUV1);
                        }
                        source += ShaderText::Particle_kans_SpriteMaterialMain_2;
                        if (!uvInput.empty() && !colorOutput.empty())
                            source += ShaderText::Particle_if_kans_ParticleSubUV_z;
                        if (!colorOutput.empty())
                            source += ShaderText::Particle_if_sprite_options_y;
                        source += ShaderText::Particle_Punctuation_7;
                    }
                    shaderc::Compiler       compiler;
                    shaderc::CompileOptions compileOptions;
                    compileOptions.SetTargetEnvironment(shaderc_target_env_vulkan, shaderc_env_version_vulkan_1_2);
                    sources[module.Stage] = source;
                    auto compiled         = compiler.CompileGlslToSpv(
                        source,
                        module.Stage == ShaderProductStage::Vertex ? shaderc_vertex_shader : shaderc_fragment_shader,
                        ShaderText::Particle_ParticleSprite,
                        compileOptions);
                    if (compiled.GetCompilationStatus() != shaderc_compilation_status_success)
                        throw std::invalid_argument(compiled.GetErrorMessage());
                    module.Words.assign(compiled.cbegin(), compiled.cend());
                    module.EntryPoint = ShaderText::Main;
                }
            };
            convert(result.Code, result.DebugSources);
            for (auto& pass : result.Passes)
                convert(pass.Code, pass.DebugSources);
            result.Reflection.clear();
            return result;
        }
        catch (const std::exception& e)
        {
            return AssetError {AssetErrorCode::InvalidData, e.what()};
        }
    }
    RenderHardware::Handle<RenderHardware::BindGroupLayout> ParticleSpriteLayout(RenderHardware::Device& device)
    {
        using namespace RenderHardware;
        return device.CreateBindGroupLayout({2,
                                             {{0, BindingType::ReadOnlyStorageBuffer},
                                              {1, BindingType::ReadOnlyStorageBuffer},
                                              {2, BindingType::UniformBuffer},
                                              {3, BindingType::CombinedTextureSampler}}});
    }
} // namespace Kans
