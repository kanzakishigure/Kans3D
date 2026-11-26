#pragma once
#include "AssetImporterBase.h"
namespace Kans
{
    class TextureImporter final : public AssetImporterBase
    {
    public:
        ImportSettingsSchema GetSettingsSchema() const override;
        std::variant<std::monostate, AssetError>
        ValidateSettings(const std::map<std::string, std::string>&) const override;
        std::variant<SourceInventory, AssetError>            Inspect(const ImportContext&) const override;
        uint32_t                                             GetVersion() const override { return 5; }
        [[nodiscard]] std::variant<ImportResult, AssetError> Import(const ImportContext& context) const override;
    };
} // namespace Kans
