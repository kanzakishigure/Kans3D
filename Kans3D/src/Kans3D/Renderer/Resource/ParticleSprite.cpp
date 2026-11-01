#include "kspch.h"
#include "ParticleSprite.h"
#include <shaderc/shaderc.hpp>
#include <spirv_cross/spirv_glsl.hpp>
namespace Kans
{
    namespace
    {
        const char* Uniforms = R"(
layout(set=2,binding=2,std140) uniform KansSprite {
mat4 owner;mat4 view;mat4 projection;mat4 inverseView;mat4 inverseProjection;
uvec4 offsets0;uvec4 offsets1;vec4 pivotGrid;vec4 facing;vec4 alignment;vec4 options;
} sprite;
)";
    }
    std::variant<ShaderData, AssetError> MakeParticleSpriteShader(const ShaderData& data)
    {
        try
        {
            auto result  = data;
            auto convert = [&](ShaderProgramCode& code) {
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
                            auto name = "kans_SpriteInput" + std::to_string(loc);
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
                                cross.set_name(v.id, "kans_MaterialUV");
                                uvInput = "kans_MaterialUV";
                            }
                        for (const auto& v : cross.get_shader_resources().stage_outputs)
                            if (cross.get_decoration(v.id, spv::DecorationLocation) == 0)
                            {
                                cross.set_name(v.id, "kans_SpriteOutput");
                                colorOutput = "kans_SpriteOutput";
                            }
                    }
                    else
                        continue;
                    auto source = cross.compile();
                    auto at     = source.find("void main()");
                    if (at == std::string::npos)
                        throw std::invalid_argument("Particle sprite material has no main");
                    source.replace(at, 11, "void kans_SpriteMaterialMain()");
                    source += Uniforms;
                    if (module.Stage == ShaderProductStage::Vertex)
                    {
                        for (const auto& [loc, name] : inputs)
                        {
                            std::string type = loc == 0 || loc == 1 || loc == 7 ? "vec3" : loc == 2 ? "vec2" : "vec4";
                            const auto  declaration = "in " + type + " " + name + ";";
                            auto        pos         = source.find(declaration);
                            if (pos == std::string::npos)
                                throw std::invalid_argument("Cannot derive particle sprite vertex input: " + name);
                            auto end  = pos + declaration.size();
                            auto line = source.rfind('\n', pos);
                            source.replace(line + 1, end - line - 1, type + " " + name + ";");
                        }
                        source += R"(
layout(set=2,binding=0,std430) readonly buffer KansSpriteAttributes{uvec4 attributes[];};
layout(set=2,binding=1,std430) readonly buffer KansSpriteAlive{uint alive[];};
layout(location=11) out vec4 kans_ParticleColor;
layout(location=12) out vec4 kans_ParticleDynamic;
layout(location=13) out vec3 kans_ParticleSubUV;
vec4 particle(uint attributeIndex,uint slot){return uintBitsToFloat(attributes[attributeIndex*sprite.offsets1.w+slot]);}
void main(){uint slot=alive[gl_InstanceIndex];vec3 position=particle(sprite.offsets0.x,slot).xyz;
if(sprite.options.w>0.5)position=(sprite.owner*vec4(position,1)).xyz;
vec3 right=sprite.inverseView[0].xyz,up=sprite.inverseView[1].xyz,normal=normalize(cross(right,up));
int mode=int(sprite.options.x);
if(mode==1){normal=normalize(sprite.inverseView[3].xyz-position);right=normalize(cross(up,normal));up=normalize(cross(normal,right));}
if(mode==2){vec3 velocity=particle(sprite.offsets1.x,slot).xyz;if(sprite.options.w>0.5)velocity=mat3(sprite.owner)*velocity;
vec3 projected=velocity-normal*dot(normal,velocity);if(dot(projected,projected)>1e-12){up=normalize(projected);right=normalize(cross(up,normal));}}
if(mode==3){normal=normalize(sprite.facing.xyz);up=normalize(sprite.alignment.xyz);right=normalize(cross(up,normal));up=normalize(cross(normal,right));}
vec2 corners[6]=vec2[](vec2(0,0),vec2(1,0),vec2(1,1),vec2(0,0),vec2(1,1),vec2(0,1));vec2 uv=corners[gl_VertexIndex%6];
vec2 corner=(uv-sprite.pivotGrid.xy)*particle(sprite.offsets0.z,slot).xy;float rotation=particle(sprite.offsets0.w,slot).x;
corner=mat2(cos(rotation),sin(rotation),-sin(rotation),cos(rotation))*corner;
position+=right*corner.x+up*corner.y;
float frame=particle(sprite.offsets1.y,slot).x;float total=sprite.pivotGrid.z*sprite.pivotGrid.w;
float current=mod(floor(frame),total),next=mod(current+1,total);
vec2 cell=vec2(mod(current,sprite.pivotGrid.z),floor(current/sprite.pivotGrid.z));
vec2 cellNext=vec2(mod(next,sprite.pivotGrid.z),floor(next/sprite.pivotGrid.z));
vec2 atlasUV=(cell+uv)/sprite.pivotGrid.zw;
kans_ParticleColor=particle(sprite.offsets0.y,slot);
kans_ParticleDynamic=particle(sprite.offsets1.z,slot);
kans_ParticleSubUV=vec3((cellNext+uv)/sprite.pivotGrid.zw,fract(frame)*sprite.options.z);
)";
                        for (const auto& [loc, name] : inputs)
                        {
                            if (loc == 0)
                                source += name + "=position;\n";
                            else if (loc == 1 || loc == 7)
                                source += name + "=normal;\n";
                            else if (loc == 2)
                                source += name + "=atlasUV;\n";
                            else if (loc == 3)
                                source += name + "=particle(sprite.offsets0.y,slot);\n";
                            else if (loc == 4)
                                source += name + "=vec4(right,1);\n";
                            else
                                throw std::invalid_argument("Unsupported particle material vertex input");
                        }
                        source += "kans_SpriteMaterialMain();}\n";
                    }
                    else
                    {
                        const auto mainAt = source.find("void kans_SpriteMaterialMain()");
                        source.insert(mainAt,
                                      "layout(location=11) in vec4 kans_ParticleColor;\nlayout(location=12) in vec4 "
                                      "kans_ParticleDynamic;\nlayout(location=13) in vec3 kans_ParticleSubUV;\n");
                        if (!uvInput.empty())
                        {
                            auto declaration = std::string("in vec2 kans_MaterialUV;");
                            auto pos         = source.find(declaration);
                            if (pos != std::string::npos)
                                source.replace(
                                    pos, declaration.size(), "in vec2 kans_RawMaterialUV;\nvec2 kans_MaterialUV;");
                        }
                        for (const auto& pair : std::vector<std::pair<std::string, std::string>> {
                                 {"KansVertexColor", "kans_ParticleColor"},
                                 {"KansVertexDynamicMaterialParameter", "kans_ParticleDynamic"},
                                 {"KansVertexSubUV", "kans_ParticleSubUV"}})
                        {
                            auto pos = source.find(pair.first + " = ");
                            if (pos != std::string::npos)
                            {
                                auto end = source.find(';', pos);
                                source.replace(pos, end - pos, pair.first + " = " + pair.second);
                            }
                        }
                        source += R"(
layout(set=2,binding=3) uniform sampler2D kans_SpriteDepth;
void main(){
)";
                        if (!uvInput.empty())
                            source += "kans_MaterialUV=kans_RawMaterialUV;\n";
                        source += "kans_SpriteMaterialMain();\n";
                        if (!uvInput.empty() && !colorOutput.empty())
                            source +=
                                "if(kans_ParticleSubUV.z>0.0){vec4 "
                                "first=kans_SpriteOutput;kans_MaterialUV=kans_ParticleSubUV.xy;kans_SpriteMaterialMain("
                                ");kans_SpriteOutput=mix(first,kans_SpriteOutput,kans_ParticleSubUV.z); }\n";
                        if (!colorOutput.empty())
                            source += R"(
if(sprite.options.y>0){vec2 uv=gl_FragCoord.xy/vec2(textureSize(kans_SpriteDepth,0));float opaque=texture(kans_SpriteDepth,uv).r;
vec4 a=sprite.inverseProjection*vec4(uv*2-1,opaque,1);vec4 b=sprite.inverseProjection*vec4(uv*2-1,gl_FragCoord.z,1);
float fade=clamp((abs(a.z/a.w)-abs(b.z/b.w))/sprite.options.y,0,1);
kans_SpriteOutput.a*=fade;if(sprite.facing.w>0.5)kans_SpriteOutput.rgb*=fade;}
)";
                        source += "}\n";
                    }
                    shaderc::Compiler       compiler;
                    shaderc::CompileOptions compileOptions;
                    compileOptions.SetTargetEnvironment(shaderc_target_env_vulkan, shaderc_env_version_vulkan_1_2);
                    auto compiled = compiler.CompileGlslToSpv(
                        source,
                        module.Stage == ShaderProductStage::Vertex ? shaderc_vertex_shader : shaderc_fragment_shader,
                        "ParticleSprite",
                        compileOptions);
                    if (compiled.GetCompilationStatus() != shaderc_compilation_status_success)
                        throw std::invalid_argument(compiled.GetErrorMessage());
                    module.Words.assign(compiled.cbegin(), compiled.cend());
                    module.EntryPoint = "main";
                }
            };
            convert(result.Code);
            for (auto& pass : result.Passes)
                convert(pass.Code);
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
