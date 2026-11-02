#pragma once
#include <cstddef>
#include <variant>
#include "Kans3D/Animation/AnimationProduct.h"
#include "Kans3D/Asset/Asset.h"
#include "Kans3D/Asset/AssetError.h"
#include "Kans3D/Timeline/TimelineProduct.h"

namespace Kans
{
    class AssetLoadContext
    {
    public:
        AssetID   ID;
        AssetType Type = AssetType::None;
        // Borrowed Product bytes; caller owns storage for the duration of Load().
        const std::byte* Data = nullptr;
        size_t           Size = 0;
    };

    template<typename T>
    class Loader;
    class VectorFieldAsset;
    template<>
    class Loader<VectorFieldAsset>
    {
    public:
        std::variant<Ref<VectorFieldAsset>, AssetError> Load(const AssetLoadContext&) const;
    };
    class VfxSystemAsset;
    template<>
    class Loader<VfxSystemAsset>
    {
    public:
        std::variant<Ref<VfxSystemAsset>, AssetError> Load(const AssetLoadContext&) const;
    };
    class RagdollAsset;
    template<>
    class Loader<RagdollAsset>
    {
    public:
        std::variant<Ref<RagdollAsset>, AssetError> Load(const AssetLoadContext&) const;
    };
    class ClothAsset;
    template<>
    class Loader<ClothAsset>
    {
    public:
        std::variant<Ref<ClothAsset>, AssetError> Load(const AssetLoadContext&) const;
    };
    class PhysicsMaterialAsset;
    template<>
    class Loader<PhysicsMaterialAsset>
    {
    public:
        std::variant<Ref<PhysicsMaterialAsset>, AssetError> Load(const AssetLoadContext&) const;
    };
    class MeshAsset;
    class ModelAsset;
    class MaterialDataAsset;
    class TextureAsset;
    class ShaderDataAsset;
    class UIAsset;
    template<>
    class Loader<UIAsset>
    {
    public:
        std::variant<Ref<UIAsset>, AssetError> Load(const AssetLoadContext&) const;
    };
    class FontAsset;
    template<>
    class Loader<FontAsset>
    {
    public:
        [[nodiscard]] std::variant<Ref<FontAsset>, AssetError> Load(const AssetLoadContext&) const;
    };
    class AudioClipAsset;
    template<>
    class Loader<AudioClipAsset>
    {
    public:
        [[nodiscard]] std::variant<Ref<AudioClipAsset>, AssetError> Load(const AssetLoadContext& context) const;
    };
    template<>
    class Loader<ShaderDataAsset>
    {
    public:
        std::variant<Ref<ShaderDataAsset>, AssetError> Load(const AssetLoadContext& context) const;
    };
    template<>
    class Loader<TextureAsset>
    {
    public:
        [[nodiscard]] std::variant<Ref<TextureAsset>, AssetError> Load(const AssetLoadContext& context) const;
    };
    template<>
    class Loader<ModelAsset>
    {
    public:
        [[nodiscard]] std::variant<Ref<ModelAsset>, AssetError> Load(const AssetLoadContext& context) const;
    };
    template<>
    class Loader<MaterialDataAsset>
    {
    public:
        [[nodiscard]] std::variant<Ref<MaterialDataAsset>, AssetError> Load(const AssetLoadContext& context) const;
    };
    template<>
    class Loader<MeshAsset>
    {
    public:
        // CPU-only Product decoding. No source paths, file IO, importer or GPU calls.
        [[nodiscard]] std::variant<Ref<MeshAsset>, AssetError> Load(const AssetLoadContext& context) const;
    };
    template<class T, AssetType Type>
    class Loader<AnimationDataAsset<T, Type>>
    {
    public:
        std::variant<Ref<AnimationDataAsset<T, Type>>, AssetError> Load(const AssetLoadContext& context) const
        {
            if (!context.ID.IsValid() || context.Type != Type)
                return AssetError {AssetErrorCode::InvalidArgument, "Invalid animation Product input"};
            auto decoded = [&] {
                if constexpr (Type == AssetType::Skeleton)
                    return DecodeSkeletonProduct(context.Data, context.Size);
                else if constexpr (Type == AssetType::AnimationClip)
                    return DecodeAnimationClipProduct(context.Data, context.Size);
                else if constexpr (Type == AssetType::Timeline)
                    return DecodeTimelineProduct(context.Data, context.Size);
                else if constexpr (Type == AssetType::Action)
                    return DecodeActionProduct(context.Data, context.Size);
                else if constexpr (Type == AssetType::Expression)
                    return DecodeExpressionProduct(context.Data, context.Size);
                else
                    return DecodeAnimationControllerProduct(context.Data, context.Size);
            }();
            if (auto* error = std::get_if<AssetError>(&decoded))
                return *error;
            auto asset     = CreateRef<AnimationDataAsset<T, Type>>(std::get<T>(std::move(decoded)));
            asset->assetID = context.ID;
            return asset;
        }
    };
} // namespace Kans
