#pragma once
#include "Kans3D/Asset/Importer/AssetImporterBase.h"
#include "Kans3D/Asset/Runtime/MaterialShaderInterface.h"
namespace Kans
{
    class VfxImporter final : public AssetImporterBase
    {
    public:
        std::string GetEnvironmentFingerprint() const override { return GetEngineShaderDigest(); }
        uint32_t                                  GetVersion() const override { return 5; }
        bool                                      RequiresProductBytes() const override { return true; }
        std::variant<ImportInputs, AssetError>    DiscoverInputs(const ImportContext&) const override;
        std::variant<SourceInventory, AssetError> Inspect(const ImportContext&) const override;
        std::variant<ImportResult, AssetError>    Import(const ImportContext&) const override;
    };
} // namespace Kans
