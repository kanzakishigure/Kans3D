#include "kspch.h"
#include "Kans3D/Asset/Product/ShaderProduct.h"
#include "Loader.h"
namespace Kans
{
    std::variant<Ref<ShaderDataAsset>, AssetError> Loader<ShaderDataAsset>::Load(const AssetLoadContext& context) const
    {
        if (!context.ID.IsValid() || context.Type != AssetType::Shader)
            return AssetError {AssetErrorCode::InvalidArgument, "Invalid Shader Product input"};
        auto decoded = DecodeShaderProduct(context.Data, context.Size);
        if (auto* error = std::get_if<AssetError>(&decoded))
            return *error;
        std::get<ShaderData>(decoded).DebugName = "ShaderProduct/" + context.ID.ToString();
        auto asset     = CreateRef<ShaderDataAsset>(std::get<ShaderData>(std::move(decoded)));
        asset->assetID = context.ID;
        return asset;
    }
} // namespace Kans
