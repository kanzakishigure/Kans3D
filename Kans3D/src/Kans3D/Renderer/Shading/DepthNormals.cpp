#include "kspch.h"
#include "DepthNormals.h"
#include <shaderc/shaderc.hpp>
#include <spirv_cross/spirv_glsl.hpp>
#include "Kans3D/Asset/Runtime/MaterialShaderInterface.h"
#include "Kans3D/Asset/Runtime/StandardShader.h"

namespace Kans
{
    namespace
    {
        class EngineIncludes final : public shaderc::CompileOptions::IncluderInterface
        {
            struct Include
            {
                std::string            Name;
                shaderc_include_result Result {};
            };

        public:
            shaderc_include_result*
            GetInclude(const char* requested, shaderc_include_type, const char*, size_t) override
            {
                auto* include    = new Include {requested};
                auto& result     = include->Result;
                result.user_data = include;
                if (const auto* text = FindEngineShaderInclude(include->Name))
                {
                    result.source_name        = include->Name.c_str();
                    result.source_name_length = include->Name.size();
                    result.content            = text->data();
                    result.content_length     = text->size();
                }
                else
                {
                    result.source_name    = "";
                    result.content        = "Unknown engine include";
                    result.content_length = std::strlen(result.content);
                }
                return &result;
            }
            void ReleaseInclude(shaderc_include_result* result) override
            {
                delete static_cast<Include*>(result->user_data);
            }
        };
    } // namespace

    void MakeSurfacePositionInvariant(ShaderData& data)
    {
        if (data.Domain != ShaderDomain::Surface)
            return;
        const auto patch = [](ShaderProgramCode& code) {
            for (auto& module : code.Modules)
            {
                if (module.Stage != ShaderProductStage::Vertex)
                    continue;
                auto&                 words = module.Words;
                std::vector<uint32_t> decorations;
                size_t                insertion = words.size();
                for (size_t at = 5; at < words.size(); at += words[at] >> 16)
                {
                    const auto op = spv::Op(words[at] & 0xffff);
                    // Annotations precede all type declarations in SPIR-V's logical layout.
                    if (op >= spv::OpTypeVoid && op <= spv::OpTypeForwardPointer)
                        insertion = std::min(insertion, at);
                    const bool member = op == spv::OpMemberDecorate;
                    if (op != spv::OpDecorate && !member)
                        continue;
                    const size_t decoration = at + (member ? 3 : 2);
                    if (words[decoration] != spv::DecorationBuiltIn || words[decoration + 1] != spv::BuiltInPosition)
                        continue;
                    bool invariant = false;
                    for (size_t other = 5; other < words.size(); other += words[other] >> 16)
                        if (spv::Op(words[other] & 0xffff) == op && words[other + 1] == words[at + 1] &&
                            (!member || words[other + 2] == words[at + 2]) &&
                            words[other + (member ? 3 : 2)] == spv::DecorationInvariant)
                            invariant = true;
                    if (!invariant)
                    {
                        decorations.push_back((uint32_t(member ? 4 : 3) << 16) | uint32_t(op));
                        decorations.push_back(words[at + 1]);
                        if (member)
                            decorations.push_back(words[at + 2]);
                        decorations.push_back(spv::DecorationInvariant);
                    }
                }
                words.insert(words.begin() + insertion, decorations.begin(), decorations.end());
            }
        };
        patch(data.Code);
        for (auto& pass : data.Passes)
            patch(pass.Code);
    }

