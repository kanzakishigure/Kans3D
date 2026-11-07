#pragma once
#include "Kans3D/Asset/Importer/AssetImporterBase.h"
#include "Kans3D/Text/FontAsset.h"
namespace Kans
{
    std::variant<FontSettings, AssetError> ParseFontImportSettings(const std::map<std::string, std::string>&);
    class FontImporter final : public AssetImporterBase
    {
    public:
        uint32_t             GetVersion() const override { return 1; }
        ImportSettingsSchema GetSettingsSchema() const override;
        std::variant<std::monostate, AssetError>
        ValidateSettings(const std::map<std::string, std::string>&) const override;
        std::variant<SourceInventory, AssetError> Inspect(const ImportContext&) const override;
        std::variant<ImportInputs, AssetError>    DiscoverInputs(const ImportContext&) const override;
        std::variant<ImportResult, AssetError>    Import(const ImportContext&) const override;
    };
} // namespace Kans
