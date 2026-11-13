#include "kspch.h"
#include "ShaderImporter.h"
#include <algorithm>
#include <functional>
#include <shaderc/shaderc.hpp>
#include "Kans3D/Asset/Product/ShaderProduct.h"
#include "Kans3D/Asset/Runtime/EngineShaderText.h"
#include "Kans3D/Asset/Runtime/MaterialShaderInterface.h"
#include "Kans3D/Asset/Storage/ProductFiles.h"
#include "ShaderProgramSource.h"

namespace Kans
{
    namespace
    {
        namespace fs = std::filesystem;

        constexpr uint64_t MaximumFileBytes  = 1024 * 1024;
        constexpr uint64_t MaximumTotalBytes = 16 * 1024 * 1024;

        // Shader sources by asset-root-relative name. In discovery mode files are
        // read from disk on first use; in import mode only declared inputs exist.
        class ShaderInputs
        {
        public:
            ShaderInputs(const ImportContext& context, bool discover, bool embedded = false) :
                m_Context(context), m_Discover(discover), m_Embedded(embedded)
            {}

            void Add(const std::string& name, const std::string& text) { m_Texts.emplace(name, text); }

            // Returns the text for a relative name, or nullptr if unavailable.
            const std::string* Find(const std::string& name)
            {
                const auto found = m_Texts.find(name);
                if (found != m_Texts.end())
                    return &found->second;
                if (m_Embedded)
                    return nullptr; // Internal programs resolve exclusively from supplied memory.
                const auto file = (m_Context.AssetRoot / fs::u8path(name)).lexically_normal();
                if (!m_Discover && file != m_Context.SourceFile.lexically_normal() &&
                    !m_Context.SourceInputs.count(file))
                {
                    m_Undeclared = file;
                    return nullptr;
                }
                auto bytes = ReadProductFile(file, MaximumFileBytes);
                if (std::holds_alternative<AssetError>(bytes))
                    return nullptr;
                const auto& value = std::get<std::vector<std::byte>>(bytes);
                if (value.empty() || m_TotalBytes + value.size() > MaximumTotalBytes ||
                    std::find(value.begin(), value.end(), std::byte {0}) != value.end())
                    return nullptr;
                m_TotalBytes += value.size();
                if (file != m_Context.SourceFile.lexically_normal())
                    m_Files.push_back(file);
                return &m_Texts.emplace(name, std::string(reinterpret_cast<const char*>(value.data()), value.size()))
                            .first->second;
            }
            // Relative name of a file inside the asset root, or empty if outside.
            std::string NameOf(const fs::path& file) const
            {
                const AssetPath logical(file.lexically_normal().lexically_relative(m_Context.AssetRoot));
                return logical.IsValid() ? logical.ToString() : std::string();
            }
            const std::vector<fs::path>& Files() const { return m_Files; }
            const std::string*           FindEmbeddedRoot(const std::string& name) const
            {
                const auto found = m_Texts.find(name);
                return m_Embedded && found != m_Texts.end() ? &found->second : nullptr;
            }
            const fs::path&              Undeclared() const { return m_Undeclared; }

        private:
            const ImportContext&               m_Context;
            bool                               m_Discover;
            bool                               m_Embedded;
            std::map<std::string, std::string> m_Texts;
            std::vector<fs::path>              m_Files;
            fs::path                           m_Undeclared;
            uint64_t                           m_TotalBytes = 0;
        };

        // Resolves relative #include directives through ShaderInputs.
        class Includer final : public shaderc::CompileOptions::IncluderInterface
        {
        public:
            explicit Includer(ShaderInputs& inputs) : m_Inputs(inputs) {}

