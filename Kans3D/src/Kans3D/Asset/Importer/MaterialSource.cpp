#include "kspch.h"
#include "MaterialSource.h"
#include <cmath>
#include <cstdio>
#include <cstring>
#include <set>
#include <stdexcept>
#include <yaml-cpp/yaml.h>
#include "Kans3D/Asset/Product/MaterialRenderStateYaml.h"
#include "Kans3D/Asset/Product/ModelProduct.h"
#include "Kans3D/Asset/Runtime/MaterialBindings.h"
#include "Kans3D/Asset/Runtime/MaterialShaderInterface.h"
#include "Kans3D/Asset/Runtime/StandardShader.h"

namespace Kans
{
    namespace
    {
        constexpr size_t MaximumEntries = 4096;
        constexpr size_t MaximumValues  = 4096;

        AssetError Invalid(const std::string& message)
        {
            return AssetError {AssetErrorCode::InvalidData, "Material source: " + message};
        }

        bool ValidName(const std::string& name)
        {
            return !name.empty() && name.size() <= 256 &&
                   std::all_of(name.begin(), name.end(), [](unsigned char c) { return c >= 32 && c != 127; });
        }

        double Number(const YAML::Node& node)
        {
            if (!node.IsScalar())
                throw std::runtime_error("parameter values must be numbers or booleans");
            double value = 0;
            if (YAML::convert<double>::decode(node, value))
                return value;
            bool flag = false;
            if (YAML::convert<bool>::decode(node, flag))
                return flag ? 1.0 : 0.0;
            throw std::runtime_error("parameter value '" + node.Scalar() + "' is not a number");
        }

        void Flatten(const YAML::Node& node, std::vector<double>& values, int depth)
        {
            if (node.IsSequence() && depth < 2)
            {
                for (const auto& element : node)
                    Flatten(element, values, depth + 1);
                return;
            }
            if (values.size() >= MaximumValues)
                throw std::runtime_error("too many parameter values");
            values.push_back(Number(node));
        }

        // Shortest decimal text that reads back as the same float.
        std::string FormatNumber(double value)
        {
            // Shader integer parameters must retain every bit, including values
            // beyond binary32 precision. Float parameters still use short decimals.
            if (value != 0 && value == std::floor(value) && value >= INT32_MIN && value <= UINT32_MAX)
                return std::to_string(static_cast<int64_t>(value));
            char buffer[32];
            for (int precision = 6; precision <= 9; ++precision)
            {
                std::snprintf(buffer, sizeof(buffer), "%.*g", precision, value);
                if (static_cast<float>(std::strtod(buffer, nullptr)) == static_cast<float>(value))
                    break;
            }
            return buffer;
        }

        MaterialSource CanonicalSource(MaterialSource source)
        {
            if (source.Parent.IsValid() || !IsStandardShader(source.Shader))
                return source;
            const auto migrate = [](auto& values, const char* oldName, const char* newName) {
                auto node = values.extract(oldName);
                if (!node.empty())
                {
                    if (values.count(newName))
                        throw std::runtime_error(std::string("both legacy and Standard names supplied: ") + newName);
                    node.key() = newName;
                    values.insert(std::move(node));
                }
            };
            migrate(source.Parameters, "base_color", "BaseColor");
            migrate(source.Parameters, "metallic", "Metallic");
            migrate(source.Parameters, "roughness", "Roughness");
            // Old emissive textures emitted without a factor.
            if (source.Textures.count("emissive") && !source.Parameters.count("EmissiveColor"))
                source.Parameters["EmissiveColor"] = {1, 1, 1};
            migrate(source.Textures, "base_color", "BaseColorMap");
            migrate(source.Textures, "normal", "NormalMap");
            migrate(source.Textures, "metallic_roughness", "MetallicRoughnessMap");
            migrate(source.Textures, "occlusion", "OcclusionMap");
            migrate(source.Textures, "emissive", "EmissiveMap");
            return source;
        }

        std::variant<std::vector<ShaderStageReflection>, AssetError> MaterialReflection(const ShaderData& shader)
        {
            auto inspected = InspectShaderProgram(shader.Code);
            if (auto* error = std::get_if<AssetError>(&inspected))
                return std::move(*error);
            auto contract =
                ValidateMaterialShaderInterface(std::get<std::vector<ShaderStageReflection>>(inspected), shader.Domain);
            if (auto* error = std::get_if<AssetError>(&contract))
                return std::move(*error);
            return inspected;
        }
    } // namespace

