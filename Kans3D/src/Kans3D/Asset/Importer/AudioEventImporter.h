#pragma once
#include "AssetImporterBase.h"

namespace Kans
{
    class AudioEventImporter final : public AssetImporterBase
    {
    public:
        static constexpr const char*           RootObjectKey = "audio-event:root";
        uint32_t                               GetVersion() const override { return 1; }
        std::variant<ImportInputs, AssetError> DiscoverInputs(const ImportContext&) const override;
        std::variant<ImportResult, AssetError> Import(const ImportContext&) const override;
    };
} // namespace Kans
