#pragma once
#include "AssetImporterBase.h"

namespace Kans
{
    // Imports a 3D model as Mesh, Material and Model Products. Textures referenced
    // by the model are separate sources; their Products are referenced by ID.
    class ModelImporter final : public AssetImporterBase
    {
    public:
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