    // Decode the actual packed values against this material's Shader. This also
    // preserves legacy parameter writes, arrays and row-major matrix layouts.
    std::variant<MaterialSource, AssetError> EditableMaterial(MaterialData data, const ShaderData& shader)
    {
        if (!data.Bindings && IsStandardShader(data.Shader))
        {
            auto resolved = ResolveStandardMaterial(data);
            if (const auto* error = std::get_if<AssetError>(&resolved))
                return *error;
            data = std::get<MaterialData>(std::move(resolved));
        }
        if (!data.Bindings)
            return AssetError {AssetErrorCode::InvalidData, "Embedded material has no binding recipe"};
        auto reflection = InspectShaderProgram(shader.Code);
        if (const auto* error = std::get_if<AssetError>(&reflection))
            return *error;
        auto packed = PackMaterialUniforms(data, shader.Code, *data.Bindings, 64 * 1024 * 1024, shader.Domain);
        if (const auto* error = std::get_if<AssetError>(&packed))
            return *error;
        auto schema = DescribeMaterial(&shader);
        if (const auto* error = std::get_if<AssetError>(&schema))
            return *error;
        MaterialSource source;
        source.Name   = data.Name;
        source.Shader = data.Shader;
        source.Render = data.Render;
        source.DisabledPasses = data.DisabledPasses;
        std::set<uint32_t> textures;
        for (const auto& resource : MaterialShaderResources(std::get<std::vector<ShaderStageReflection>>(reflection)))
        {
            if (resource.Kind == ShaderBindingKind::CombinedImageSampler)
            {
                for (const auto& assignment : data.Bindings->Textures)
                    if (assignment.Set == resource.Set && assignment.Binding == resource.Binding)
                    {
                        source.Textures[resource.Name] = data.Textures.at(assignment.TextureIndex).Texture;
                        textures.insert(assignment.TextureIndex);
                    }
                continue;
            }
            const auto& blocks = std::get<std::vector<MaterialUniformBlock>>(packed);
            const auto  block  = std::find_if(blocks.begin(), blocks.end(), [&](const auto& item) {
                return item.Set == resource.Set && item.Binding == resource.Binding;
            });
            if (block == blocks.end())
                return AssetError {AssetErrorCode::InvalidData, "Missing embedded material uniform block"};
            for (const auto& member : resource.Members)
            {
                if (member.Type == ShaderUniformType::Struct)
                    return AssetError {AssetErrorCode::NotImplemented, "Cannot extract struct material parameters"};
                auto& values = source.Parameters[member.Name];
                for (uint32_t element = 0; element < member.ArrayCount; ++element)
                    for (uint32_t column = 0; column < member.Columns; ++column)
                        for (uint32_t row = 0; row < member.Components; ++row)
                        {
                            uint64_t offset = member.Offset + element * member.ArrayStride;
                            offset += member.Columns == 1 ? row * 4 :
                                      member.RowMajor     ? row * member.MatrixStride + column * 4 :
                                                            column * member.MatrixStride + row * 4;
                            if (offset + 4 > block->Bytes.size())
                                return AssetError {AssetErrorCode::InvalidData, "Invalid material parameter layout"};
                            uint32_t bits = 0;
                            for (size_t byte = 0; byte < 4; ++byte)
                                bits |= uint32_t(std::to_integer<uint8_t>(block->Bytes[offset + byte])) << (8 * byte);
                            if (member.Type == ShaderUniformType::Float)
                            {
                                float value;
                                std::memcpy(&value, &bits, 4);
                                values.push_back(value);
                            }
                            else if (member.Type == ShaderUniformType::Signed)
                            {
                                int32_t value;
                                std::memcpy(&value, &bits, 4);
                                values.push_back(value);
                            }
                            else
                                values.push_back(bits);
                        }
            }
        }
        if (textures.size() != data.Textures.size())
            return AssetError {AssetErrorCode::NotImplemented, "Cannot extract unbound material textures"};
        return source;
    }

