#include "kspch.h"
#include "AssetLoaderRegistry.h"
#include "Kans3D/Asset/Runtime/AudioClipData.h"
#include "Kans3D/Asset/Runtime/MeshData.h"
#include "Kans3D/Asset/Runtime/ModelData.h"
#include "Kans3D/Asset/Runtime/ShaderData.h"
#include "Kans3D/Asset/Runtime/TextureData.h"
#include "Kans3D/Physics/ClothAsset.h"
#include "Kans3D/Physics/PhysicsMaterial.h"
#include "Kans3D/Physics/RagdollAsset.h"
#include "Kans3D/Text/FontAsset.h"
#include "Kans3D/UI/UIAsset.h"
#include "Kans3D/VFX/VfxSystemAsset.h"

namespace Kans
{
    std::variant<std::monostate, AssetError> AssetLoaderRegistry::Register(AssetType type, AssetDecodeFn decoder)
    {
        if (type == AssetType::None || !decoder)
            return AssetError {AssetErrorCode::InvalidArgument, "Invalid runtime asset decoder"};
        if (!m_Decoders.emplace(type, std::move(decoder)).second)
            return AssetError {AssetErrorCode::DuplicateID, "Runtime decoder already registered for asset type"};
        return std::monostate {};
    }

    const AssetDecodeFn* AssetLoaderRegistry::Find(AssetType type) const
    {
        const auto found = m_Decoders.find(type);
        return found == m_Decoders.end() ? nullptr : &found->second;
    }

    namespace
    {
        template<typename T>
        std::variant<Ref<Asset>, AssetError> Decode(const AssetLoadContext& context)
        {
            auto result = Loader<T>().Load(context);
            if (const auto* error = std::get_if<AssetError>(&result))
                return *error;
            return std::static_pointer_cast<Asset>(std::get<Ref<T>>(std::move(result)));
        }
    } // namespace

    AssetLoaderRegistry AssetLoaderRegistry::CreateDefault()
    {
        AssetLoaderRegistry registry;
        registry.m_Decoders.emplace(VfxSystemAsset::GetStaticType(), Decode<VfxSystemAsset>);
        registry.m_Decoders.emplace(MeshAsset::GetStaticType(), Decode<MeshAsset>);
        registry.m_Decoders.emplace(MaterialDataAsset::GetStaticType(), Decode<MaterialDataAsset>);
        registry.m_Decoders.emplace(ModelAsset::GetStaticType(), Decode<ModelAsset>);
        registry.m_Decoders.emplace(TextureAsset::GetStaticType(), Decode<TextureAsset>);
        registry.m_Decoders.emplace(ShaderDataAsset::GetStaticType(), Decode<ShaderDataAsset>);
        registry.m_Decoders.emplace(SkeletonAsset::GetStaticType(), Decode<SkeletonAsset>);
        registry.m_Decoders.emplace(AnimationClipAsset::GetStaticType(), Decode<AnimationClipAsset>);
        registry.m_Decoders.emplace(AnimationControllerAsset::GetStaticType(), Decode<AnimationControllerAsset>);
        registry.m_Decoders.emplace(ActionAsset::GetStaticType(), Decode<ActionAsset>);
        registry.m_Decoders.emplace(TimelineAsset::GetStaticType(), Decode<TimelineAsset>);
        registry.m_Decoders.emplace(ExpressionAsset::GetStaticType(), Decode<ExpressionAsset>);
        registry.m_Decoders.emplace(FontAsset::GetStaticType(), Decode<FontAsset>);
        registry.m_Decoders.emplace(AudioClipAsset::GetStaticType(), Decode<AudioClipAsset>);
        registry.m_Decoders.emplace(UIAsset::GetStaticType(), Decode<UIAsset>);
        registry.m_Decoders.emplace(RagdollAsset::GetStaticType(), Decode<RagdollAsset>);
        registry.m_Decoders.emplace(ClothAsset::GetStaticType(), Decode<ClothAsset>);
        registry.m_Decoders.emplace(PhysicsMaterialAsset::GetStaticType(), Decode<PhysicsMaterialAsset>);
        return registry;
    }
} // namespace Kans
