#pragma once
#include "Kans3D/Asset/Runtime/MaterialShaderInterface.h"
#include "AssetImporterBase.h"

namespace Kans
{
    // Imports a 3D model as Mesh, Material and Model Products. Textures referenced
    // by the model are separate sources; their Products are referenced by ID.
    class ModelImporter final : public AssetImporterBase
    {
    public:
        // Model material products use ResolveStandardMaterial and are packed/validated
        // against Standard shader reflection, so shader text changes must invalidate them.
        std::string          GetEnvironmentFingerprint() const override { return GetEngineShaderDigest(); }
        ImportSettingsSchema GetSettingsSchema() const override;
        std::variant<std::monostate, AssetError>
        ValidateSettings(const std::map<std::string, std::string>&) const override;
        std::variant<SourceInventory, AssetError> Inspect(const ImportContext&) const override;
        uint32_t                                  GetVersion() const override { return 22; }
        bool     RequiresProductBytes() const override { return true; }
        [[nodiscard]] std::variant<ImportInputs, AssetError>
        DiscoverInputs(const ImportContext& context) const override;
        [[nodiscard]] std::variant<ImportResult, AssetError> Import(const ImportContext& context) const override;
    };
} // namespace Kans
