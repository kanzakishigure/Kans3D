#pragma once
#include "AssetImporterBase.h"
namespace Kans
{
    class RenderTextureImporter final : public AssetImporterBase
    {
    public:
        static constexpr const char*           RootObjectKey = "RenderTexture";
        uint32_t                               GetVersion() const override { return 1; }
        bool                                   RequiresProductBytes() const override { return true; }
        std::variant<ImportInputs, AssetError> DiscoverInputs(const ImportContext&) const override;
        std::variant<ImportResult, AssetError> Import(const ImportContext&) const override;
    };
} // namespace Kans
