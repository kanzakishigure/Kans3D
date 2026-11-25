#include "kspch.h"
#include "AudioAttenuationImporter.h"
#include "Kans3D/Asset/Importer/LocalIDMap.h"
#include "Kans3D/Asset/Loader/Loader.h"
#include "Kans3D/Asset/Storage/ProductFiles.h"
#include "Kans3D/Audio/AudioAttenuation.h"

namespace Kans
{
    std::variant<ImportResult, AssetError> AudioAttenuationImporter::Import(const ImportContext& context) const
    {
        std::error_code ec;
        const auto      status = std::filesystem::symlink_status(context.StagingDirectory, ec);
        if (!context.SourceID.IsValid() || !context.SourceFile.is_absolute() ||
            !context.StagingDirectory.is_absolute() || !context.Settings.empty() || !context.SourceInputs.empty() ||
            ec || !std::filesystem::is_directory(status) || !std::filesystem::is_empty(context.StagingDirectory, ec) ||
            ec)
            return AssetError {AssetErrorCode::InvalidArgument, "Invalid audio attenuation import context"};

        auto read = ReadProductFile(context.SourceFile, 1024 * 1024);
        if (auto* error = std::get_if<AssetError>(&read))
            return *error;
        const auto& bytes = std::get<std::vector<std::byte>>(read);
        if (bytes.empty())
            return AssetError {AssetErrorCode::InvalidData, "Audio attenuation file is empty"};
        const auto data = ParseAudioAttenuation(std::string(reinterpret_cast<const char*>(bytes.data()), bytes.size()));
        if (const auto* error = std::get_if<AssetError>(&data))
            return *error;
        auto ids = AllocateLocalIDs(context.PreviousLocalIDs, {RootObjectKey});
        if (auto* error = std::get_if<AssetError>(&ids))
            return *error;
        ImportResult result;
        result.LocalIDs    = std::get<LocalIDMap>(std::move(ids));
        const auto local   = result.LocalIDs.at(RootObjectKey);
        const auto file    = std::to_string(local) + ".kaudioattenuation";
        auto       written = WriteProductFile(context.StagingDirectory / file, bytes);
        if (auto* error = std::get_if<AssetError>(&written))
            return *error;
        result.Products.push_back({local, AssetType::AudioAttenuation, file, {}});
        return result;
    }

    std::variant<Ref<AudioAttenuationAsset>, AssetError>
    Loader<AudioAttenuationAsset>::Load(const AssetLoadContext& context) const
    {
        if (!context.ID.IsValid() || context.Type != AssetType::AudioAttenuation || !context.Data || !context.Size ||
            context.Size > 1024 * 1024)
            return AssetError {AssetErrorCode::InvalidArgument, "Invalid audio attenuation product"};
        auto decoded = ParseAudioAttenuation(std::string(reinterpret_cast<const char*>(context.Data), context.Size));
        if (const auto* error = std::get_if<AssetError>(&decoded))
            return *error;
        auto asset     = CreateRef<AudioAttenuationAsset>(std::get<AudioAttenuationData>(std::move(decoded)));
        asset->assetID = context.ID;
        return asset;
    }
} // namespace Kans
