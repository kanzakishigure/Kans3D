#pragma once
#include "Kans3D/Asset/Importer/AssetImporterBase.h"
namespace Kans
{
    class UIImporter final : public AssetImporterBase
    {
    public:
        inline static constexpr const char*       RootObjectKey = "ui:root";
        uint32_t                                  GetVersion() const override { return 1; }
        bool                                      RequiresProductBytes() const override { return true; }
        std::variant<SourceInventory, AssetError> Inspect(const ImportContext&) const override;
        std::variant<ImportInputs, AssetError>    DiscoverInputs(const ImportContext&) const override;
        std::variant<ImportResult, AssetError>    Import(const ImportContext&) const override;
    };
} // namespace Kans
