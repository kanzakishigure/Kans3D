#pragma once
#include "Kans3D/Asset/Importer/AssetImporterBase.h"
namespace Kans
{
    class ClothImporter final : public AssetImporterBase
    {
    public:
        inline static constexpr const char*       RootObjectKey = "cloth:root";
        bool                                      RequiresProductBytes() const override { return false; }
        std::variant<ImportInputs, AssetError>    DiscoverInputs(const ImportContext&) const override;
        uint32_t                                  GetVersion() const override { return 1; }
        std::variant<SourceInventory, AssetError> Inspect(const ImportContext&) const override;
        std::variant<ImportResult, AssetError>    Import(const ImportContext&) const override;
    };
} // namespace Kans