    std::variant<MaterialSource, AssetError> ParseMaterialSource(const std::byte* bytes, size_t size)
    {
        if (!bytes || !size || size > 1024 * 1024)
            return Invalid("invalid size");
        try
        {
            std::string text(reinterpret_cast<const char*>(bytes), size);
            if (text.find('\0') != std::string::npos)
                return Invalid("contains NUL");
            auto documents = YAML::LoadAll(text);
            if (documents.size() != 1 || !documents.front().IsMap())
                return Invalid("requires one YAML mapping");
            const auto            root = documents.front();
            std::set<std::string> fields;
            for (const auto& entry : root)
            {
                const auto field = entry.first.as<std::string>();
                if (!fields.insert(field).second || !std::set<std::string> {"magic",
                                                                            "version",
                                                                            "name",
                                                                            "shader",
                                                                            "parent",
                                                                            "parameters",
                                                                            "textures",
                                                                            "render",
                                                                            "disabled_passes"}
                                                         .count(field))
                    return Invalid("unknown or duplicate field '" + field + "'");
            }
            if (!root["magic"] || root["magic"].as<std::string>() != "KansMaterial")
                return Invalid("missing 'magic: KansMaterial'");
            if (!root["version"] || root["version"].as<uint32_t>() < 2 || root["version"].as<uint32_t>() > 4)
                return AssetError {AssetErrorCode::UnsupportedVersion, "Material source requires version 2, 3 or 4"};

            MaterialSource source;
            if (const auto disabled = root["disabled_passes"])
            {
                if (!disabled.IsSequence() || disabled.size() > 32)
                    return Invalid("invalid disabled_passes");
                for (const auto& entry : disabled)
                {
                    auto name = entry.as<std::string>();
                    if (name.empty() || name.size() > 256 ||
                        std::find(source.DisabledPasses.begin(), source.DisabledPasses.end(), name) !=
                            source.DisabledPasses.end())
                        return Invalid("invalid or duplicate disabled pass");
                    source.DisabledPasses.push_back(std::move(name));
                }
            }
            if (root["parent"])
            {
                if (root["version"].as<uint32_t>() < 4)
                    return Invalid("parent requires version 4");
                source.Parent = AssetID::FromString(root["parent"].as<std::string>());
                if (!source.Parent.IsValid())
                    return Invalid("invalid parent Material AssetID");
                if (root["shader"])
                    return Invalid("an instance cannot specify both parent and shader");
            }
            source.Render = ReadMaterialRenderState(root["render"]);
            if (source.Parent.IsValid())
            {
                const auto render = root["render"];
                if (render && render["temporal"])
                    source.RenderOverrides.TemporalResponse = source.Render.TemporalResponse;
                if (render && render["alpha_mode"])
                    source.RenderOverrides.AlphaMode = source.Render.AlphaMode;
                if (render && render["alpha_cutoff"])
                    source.RenderOverrides.AlphaCutoff = source.Render.AlphaCutoff;
                if (render && render["double_sided"])
                    source.RenderOverrides.DoubleSided = source.Render.DoubleSided;
                if (render && render["receive_shadows"])
                    source.RenderOverrides.ReceiveShadows = source.Render.ReceiveShadows;
                source.Render = {};
            }
            source.Name = root["name"] ? root["name"].as<std::string>() : std::string();
            if (source.Name.size() > 4096)
                return Invalid("name is too long");
            if (root["shader"] && !root["shader"].IsNull())
            {
                const auto shader = root["shader"].as<std::string>();
                source.Shader     = AssetID::FromString(shader);
                if (!shader.empty() && !source.Shader.IsValid())
                    return Invalid("invalid shader AssetID '" + shader + "'");
            }
            if (const auto parameters = root["parameters"]; parameters && !parameters.IsNull())
            {
                if (!parameters.IsMap() || parameters.size() > MaximumEntries)
                    return Invalid("'parameters' must be a mapping");
                for (const auto& entry : parameters)
                {
                    const auto name = entry.first.as<std::string>();
                    if (!ValidName(name) || source.Parameters.count(name))
                        return Invalid("invalid or duplicate parameter '" + name + "'");
                    Flatten(entry.second, source.Parameters[name], 0);
                }
            }
            if (const auto textures = root["textures"]; textures && !textures.IsNull())
            {
                if (!textures.IsMap() || textures.size() > MaximumEntries)
                    return Invalid("'textures' must be a mapping");
                for (const auto& entry : textures)
                {
                    const auto name = entry.first.as<std::string>();
                    const bool clear =
                        entry.second.IsNull() || (entry.second.IsScalar() && entry.second.Scalar().empty());
                    const auto id = clear ? AssetID {} : AssetID::FromString(entry.second.as<std::string>());
                    if (!ValidName(name) || (!id.IsValid() && !(clear && source.Parent.IsValid())) ||
                        !source.Textures.emplace(name, id).second)
                        return Invalid("invalid texture entry '" + name + "'");
                }
            }
            return CanonicalSource(std::move(source));
        }
        catch (const std::exception& error)
        {
            return Invalid(std::string(error.what()).substr(0, 512));
        }
    }

