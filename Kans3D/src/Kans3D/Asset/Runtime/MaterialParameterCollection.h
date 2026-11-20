#pragma once

#include <array>
#include <optional>
#include <string_view>
#include <variant>
#include <vector>
#include <glm/glm.hpp>
#include "Kans3D/Asset/Asset.h"
#include "Kans3D/Asset/AssetError.h"

namespace Kans
{
    constexpr uint32_t MaterialParameterLimit      = 64;
    constexpr uint32_t MaterialCollectionLimit     = 8;
    constexpr uint32_t MaterialParameterSceneLimit = MaterialParameterLimit * MaterialCollectionLimit;

    enum class MaterialParameterType
    {
        Scalar,
        Vec4
    };

    struct MaterialParameterDefinition
    {
        std::string              Name;
        MaterialParameterType    Type = MaterialParameterType::Scalar;
        glm::vec4                Default {0};
        std::optional<glm::vec2> Range;
    };

    struct MaterialParameterCollection
    {
        std::string                              Name;
        std::vector<MaterialParameterDefinition> Parameters;
    };

    class MaterialParameterCollectionAsset : public Asset
    {
    public:
        explicit MaterialParameterCollectionAsset(MaterialParameterCollection data) : m_Data(std::move(data)) {}
        static AssetType                   GetStaticType() { return AssetType::MaterialParameterCollection; }
        AssetType                          GetAssetType() const override { return GetStaticType(); }
        const MaterialParameterCollection& GetData() const { return m_Data; }

    private:
        MaterialParameterCollection m_Data;
    };

    bool                      IsMaterialParameterName(std::string_view name);
    bool                      IsFiniteMaterialParameter(glm::vec4 value);
    std::optional<AssetError> ValidateMaterialParameterCollection(const MaterialParameterCollection& collection);
    std::variant<MaterialParameterCollection, AssetError> ParseMaterialParameterCollection(const std::string& text);
    std::variant<std::string, AssetError> SerializeMaterialParameterCollection(const MaterialParameterCollection&);
    uint64_t    MaterialParameterKey(std::string_view collection, const MaterialParameterDefinition& parameter);
    std::string MakeMaterialParameterInclude(const MaterialParameterCollection&, const std::string& sourceName);

    // Packed std140 data. Keys store two 64-bit identities per uvec4, low word first.
    struct MaterialParameterUniforms
    {
        MaterialParameterUniforms()
        {
            Keys.fill(glm::uvec4(0));
            Values.fill(glm::vec4(0));
        }

        glm::uvec4                                              Count {0};
        std::array<glm::uvec4, MaterialParameterSceneLimit / 2> Keys {};
        std::array<glm::vec4, MaterialParameterSceneLimit>      Values {};
    };
    static_assert(sizeof(MaterialParameterUniforms) == 12304);
} // namespace Kans
