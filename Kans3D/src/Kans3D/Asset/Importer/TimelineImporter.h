#pragma once
#include "AssetImporterBase.h"
namespace Kans
{
    class TimelineImporter final : public AssetImporterBase
    {
    public:
        inline static constexpr const char*       RootObjectKey = "timeline:root";
        bool                                      RequiresProductBytes() const override { return true; }
        std::variant<ImportInputs, AssetError>    DiscoverInputs(const ImportContext&) const override;
        uint32_t                                  GetVersion() const override { return 2; }
        std::variant<SourceInventory, AssetError> Inspect(const ImportContext&) const override;
        std::variant<ImportResult, AssetError>    Import(const ImportContext&) const override;
    };
} // namespace Kans