    std::string SerializeMaterialSource(const MaterialSource& input)
    {
        const auto    source = CanonicalSource(input);
        YAML::Emitter out;
        out.SetFloatPrecision(9);
        out << YAML::BeginMap;
        out << YAML::Key << "magic" << YAML::Value << "KansMaterial";
        out << YAML::Key << "version" << YAML::Value << 4;
        out << YAML::Key << "name" << YAML::Value << source.Name;
        if (source.Parent.IsValid())
        {
            if (source.Shader.IsValid())
                throw std::invalid_argument("an instance cannot specify both parent and shader");
            out << YAML::Key << "parent" << YAML::Value << source.Parent.ToString();
            const auto& render = source.RenderOverrides;
            if (render.AlphaMode || render.AlphaCutoff || render.DoubleSided || render.ReceiveShadows ||
                render.TemporalResponse)
            {
                MaterialRenderState state;
                if (render.TemporalResponse)
                    state.TemporalResponse = *render.TemporalResponse;
                if (render.AlphaMode)
                    state.AlphaMode = *render.AlphaMode;
                if (render.AlphaCutoff)
                    state.AlphaCutoff = *render.AlphaCutoff;
                if (!IsValidMaterialRenderState(state))
                    throw std::invalid_argument("invalid material render override");
                out << YAML::Key << "render" << YAML::Value << YAML::Flow << YAML::BeginMap;
                if (render.AlphaMode)
                    out << YAML::Key << "alpha_mode" << YAML::Value
                        << (state.AlphaMode == MaterialAlphaMode::Mask          ? "mask" :
                            state.AlphaMode == MaterialAlphaMode::Blend         ? "blend" :
                            state.AlphaMode == MaterialAlphaMode::Dithered      ? "dithered" :
                            state.AlphaMode == MaterialAlphaMode::Additive      ? "additive" :
                            state.AlphaMode == MaterialAlphaMode::Premultiplied ? "premultiplied" :
                                                                                  "opaque");
                if (render.TemporalResponse)
                    out << YAML::Key << "temporal" << YAML::Value
                        << (*render.TemporalResponse == MaterialTemporalResponse::Reactive    ? "reactive" :
                            *render.TemporalResponse == MaterialTemporalResponse::Composition ? "composition" :
                                                                                                "default");
                if (render.AlphaCutoff)
                    out << YAML::Key << "alpha_cutoff" << YAML::Value << *render.AlphaCutoff;
                if (render.ReceiveShadows)
                    out << YAML::Key << "receive_shadows" << YAML::Value << *render.ReceiveShadows;
                if (render.DoubleSided)
                    out << YAML::Key << "double_sided" << YAML::Value << *render.DoubleSided;
                out << YAML::EndMap;
            }
        }
        else
        {
            out << YAML::Key << "shader" << YAML::Value
                << (source.Shader.IsValid() ? source.Shader.ToString() : std::string());
            EmitMaterialRenderState(out, source.Render);
        }
        out << YAML::Key << "disabled_passes" << YAML::Value << YAML::Flow << YAML::BeginSeq;
        for (const auto& name : source.DisabledPasses)
            out << name;
        out << YAML::EndSeq;
        out << YAML::Key << "parameters" << YAML::Value << YAML::BeginMap;
        for (const auto& [name, values] : source.Parameters)
        {
            out << YAML::Key << name << YAML::Value;
            if (values.size() == 1)
                out << FormatNumber(values[0]);
            else
            {
                out << YAML::Flow << YAML::BeginSeq;
                for (double value : values)
                    out << FormatNumber(value);
                out << YAML::EndSeq;
            }
        }
        out << YAML::EndMap;
        out << YAML::Key << "textures" << YAML::Value << YAML::BeginMap;
        for (const auto& [name, id] : source.Textures)
        {
            out << YAML::Key << name << YAML::Value;
            if (id.IsValid())
                out << id.ToString();
            else if (source.Parent.IsValid())
                out << YAML::Null;
            else
                throw std::invalid_argument("empty texture requires a parent");
        }
        out << YAML::EndMap;
        out << YAML::EndMap;
        return std::string(out.c_str()) + "\n";
    }

