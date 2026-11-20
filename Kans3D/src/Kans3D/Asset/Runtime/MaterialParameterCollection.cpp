#include "kspch.h"
#include "MaterialParameterCollection.h"
#include <algorithm>
#include <cmath>
#include <set>
#include <yaml-cpp/yaml.h>
#include "MaterialShaderInterface.h"

namespace Kans
{
    bool IsMaterialParameterName(std::string_view name)
    {
        const auto letter = [](char c) { return (c >= 'A' && c <= 'Z') || (c >= 'a' && c <= 'z') || c == '_'; };
        return !name.empty() && name.size() <= 64 && letter(name.front()) &&
               std::all_of(name.begin(), name.end(), [&](char c) { return letter(c) || (c >= '0' && c <= '9'); });
    }

    bool IsFiniteMaterialParameter(glm::vec4 value)
    {
        return std::isfinite(value.x) && std::isfinite(value.y) && std::isfinite(value.z) && std::isfinite(value.w);
    }

    std::optional<AssetError> ValidateMaterialParameterCollection(const MaterialParameterCollection& collection)
    {
        if (!IsMaterialParameterName(collection.Name) || collection.Parameters.size() > MaterialParameterLimit)
            return AssetError {AssetErrorCode::InvalidData, "Invalid collection name or too many material parameters"};
        std::set<std::string_view> names;
        std::set<uint64_t>         keys;
        for (const auto& p : collection.Parameters)
        {
            if (!IsMaterialParameterName(p.Name) || !names.insert(p.Name).second ||
                (p.Type != MaterialParameterType::Scalar && p.Type != MaterialParameterType::Vec4) ||
                !keys.insert(MaterialParameterKey(collection.Name, p)).second ||
                !IsFiniteMaterialParameter(p.Default) ||
                (p.Type == MaterialParameterType::Scalar &&
                 glm::vec3(p.Default.y, p.Default.z, p.Default.w) != glm::vec3(0)) ||
                (p.Range && (!std::isfinite(p.Range->x) || !std::isfinite(p.Range->y) || p.Range->x > p.Range->y)))
                return AssetError {AssetErrorCode::InvalidData, "Invalid or duplicate material parameter: " + p.Name};
        }
        return std::nullopt;
    }

    std::variant<MaterialParameterCollection, AssetError> ParseMaterialParameterCollection(const std::string& text)
    {
        try
        {
            const auto root = YAML::Load(text);
            if (!root.IsMap() || !root["MaterialParameters"] || root["MaterialParameters"].as<int>() != 1)
                return AssetError {AssetErrorCode::UnsupportedVersion, "Expected MaterialParameters version 1"};
            MaterialParameterCollection collection;
            collection.Name = root["Name"].as<std::string>();
            if (const auto parameters = root["Parameters"])
            {
                if (!parameters.IsSequence() || parameters.size() > MaterialParameterLimit)
                    throw std::runtime_error("Parameters must be a sequence of at most 64 entries");
                for (const auto& node : parameters)
                {
                    MaterialParameterDefinition p;
                    p.Name          = node["Name"].as<std::string>();
                    const auto type = node["Type"].as<std::string>();
                    if (type == "Vec4")
                        p.Type = MaterialParameterType::Vec4;
                    else if (type != "Scalar")
                        throw std::runtime_error("Unknown material parameter type: " + type);
                    if (const auto value = node["Default"])
                    {
                        if (p.Type == MaterialParameterType::Scalar)
                            p.Default.x = value.as<float>();
                        else
                        {
                            if (!value.IsSequence() || value.size() != 4)
                                throw std::runtime_error("Vec4 defaults require four numbers");
                            for (int i = 0; i < 4; ++i)
                                p.Default[i] = value[i].as<float>();
                        }
                    }
                    if (const auto range = node["Range"])
                    {
                        if (!range.IsSequence() || range.size() != 2)
                            throw std::runtime_error("Range requires a minimum and maximum");
                        p.Range = glm::vec2(range[0].as<float>(), range[1].as<float>());
                    }
                    collection.Parameters.push_back(std::move(p));
                }
            }
            if (auto error = ValidateMaterialParameterCollection(collection))
                return *error;
            return collection;
        }
        catch (const std::exception& e)
        {
            return AssetError {AssetErrorCode::InvalidData, e.what()};
        }
    }