            shaderc_include_result*
            GetInclude(const char* requested, shaderc_include_type type, const char* requesting, size_t depth) override
            {
                try
                {
                    if (!requested || !requesting || depth > 16)
                        return Failure("Invalid include");
                    const std::string name(requested);
                    if (type == shaderc_include_type_standard)
                    {
                        // Engine headers are covered by GetEnvironmentFingerprint(), not project file inputs.
                        const auto* text = FindEngineShaderInclude(name);
                        if (!text)
                            return Failure("Unknown engine include");
                        return Result(name, *text);
                    }
                    if (name.empty() || name.find('\\') != std::string::npos || name.find(':') != std::string::npos)
                        return Failure("Invalid include path");
                    const auto relative = (fs::u8path(requesting).parent_path() / fs::u8path(name)).lexically_normal();
                    const AssetPath logical(relative);
                    const auto*     text = logical.IsValid() ? m_Inputs.Find(logical.ToString()) : nullptr;
                    // Embedded programs may supply virtual include roots, never filesystem roots.
                    if (!text)
                        if (const auto* rootText = m_Inputs.FindEmbeddedRoot(name))
                            return Result(name, *rootText);
                    if (!text)
                        return Failure("Include not found or not declared");
                    return Result(logical.ToString(), *text);
                }
                catch (...)
                {
                    return Failure("Include failed");
                }
            }
            void ReleaseInclude(shaderc_include_result* result) override
            {
                if (result && result != &m_Failure)
                    delete static_cast<Owned*>(result->user_data);
            }

        private:
            struct Owned
            {
                std::string            Name;
                shaderc_include_result Result;
            };
            // `text` must outlive the compilation.
            shaderc_include_result* Result(std::string name, const std::string& text)
            {
                auto* value                      = new Owned {std::move(name), {}};
                value->Result.source_name        = value->Name.c_str();
                value->Result.source_name_length = value->Name.size();
                value->Result.content            = text.data();
                value->Result.content_length     = text.size();
                value->Result.user_data          = value;
                return &value->Result;
            }
            shaderc_include_result* Failure(const char* message) noexcept
            {
                m_Failure = {"", 0, message, std::strlen(message), nullptr};
                return &m_Failure;
            }
            ShaderInputs&          m_Inputs;
            shaderc_include_result m_Failure {};
        };