    std::variant<MaterialSchema, AssetError> DescribeMaterial(const ShaderData* shader)
    {
        if (!shader)
            shader = &GetStandardShader();
        try
        {
            auto reflection = MaterialReflection(*shader);
            if (auto* error = std::get_if<AssetError>(&reflection))
                return std::move(*error);
            MaterialSchema        schema;
            std::set<std::string> names;
            for (const auto& resource :
                 MaterialShaderResources(std::get<std::vector<ShaderStageReflection>>(reflection)))
            {
                if (resource.Count != 1)
                    return Invalid("descriptor arrays are not supported in materials ('" + resource.Name + "')");
                if (resource.Kind == ShaderBindingKind::CombinedImageSampler)
                {
                    if (!names.insert(resource.Name).second)
                        return Invalid("duplicate material name '" + resource.Name + "'");
                    schema.Textures.push_back(resource.Name);
                    continue;
                }
                for (const auto& member : resource.Members)
                {
                    if (member.Type == ShaderUniformType::Struct)
                        continue;
                    if (!names.insert(member.Name).second)
                        return Invalid("duplicate material name '" + member.Name + "'");
                    schema.Parameters.push_back(
                        {member.Name, member.Type, member.Components, member.Columns, member.ArrayCount});
                }
            }
            for (const auto& [name, values] : shader->ParameterDefaults)
            {
                const auto found = std::find_if(schema.Parameters.begin(),
                                                schema.Parameters.end(),
                                                [&](const auto& parameter) { return parameter.Name == name; });
                if (found == schema.Parameters.end() || found->ValueCount() != values.size())
                    return Invalid("Shader default '" + name + "' does not match a material parameter of " +
                                   std::to_string(values.size()) + " values");
                schema.Defaults.emplace(name, values);
            }
            for (const auto& [name, hint] : shader->ParameterHints)
            {
                const auto found = std::find_if(schema.Parameters.begin(),
                                                schema.Parameters.end(),
                                                [&](const auto& parameter) { return parameter.Name == name; });
                if (found == schema.Parameters.end())
                    return Invalid("Shader hint '" + name + "' does not name a material parameter");
                if (hint.Type == ShaderParameterHint::Kind::Color &&
                    (found->Type != ShaderUniformType::Float || found->Columns != 1 || found->ArrayCount != 1 ||
                     found->Components < 3))
                    return Invalid("Shader hint '" + name + "': color needs a vec3 or vec4");
                if (hint.Type == ShaderParameterHint::Kind::Range && found->Type == ShaderUniformType::Boolean)
                    return Invalid("Shader hint '" + name + "': range does not apply to a bool");
                found->Hint = hint;
            }
            for (const auto& [name, role] : shader->TextureFallbacks)
                if (std::find(schema.Textures.begin(), schema.Textures.end(), name) == schema.Textures.end() ||
                    uint32_t(role) > uint32_t(ShaderTextureFallback::Normal))
                    return Invalid("Shader texture fallback '" + name + "' does not name a material sampler");
            auto groups = ValidateMaterialParameterGroups(shader->Groups,
                                                          std::get<std::vector<ShaderStageReflection>>(reflection));
            if (auto* error = std::get_if<AssetError>(&groups))
                return *error;
            schema.Groups = shader->Groups;
            return schema;
        }
        catch (const std::exception& error)
        {
            return Invalid(std::string(error.what()).substr(0, 512));
        }
    }

