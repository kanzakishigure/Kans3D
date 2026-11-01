#include "kspch.h"
#include "VfxSystemAsset.h"
#include "Kans3D/Asset/Loader/Loader.h"
namespace Kans
{
    std::variant<Ref<VfxSystemAsset>, AssetError> Loader<VfxSystemAsset>::Load(const AssetLoadContext& c) const
    {
        if (!c.ID.IsValid() || c.Type != AssetType::VfxSystem || !c.Data || !c.Size)
            return AssetError {AssetErrorCode::InvalidArgument, "Invalid VFX product input"};
        try
        {
            auto data      = Vfx::DecodeProduct(YAML::Load(std::string(reinterpret_cast<const char*>(c.Data), c.Size)));
            auto asset     = CreateRef<VfxSystemAsset>(std::move(data));
            asset->assetID = c.ID;
            return asset;
        }
        catch (const std::exception& e)
        {
            return AssetError {AssetErrorCode::InvalidData, e.what()};
        }
    }
} // namespace Kans