        // Compiles the source (a stage file or a .shaderprog listing stage files and
        // parameter defaults).
        std::variant<ShaderData, AssetError> CompileShader(const ImportContext& context, ShaderInputs& inputs)
        {
            auto extension = context.SourceFile.extension().u8string();
            std::transform(extension.begin(), extension.end(), extension.begin(), [](unsigned char c) {
                return static_cast<char>(std::tolower(c));
            });
            if (extension != ".vert" && extension != ".frag" && extension != ".comp" && extension != ".shaderprog")
                return AssetError {AssetErrorCode::InvalidArgument, "Unsupported Shader source extension"};
            const auto  primaryName = inputs.NameOf(context.SourceFile);
            const auto* primary     = primaryName.empty() ? nullptr : inputs.Find(primaryName);
            if (!primary)
                return AssetError {AssetErrorCode::NotFound, "Cannot read Shader source"};

            struct ModuleSource
            {
                ShaderProductStage Stage;
                std::string        Name;
                ShaderProgramCode* Code;
            };
            std::vector<ModuleSource>                               modules;
            ShaderData                                              shader;
            shader.DebugName = context.SourceFile.generic_u8string();
            if (extension == ".shaderprog")
            {
                auto domain = ParseShaderProgramDomain(*primary);
                if (auto* error = std::get_if<AssetError>(&domain))
                    return *error;
                shader.Domain    = std::get<ShaderDomainSource>(domain).Domain;
                shader.PostStage = std::get<ShaderDomainSource>(domain).Stage;
                if (shader.Domain == ShaderDomain::PostProcess)
                {
                    inputs.Add("__KansPost.vert", ShaderText::ImporterPostVertex);
                    modules.push_back({ShaderProductStage::Vertex, "__KansPost.vert", &shader.Code});
                }
                auto space = ParseShaderOutputSpace(*primary);
                if (auto* error = std::get_if<AssetError>(&space))
                    return *error;
                shader.OutputSpace = std::get<ShaderOutputSpace>(space);
                auto parsed = ParseShaderProgramSource(*primary);
                if (auto* error = std::get_if<AssetError>(&parsed))
                    return std::move(*error);
                auto defaults = ParseShaderProgramDefaults(*primary);
                if (auto* error = std::get_if<AssetError>(&defaults))
                    return std::move(*error);
                shader.ParameterDefaults = std::get<ShaderParameterDefaults>(std::move(defaults));
                auto hints               = ParseShaderProgramHints(*primary);
                if (auto* error = std::get_if<AssetError>(&hints))
                    return std::move(*error);
                shader.ParameterHints = std::get<ShaderParameterHints>(std::move(hints));
                auto fallbacks        = ParseShaderProgramTextureFallbacks(*primary);
                if (auto* error = std::get_if<AssetError>(&fallbacks))
                    return std::move(*error);
                auto groups = ParseShaderProgramGroups(*primary);
                if (auto* error = std::get_if<AssetError>(&groups))
                    return *error;
                shader.Groups           = std::get<std::vector<MaterialParameterGroup>>(std::move(groups));
                shader.TextureFallbacks = std::get<ShaderTextureFallbacks>(std::move(fallbacks));
                for (const auto& stage : std::get<std::vector<ShaderStageSource>>(parsed))
                    modules.push_back(
                        {stage.Stage,
                         (fs::u8path(primaryName).parent_path() / stage.Path.GetPath()).generic_u8string(),
                         &shader.Code});
                auto passes = ParseShaderProgramPasses(*primary);
                if (auto* error = std::get_if<AssetError>(&passes))
                    return *error;
                const auto& sources = std::get<std::vector<ShaderPassSource>>(passes);
                shader.Passes.reserve(sources.size());
                for (const auto& source : sources)
                {
                    shader.Passes.push_back({source.Name, source.Tag, {}, source.State, {}});
                    for (const auto& stage : source.Stages)
                        modules.push_back(
                            {stage.Stage,
                             (fs::u8path(primaryName).parent_path() / stage.Path.GetPath()).generic_u8string(),
                             &shader.Passes.back().Code});
                }
            }
            else
                modules.push_back({extension == ".vert" ? ShaderProductStage::Vertex :
                                   extension == ".frag" ? ShaderProductStage::Fragment :
                                                          ShaderProductStage::Compute,
                                   primaryName,
                                   &shader.Code});

            for (const auto& [stage, name, code] : modules)
            {
                const auto* text = inputs.Find(name);
                if (!text)
                    return AssetError {AssetErrorCode::NotFound, "Cannot read Shader stage source " + name};
                shaderc::Compiler       compiler;
                shaderc::CompileOptions options;
                options.SetTargetEnvironment(shaderc_target_env_vulkan, shaderc_env_version_vulkan_1_2);
                options.SetWarningsAsErrors();
                if (stage == ShaderProductStage::Fragment)
                    options.AddMacroDefinition(ShaderText::Depth_KANS_FRAGMENT_STAGE);
                options.SetIncluder(std::make_unique<Includer>(inputs));
                const auto kind     = stage == ShaderProductStage::Vertex   ? shaderc_vertex_shader :
                                      stage == ShaderProductStage::Fragment ? shaderc_fragment_shader :
                                                                              shaderc_compute_shader;
                const auto compiled = compiler.CompileGlslToSpv(*text, kind, name.c_str(), options);
                if (compiled.GetCompilationStatus() != shaderc_compilation_status_success)
                {
                    if (!inputs.Undeclared().empty())
                        return AssetError {AssetErrorCode::NotFound,
                                           "Shader input is not declared: " + inputs.Undeclared().u8string()};
                    return AssetError {AssetErrorCode::InvalidData,
                                       "Shader compilation failed: " + compiled.GetErrorMessage().substr(0, 4096)};
                }
                code->Modules.push_back({stage, "main", {compiled.cbegin(), compiled.cend()}});
                if (code == &shader.Code)
                    shader.DebugSources[stage] = *text;
                else
                    for (auto& pass : shader.Passes)
                        if (code == &pass.Code)
                            pass.DebugSources[stage] = *text;
            }
            auto valid = ValidateMaterialShaderPasses(shader.Code, shader.Passes);
            if (auto* error = std::get_if<AssetError>(&valid))
                return *error;
            return shader;
        }