    std::variant<MaterialSource, AssetError>
    ApplyMaterialOverrides(const MaterialSource& parent, const MaterialSource& instance, const ShaderData* shader)
    {
        if (parent.Parent.IsValid() || !instance.Parent.IsValid() || instance.Shader.IsValid())
            return Invalid("overrides require an effective parent and an instance without its own Shader");
        auto described = DescribeMaterial(IsStandardShader(parent.Shader)        ? nullptr :
                                          parent.Shader == SixWaySmokeShaderID() ? &GetSixWaySmokeShader() :
                                                                                   shader);
        if (const auto* error = std::get_if<AssetError>(&described))
            return *error;
        const auto&    schema     = std::get<MaterialSchema>(described);
        MaterialSource effective  = parent;
        effective.Name            = instance.Name;
        effective.RenderOverrides = {};
        for (const auto& name : instance.DisabledPasses)
            if (std::find(effective.DisabledPasses.begin(), effective.DisabledPasses.end(), name) ==
                effective.DisabledPasses.end())
                effective.DisabledPasses.push_back(name);
        for (const auto& [name, values] : instance.Parameters)
        {
            if (std::none_of(schema.Parameters.begin(), schema.Parameters.end(), [&](const auto& parameter) {
                    return parameter.Name == name;
                }))
                return Invalid("unknown parameter override '" + name + "'");
            effective.Parameters[name] = values;
        }
        for (const auto& [name, id] : instance.Textures)
        {
            if (std::find(schema.Textures.begin(), schema.Textures.end(), name) == schema.Textures.end())
                return Invalid("unknown texture override '" + name + "'");
            if (id.IsValid())
                effective.Textures[name] = id;
            else
                effective.Textures.erase(name);
        }
        if (instance.RenderOverrides.TemporalResponse)
            effective.Render.TemporalResponse = *instance.RenderOverrides.TemporalResponse;
        if (instance.RenderOverrides.AlphaMode)
            effective.Render.AlphaMode = *instance.RenderOverrides.AlphaMode;
        if (instance.RenderOverrides.AlphaCutoff)
            effective.Render.AlphaCutoff = *instance.RenderOverrides.AlphaCutoff;
        if (instance.RenderOverrides.ReceiveShadows)
            effective.Render.ReceiveShadows = *instance.RenderOverrides.ReceiveShadows;
        if (instance.RenderOverrides.DoubleSided)
            effective.Render.DoubleSided = *instance.RenderOverrides.DoubleSided;
        auto validated = ResolveMaterialSource(effective, shader);
        if (const auto* error = std::get_if<AssetError>(&validated))
            return *error;
        return effective;
    }

