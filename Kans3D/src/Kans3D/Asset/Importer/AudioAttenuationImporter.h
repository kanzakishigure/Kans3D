#pragma once
#include "AssetImporterBase.h"

namespace Kans
{
    class AudioAttenuationImporter final : public AssetImporterBase
    {
    public:
        static constexpr const char*           RootObjectKey = "audio-attenuation:root";
        uint32_t                               GetVersion() const override { return 1; }
        std::variant<ImportResult, AssetError> Import(const ImportContext&) const override;
    };
} // namespace Kans
