#pragma once
#include "Kans3D/Asset/Importer/AssetImporterBase.h"
namespace Kans
{
    class ExpressionImporter final : public AssetImporterBase
    {
    public:
        inline static constexpr const char*       RootObjectKey = "expression:root";
        uint32_t                                  GetVersion() const override { return 1; }
        std::variant<SourceInventory, AssetError> Inspect(const ImportContext&) const override;
        std::variant<ImportResult, AssetError>    Import(const ImportContext&) const override;
    };
} // namespace Kans