    std::variant<MaterialData, AssetError> ResolveMaterialSource(const MaterialSource& input, const ShaderData* shader)
    {
        if (input.Parent.IsValid())
            return Invalid("instance must be combined with its effective parent before resolving");
        MaterialSource source;
        try
        {
            source = CanonicalSource(input);
        }
        catch (const std::exception& error)
        {
            return Invalid(error.what());
        }
        const bool standard = IsStandardShader(source.Shader);
        if (source.Shader == SixWaySmokeShaderID())
            shader = &GetSixWaySmokeShader();
        if ((!standard && !shader) || (!source.Shader.IsValid() && shader && shader != &GetStandardShader() &&
                                       shader != &GetStandardShaderAsset()->GetData()))
            return AssetError {AssetErrorCode::InvalidArgument, "Material source and Shader do not match"};
        if (standard)
            shader = &GetStandardShader();
        auto described = DescribeMaterial(shader);
        if (auto* error = std::get_if<AssetError>(&described))
            return std::move(*error);
        const auto& schema = std::get<MaterialSchema>(described);
        const auto  info   = [&](const std::string& name) -> const MaterialParameterInfo* {
            for (const auto& parameter : schema.Parameters)
                if (parameter.Name == name)
                    return &parameter;
            return nullptr;
        };
        // Parameters the source leaves unset take the schema defaults.
        auto parameters = schema.Defaults;
        for (const auto& [name, values] : source.Parameters)
            parameters[name] = values;
        for (const auto& [name, values] : parameters)
        {
            const auto* parameter = info(name);
            if (!parameter)
                return Invalid("unknown parameter '" + name + "'");
            if (values.size() != parameter->ValueCount())
                return Invalid("parameter '" + name + "' needs " + std::to_string(parameter->ValueCount()) +
                               " values, got " + std::to_string(values.size()));
            for (double value : values)
                if (!std::isfinite(value) ||
                    (parameter->Type != ShaderUniformType::Float &&
                     (value != std::floor(value) ||
                      (parameter->Type == ShaderUniformType::Signed && (value < INT32_MIN || value > INT32_MAX)) ||
                      (parameter->Type == ShaderUniformType::Unsigned && (value < 0 || value > UINT32_MAX)) ||
                      (parameter->Type == ShaderUniformType::Boolean && value != 0 && value != 1))))
                    return Invalid("parameter '" + name + "' has a value of the wrong type");
        }
        for (const auto& [name, id] : source.Textures)
            if (std::find(schema.Textures.begin(), schema.Textures.end(), name) == schema.Textures.end())
                return Invalid("unknown texture '" + name + "'");

        MaterialData data;
        data.Name = source.Name;
        data.Render = source.Render;
        data.DisabledPasses = source.DisabledPasses;
        if (!IsValidMaterialRenderState(data.Render))
            return Invalid("invalid render state");
        if (standard)
        {
            const auto& color = parameters.at("BaseColor");
            for (size_t i = 0; i < 4; ++i)
                data.BaseColor[i] = static_cast<float>(color[i]);
            data.Metallic  = static_cast<float>(parameters.at("Metallic")[0]);
            data.Roughness = static_cast<float>(parameters.at("Roughness")[0]);
        }

        try
        {
            data.Shader     = standard ? StandardShaderID() : source.Shader;
            data.Bindings  = MaterialBindingRecipe {};
            auto& recipe   = *data.Bindings;
            auto  inspected = MaterialReflection(*shader);
            if (auto* error = std::get_if<AssetError>(&inspected))
                return std::move(*error);
            for (const auto& resource :
                 MaterialShaderResources(std::get<std::vector<ShaderStageReflection>>(inspected)))
            {
                if (resource.Kind == ShaderBindingKind::CombinedImageSampler)
                {
                    const auto found = source.Textures.find(resource.Name);
                    if (found == source.Textures.end())
                        continue;
                    uint32_t semantic = uint32_t(MaterialTextureSemantic::Unknown);
                    if (standard)
                    {
                        const std::map<std::string, MaterialTextureSemantic> roles = {
                            {"BaseColorMap", MaterialTextureSemantic::BaseColor},
                            {"NormalMap", MaterialTextureSemantic::Normal},
                            {"MetallicRoughnessMap", MaterialTextureSemantic::MetallicRoughness},
                            {"OcclusionMap", MaterialTextureSemantic::AmbientOcclusion},
                            {"EmissiveMap", MaterialTextureSemantic::EmissionColor}};
                        semantic = uint32_t(roles.at(resource.Name));
                    }
                    data.Textures.push_back({semantic, 0, found->second});
                    recipe.Textures.push_back({MaterialDescriptorSet,
                                               resource.Binding,
                                               0,
                                               static_cast<uint32_t>(data.Textures.size() - 1)});
                    continue;
                }
                MaterialUniformAssignment assignment;
                assignment.Set     = MaterialDescriptorSet;
                assignment.Binding = resource.Binding;
                for (uint32_t index = 0; index < resource.Members.size(); ++index)
                {
                    const auto& member = resource.Members[index];
                    const auto  found  = parameters.find(member.Name);
                    if (member.Type == ShaderUniformType::Struct || found == parameters.end())
                        continue;
                    const size_t stride = size_t(member.Components) * member.Columns;
                    for (uint32_t element = 0; element < member.ArrayCount; ++element)
                    {
                        const auto begin = found->second.begin() + element * stride;
                        ShaderUniformWrite write;
                        write.Path = {{index, element}};
                        if (member.Type == ShaderUniformType::Float)
                            write.Values = std::vector<float>(begin, begin + stride);
                        else if (member.Type == ShaderUniformType::Signed)
                            write.Values = std::vector<int32_t>(begin, begin + stride);
                        else
                            write.Values = std::vector<uint32_t>(begin, begin + stride);
                        assignment.Values.push_back(std::move(write));
                    }
                }
                recipe.Uniforms.push_back(std::move(assignment));
            }
            auto packed = PackMaterialUniforms(data, shader->Code, recipe, 64 * 1024 * 1024, shader->Domain);
            if (auto* error = std::get_if<AssetError>(&packed))
                return std::move(*error);
            auto valid = ValidateMaterialData(data);
            if (auto* error = std::get_if<AssetError>(&valid))
                return *error;
            return data;
        }
        catch (const std::exception& error)
        {
            return Invalid(std::string(error.what()).substr(0, 512));
        }
    }
} // namespace Kans
