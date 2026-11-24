#pragma once
#include "AssetImporterBase.h"

namespace Kans
{
    class VideoImporter final : public AssetImporterBase
    {
    public:
        uint32_t                               GetVersion() const override { return 1; }
        std::variant<ImportResult, AssetError> Import(const ImportContext&) const override;
    };
} // namespace Kans
