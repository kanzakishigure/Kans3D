#pragma once
#include <variant>
#include <vector>
#include <yaml-cpp/yaml.h>
#include "Kans3D/Asset/Asset.h"
#include "Kans3D/Asset/AssetError.h"
#include "PhysicsSettings.h"
namespace Kans
{
    class PhysicsMaterialAsset final : public Asset
    {
    public:
        explicit PhysicsMaterialAsset(PhysicsMaterialValues values = {}) : Values(values) {}
        static AssetType             GetStaticType() { return AssetType::PhysicsMaterial; }
        AssetType                    GetAssetType() const override { return GetStaticType(); }
        const PhysicsMaterialValues& GetData() const { return Values; }

    private:
        PhysicsMaterialValues Values;
    };
    std::variant<std::monostate, AssetError>         ValidatePhysicsMaterial(const PhysicsMaterialValues&);
    YAML::Node                                       EncodePhysicsMaterialNode(const PhysicsMaterialValues&);
    PhysicsMaterialValues                            DecodePhysicsMaterialNode(const YAML::Node&);
    std::variant<std::vector<std::byte>, AssetError> EncodePhysicsMaterialProduct(const PhysicsMaterialValues&);
    std::variant<PhysicsMaterialValues, AssetError>  DecodePhysicsMaterialProduct(const std::byte*, size_t);
} // namespace Kans
