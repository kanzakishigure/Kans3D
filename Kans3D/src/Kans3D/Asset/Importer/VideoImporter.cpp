#include "kspch.h"
#include "VideoImporter.h"
#include "Kans3D/Asset/Loader/Loader.h"
#include "Kans3D/Asset/Storage/ProductFiles.h"
#include "Kans3D/Video/VideoClip.h"
#include "LocalIDMap.h"

namespace Kans
{
    std::variant<ImportResult, AssetError> VideoImporter::Import(const ImportContext& context) const
    {
        if (!IsVideoDecodingAvailable())
            return AssetError {AssetErrorCode::NotImplemented, VideoDecodingUnavailable};
        std::error_code ec;
        const auto      status = std::filesystem::symlink_status(context.StagingDirectory, ec);
        if (!context.SourceID.IsValid() || !context.SourceFile.is_absolute() ||
            !context.StagingDirectory.is_absolute() || !context.Settings.empty() || !context.SourceInputs.empty() ||
            ec || !std::filesystem::is_directory(status) || !std::filesystem::is_empty(context.StagingDirectory, ec) ||
            ec)
            return AssetError {AssetErrorCode::InvalidArgument, "Invalid VideoClip import context"};

        auto bytes = ReadProductFile(context.SourceFile, VideoProductMaximumBytes);
        if (auto* error = std::get_if<AssetError>(&bytes))
            return *error;
        auto info = ProbeVideo(std::get<VideoBytes>(bytes));
        if (auto* error = std::get_if<AssetError>(&info))
            return *error;
        VideoClipData clip {std::get<VideoClipInfo>(std::move(info)),
                            std::make_shared<const VideoBytes>(std::get<VideoBytes>(std::move(bytes)))};
        auto          product = EncodeVideoProduct(clip);
        if (auto* error = std::get_if<AssetError>(&product))
            return *error;
        constexpr auto root = "video:root";
        auto           ids  = AllocateLocalIDs(context.PreviousLocalIDs, {root});
        if (auto* error = std::get_if<AssetError>(&ids))
            return *error;
        ImportResult result;
        result.LocalIDs    = std::get<LocalIDMap>(std::move(ids));
        const auto local   = result.LocalIDs.at(root);
        const auto file    = std::to_string(local) + ".kvid";
        auto       written = WriteProductFile(context.StagingDirectory / file, std::get<VideoBytes>(product));
        if (auto* error = std::get_if<AssetError>(&written))
            return *error;
        result.Products.push_back({local, AssetType::VideoClip, file, {}});
        return result;
    }

    std::variant<Ref<VideoClipAsset>, AssetError> Loader<VideoClipAsset>::Load(const AssetLoadContext& context) const
    {
        if (!IsVideoDecodingAvailable())
            return AssetError {AssetErrorCode::NotImplemented, VideoDecodingUnavailable};
        if (!context.ID.IsValid() || context.Type != AssetType::VideoClip)
            return AssetError {AssetErrorCode::InvalidArgument, "Invalid VideoClip product input"};
        auto decoded = DecodeVideoProduct(context.Data, context.Size);
        if (auto* error = std::get_if<AssetError>(&decoded))
            return *error;
        auto asset     = CreateRef<VideoClipAsset>(std::get<VideoClipData>(std::move(decoded)));
        asset->assetID = context.ID;
        return asset;
    }
} // namespace Kans
