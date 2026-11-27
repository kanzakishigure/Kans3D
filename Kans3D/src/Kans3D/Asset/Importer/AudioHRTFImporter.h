#pragma once

#include "AssetImporterBase.h"

namespace Kans
{
    class AudioHRTFImporter final : public AssetImporterBase
    {
    public:
        static constexpr const char* RootObjectKey = "audio-hrtf:root";
        uint32_t                     GetVersion() const override { return 1; }
        ImportSettingsSchema         GetSettingsSchema() const override;
        std::variant<std::monostate, AssetError>
        ValidateSettings(const std::map<std::string, std::string>& settings) const override;
        std::variant<ImportResult, AssetError> Import(const ImportContext&) const override;
    };
} // namespace Kans
