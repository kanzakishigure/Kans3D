#pragma once
#include "AssetImporterBase.h"

namespace Kans
{
    class MaterialParameterCollectionImporter final : public AssetImporterBase
    {
    public:
        static constexpr const char*           RootObjectKey = "material-parameters:root";
        uint32_t                               GetVersion() const override { return 1; }
        std::variant<ImportResult, AssetError> Import(const ImportContext&) const override;
    };
} // namespace Kans