    std::variant<std::string, AssetError>
    SerializeMaterialParameterCollection(const MaterialParameterCollection& collection)
    {
        if (auto error = ValidateMaterialParameterCollection(collection))
            return *error;
        YAML::Node root;
        root["MaterialParameters"] = 1;
        root["Name"]               = collection.Name;
        YAML::Node parameters(YAML::NodeType::Sequence);
        for (const auto& p : collection.Parameters)
        {
            YAML::Node node;
            node["Name"] = p.Name;
            node["Type"] = p.Type == MaterialParameterType::Scalar ? "Scalar" : "Vec4";
            if (p.Type == MaterialParameterType::Scalar)
                node["Default"] = p.Default.x;
            else
            {
                for (int i = 0; i < 4; ++i)
                    node["Default"].push_back(p.Default[i]);
                node["Default"].SetStyle(YAML::EmitterStyle::Flow);
            }
            if (p.Range)
            {
                node["Range"].push_back(p.Range->x);
                node["Range"].push_back(p.Range->y);
                node["Range"].SetStyle(YAML::EmitterStyle::Flow);
            }
            parameters.push_back(node);
        }
        root["Parameters"] = parameters;
        return YAML::Dump(root);
    }

    uint64_t MaterialParameterKey(std::string_view collection, const MaterialParameterDefinition& parameter)
    {
        // Stable across builds, hosts and scene collection order. Including the type isolates stale shader accessors.
        uint64_t   hash = 14695981039346656037ull;
        const auto add  = [&](unsigned char byte) { hash = (hash ^ byte) * 1099511628211ull; };
        for (char c : collection)
            add(c);
        add('.');
        for (char c : parameter.Name)
            add(c);
        add(static_cast<unsigned char>(parameter.Type));
        return hash;
    }

    std::string MakeMaterialParameterInclude(const MaterialParameterCollection& collection,
                                             const std::string&                 sourceName)
    {
        const auto  guard  = "KANS_MPC_SOURCE_" + std::to_string(Hash::Generate64MD5Hash(sourceName));
        std::string result = "#ifndef " + guard + "\n#define " + guard + "\n";
        result += "#ifndef KANS_MPC_BUFFER\n#define KANS_MPC_BUFFER\nlayout(std140,set=0,binding=" +
                  std::to_string(uint32_t(EngineBinding::MaterialParameters)) +
                  ") uniform KansMaterialParameters {\n"
                  "    uvec4 Count;\n    uvec4 Keys[" +
                  std::to_string(MaterialParameterSceneLimit / 2) + "];\n    vec4 Values[" +
                  std::to_string(MaterialParameterSceneLimit) + "];\n} u_KansMPC;\n";
        result += R"glsl(
vec4 KansMPCRead(uvec2 key)
{
    uint first = 0u, end = u_KansMPC.Count.x;
    while (first < end)
    {
        uint middle = (first + end) / 2u;
        uvec4 pair = u_KansMPC.Keys[middle / 2u];
        uvec2 found = (middle & 1u) == 0u ? pair.xy : pair.zw;
        if (found == key)
            return u_KansMPC.Values[middle];
        if (found.y < key.y || (found.y == key.y && found.x < key.x))
            first = middle + 1u;
        else
            end = middle;
    }
    return vec4(0.0);
}
#endif
)glsl";
        for (const auto& p : collection.Parameters)
        {
            const auto key    = MaterialParameterKey(collection.Name, p);
            const bool scalar = p.Type == MaterialParameterType::Scalar;
            result += std::string(scalar ? "float " : "vec4 ") + "KansMPC_" + collection.Name + "_" + p.Name +
                      "() { return KansMPCRead(uvec2(" + std::to_string(uint32_t(key)) + "u," +
                      std::to_string(uint32_t(key >> 32)) + "u))" + (scalar ? ".x" : "") + "; }\n";
        }
        return result + "#endif\n";
    }
} // namespace Kans