    std::variant<ShaderData, AssetError> MakeStandardDepthNormalsShader(const ShaderData& surface)
    {
        static const auto fragment = [] {
            // Reuse Standard's parameter ABI and exact tangent/derivative normal evaluation.
            auto text = GetStandardShader().DebugSources.at(ShaderProductStage::Fragment);
            text.resize(text.find("void main()"));
            text += R"(
#include <Kans3D/DepthNormals.glsl>
void main()
{
    vec2 uv = v_UV * p.UVTiling + p.UVOffset;
    vec4 baseColor = p.BaseColor * mix(vec4(1), v_Color, p.VertexColorStrength) * texture(BaseColorMap, uv);
    KansAlphaClip(baseColor.a);
    vec3 N = StandardNormal(uv);
    if (u_Object.MaterialRender.z != 0.0 && !gl_FrontFacing)
        N = -N;
    o_Color = KansNormalRoughness(N, p.Roughness * texture(MetallicRoughnessMap, uv).g);
}
)";
            return text;
        }();
        // Compile only the replacement fragment, once. Never recompile the vertex
        // source: serialized products and patched skin/morph modules are authoritative.
        static const std::variant<ShaderModuleCode, AssetError> compiled =
            []() -> std::variant<ShaderModuleCode, AssetError> {
            shaderc::Compiler       compiler;
            shaderc::CompileOptions options;
            options.SetTargetEnvironment(shaderc_target_env_vulkan, shaderc_env_version_vulkan_1_2);
            options.SetWarningsAsErrors();
            options.AddMacroDefinition("KANS_FRAGMENT_STAGE");
            options.SetIncluder(std::make_unique<EngineIncludes>());
            const auto product =
                compiler.CompileGlslToSpv(fragment, shaderc_fragment_shader, "StandardDepthNormals.frag", options);
            if (product.GetCompilationStatus() != shaderc_compilation_status_success)
                return AssetError {AssetErrorCode::InvalidData, product.GetErrorMessage()};
            return ShaderModuleCode {ShaderProductStage::Fragment, "main", {product.cbegin(), product.cend()}};
        }();
        if (const auto* error = std::get_if<AssetError>(&compiled))
            return *error;
        auto result = surface;
        result.Passes.clear();
        result.DebugName                                  = "StandardDepthNormals";
        result.DebugSources[ShaderProductStage::Fragment] = fragment;
        for (auto& module : result.Code.Modules)
            if (module.Stage == ShaderProductStage::Fragment)
                module = std::get<ShaderModuleCode>(compiled);
        return result;
    }

    static std::variant<ShaderData, AssetError>
    MakeFlagShader(const ShaderProgramCode& code, bool replace, uint32_t location)
    {
        try
        {
            ShaderData result;
            result.Code      = code;
            result.DebugName = replace ? "DepthOnlyPrepass" : "PrepassNormalInvalidation";
            for (auto& module : result.Code.Modules)
                if (module.Stage == ShaderProductStage::Fragment)
                {
                    spirv_cross::CompilerGLSL cross(module.Words);
                    auto                      options = cross.get_common_options();
                    options.version                   = 450;
                    options.vulkan_semantics          = true;
                    cross.set_common_options(options);
                    const auto outputs = cross.get_shader_resources().stage_outputs;
                    if (replace && !outputs.empty())
                        cross.set_name(outputs[0].id, "kans_PrepassFlag");
                    std::string source = cross.compile();
                    const auto  main   = source.find("void main()");
                    if (main == std::string::npos)
                        throw std::runtime_error("DepthOnly pass has no fragment main");
                    source.replace(main, 11, "void kans_DepthMain()");
                    if (!replace || outputs.empty())
                        source += "\nlayout(location=" + std::to_string(location) + ") out vec4 kans_PrepassFlag;\n";
                    // Wrapping rather than replacing Main preserves discard, early returns,
                    // texture-driven alpha and authored fragment depth. No surface function
                    // exists in the material interface from which to derive a shading normal.
                    source += replace ? "\nvoid main(){ kans_DepthMain(); kans_PrepassFlag=vec4(0,0,0,2); }\n" :
                                        "\nvoid main(){ kans_DepthMain(); kans_PrepassFlag=vec4(0); }\n";
                    shaderc::Compiler       compiler;
                    shaderc::CompileOptions compileOptions;
                    compileOptions.SetTargetEnvironment(shaderc_target_env_vulkan, shaderc_env_version_vulkan_1_2);
                    auto compiled = compiler.CompileGlslToSpv(
                        source, shaderc_fragment_shader, "DepthOnlyPrepass.frag", compileOptions);
                    if (compiled.GetCompilationStatus() != shaderc_compilation_status_success)
                        throw std::runtime_error(compiled.GetErrorMessage());
                    module.Words.assign(compiled.cbegin(), compiled.cend());
                    module.EntryPoint                 = "main";
                    result.DebugSources[module.Stage] = std::move(source);
                }
            return result;
        }
        catch (const std::exception& error)
        {
            return AssetError {AssetErrorCode::InvalidData, error.what()};
        }
    }
    std::variant<ShaderData, AssetError> MakeDepthOnlyPrepassShader(const ShaderProgramCode& code)
    {
        return MakeFlagShader(code, true, 0);
    }
    std::variant<ShaderData, AssetError> MakePrepassInvalidationShader(const ShaderProgramCode& code, uint32_t location)
    {
        return MakeFlagShader(code, false, location);
    }
} // namespace Kans
