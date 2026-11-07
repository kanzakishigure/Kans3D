#pragma once
#include "Kans3D/Asset/Importer/AssetImporterBase.h"

namespace Kans
{
    class AudioImporter final : public AssetImporterBase
    {
    public:
        ImportSettingsSchema GetSettingsSchema() const override;
        std::variant<std::monostate, AssetError>
        ValidateSettings(const std::map<std::string, std::string>&) const override;
        uint32_t                                             GetVersion() const override { return 1; }
        [[nodiscard]] std::variant<ImportResult, AssetError> Import(const ImportContext& context) const override;
    };
} // namespace Kans