        bool ValidContext(const ImportContext& context)
        {
            return context.SourceFile.is_absolute() && context.AssetRoot.is_absolute() && context.Settings.empty() &&
                   context.ProductsBySource.empty();
        }
    } // namespace

    std::variant<ShaderData, AssetError> CompileEmbeddedShaderProgram(const std::string& manifest,
                                                                      const std::map<std::string, std::string>& stages)
    {
        ImportContext context;
        context.AssetRoot  = "/__kans_engine__";
        context.SourceFile = context.AssetRoot / "Standard.shaderprog";
        ShaderInputs inputs(context, false, true);
        inputs.Add("Standard.shaderprog", manifest);
        for (const auto& [name, source] : stages)
            inputs.Add(name, source);
        auto compiled = CompileShader(context, inputs);
        if (auto* error = std::get_if<AssetError>(&compiled))
            return *error;
        const auto& shader = std::get<ShaderData>(compiled);
        auto        encoded = EncodeShaderProduct(shader.Code,
                                                  shader.ParameterDefaults,
                                                  shader.ParameterHints,
                                                  shader.TextureFallbacks,
                                                  shader.Groups,
                                                  shader.Passes,
                                                  shader.OutputSpace,
                                                  shader.Domain,
                                                  shader.PostStage);
        if (auto* error = std::get_if<AssetError>(&encoded))
            return *error;
        const auto& bytes = std::get<std::vector<std::byte>>(encoded);
        return DecodeShaderProduct(bytes.data(), bytes.size());
    }

    std::variant<ImportInputs, AssetError> ShaderImporter::DiscoverInputs(const ImportContext& context) const
    {
        if (!ValidContext(context))
            return AssetError {AssetErrorCode::InvalidArgument, "Invalid Shader import context"};
        ShaderInputs inputs(context, true);
        auto         compiled = CompileShader(context, inputs);
        if (auto* error = std::get_if<AssetError>(&compiled))
            return std::move(*error);
        ImportInputs result;
        result.SourceFiles = inputs.Files();
        return result;
    }

    std::variant<ImportResult, AssetError> ShaderImporter::Import(const ImportContext& context) const
    {
        if (!context.SourceID.IsValid() || !ValidContext(context) || !context.StagingDirectory.is_absolute())
            return AssetError {AssetErrorCode::InvalidArgument, "Invalid Shader import context"};
        if (context.Target != "vulkan12")
            return AssetError {AssetErrorCode::NotImplemented, "ShaderImporter requires the vulkan12 target"};
        try
        {
            ShaderInputs inputs(context, false);
            auto         compiled = CompileShader(context, inputs);
            if (auto* error = std::get_if<AssetError>(&compiled))
                return std::move(*error);
            const auto& shader  = std::get<ShaderData>(compiled);
            auto        encoded = EncodeShaderProduct(shader.Code,
                                                      shader.ParameterDefaults,
                                                      shader.ParameterHints,
                                                      shader.TextureFallbacks,
                                                      shader.Groups,
                                                      shader.Passes,
                                                      shader.OutputSpace,
                                                      shader.Domain,
                                                      shader.PostStage);
            if (auto* error = std::get_if<AssetError>(&encoded))
                return std::move(*error);
            auto allocated = AllocateLocalIDs(context.PreviousLocalIDs, {"shader:root"});
            if (auto* error = std::get_if<AssetError>(&allocated))
                return std::move(*error);
            ImportResult result;
            result.LocalIDs  = std::get<LocalIDMap>(std::move(allocated));
            const auto local = result.LocalIDs.at("shader:root");
            result.Products.push_back({local, AssetType::Shader, std::to_string(local) + ".kshd", {}});
            auto written = WriteProductFile(context.StagingDirectory / result.Products[0].StagedFile,
                                            std::get<std::vector<std::byte>>(encoded));
            if (auto* error = std::get_if<AssetError>(&written))
                return std::move(*error);
            return result;
        }
        catch (const std::exception& error)
        {
            return AssetError {AssetErrorCode::InvalidData,
                               "Shader import failed: " + std::string(error.what()).substr(0, 4096)};
        }
    }
} // namespace Kans
