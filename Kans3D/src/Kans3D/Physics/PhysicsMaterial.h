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
    // Coefficients are clamped to [0, 1]. NaNs use the corresponding preset coefficient.
    void             SanitizeAcousticMaterial(AcousticMaterial&,
                                              std::vector<std::string>* reasons  = nullptr,
                                              const AcousticMaterial&   defaults = {});
    YAML::Node       EncodeAcousticMaterial(const AcousticMaterial&);
    AcousticMaterial DecodeAcousticMaterial(const YAML::Node&, std::vector<std::string>* reasons = nullptr);
    YAML::Node                                       EncodePhysicsMaterialNode(const PhysicsMaterialValues&);
    PhysicsMaterialValues DecodePhysicsMaterialNode(const YAML::Node&, std::vector<std::string>* reasons = nullptr);
    std::variant<std::vector<std::byte>, AssetError> EncodePhysicsMaterialProduct(const PhysicsMaterialValues&);
    std::variant<PhysicsMaterialValues, AssetError>  DecodePhysicsMaterialProduct(const std::byte*, size_t);
} // namespace Kans
