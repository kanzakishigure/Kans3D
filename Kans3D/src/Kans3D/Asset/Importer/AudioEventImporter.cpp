#include "kspch.h"
#include "AudioEventImporter.h"
#include "Kans3D/Asset/Importer/LocalIDMap.h"
#include "Kans3D/Asset/Loader/Loader.h"
#include "Kans3D/Asset/Storage/ProductFiles.h"
#include "Kans3D/Audio/AudioEvent.h"

namespace Kans
{
    namespace
    {
        std::variant<AudioEventData, AssetError> Decode(const std::vector<std::byte>& bytes)
        {
            if (bytes.empty())
                return AssetError {AssetErrorCode::InvalidData, "Audio event file is empty"};
            return ParseAudioEvent(std::string(reinterpret_cast<const char*>(bytes.data()), bytes.size()));
        }

        ImportInputs ClipInputs(const AudioEventData& data)
        {
            ImportInputs inputs;
            for (const auto& entry : data.Entries)
                inputs.ProductsWithDependencies.emplace(entry.Clip, AssetType::AudioClip);
            return inputs;
        }
    } // namespace

    std::variant<ImportInputs, AssetError> AudioEventImporter::DiscoverInputs(const ImportContext& context) const
    {
        auto read = ReadProductFile(context.SourceFile, 1024 * 1024);
        if (auto* error = std::get_if<AssetError>(&read))
            return *error;
        auto data = Decode(std::get<std::vector<std::byte>>(read));
        if (auto* error = std::get_if<AssetError>(&data))
            return *error;
        return ClipInputs(std::get<AudioEventData>(data));
    }

    std::variant<ImportResult, AssetError> AudioEventImporter::Import(const ImportContext& context) const
    {
        std::error_code ec;
        const auto      status = std::filesystem::symlink_status(context.StagingDirectory, ec);
        if (!context.SourceID.IsValid() || !context.SourceFile.is_absolute() ||
            !context.StagingDirectory.is_absolute() || !context.Settings.empty() || !context.SourceInputs.empty() ||
            ec || !std::filesystem::is_directory(status) || !std::filesystem::is_empty(context.StagingDirectory, ec) ||
            ec)
            return AssetError {AssetErrorCode::InvalidArgument, "Invalid audio event import context"};

        auto read = ReadProductFile(context.SourceFile, 1024 * 1024);
        if (auto* error = std::get_if<AssetError>(&read))
            return *error;
        const auto& bytes = std::get<std::vector<std::byte>>(read);
        const auto  data  = Decode(bytes);
        if (const auto* error = std::get_if<AssetError>(&data))
            return *error;
        const auto           inputs = ClipInputs(std::get<AudioEventData>(data));
        std::vector<AssetID> dependencies;
        // The build coordinator resolves and type-checks the discovered products before staging.
        for (const auto& dependency : inputs.ProductsWithDependencies)
            dependencies.push_back(dependency.first);
        auto ids = AllocateLocalIDs(context.PreviousLocalIDs, {RootObjectKey});
        if (auto* error = std::get_if<AssetError>(&ids))
            return *error;
        ImportResult result;
        result.LocalIDs    = std::get<LocalIDMap>(std::move(ids));
        const auto local   = result.LocalIDs.at(RootObjectKey);
        const auto file    = std::to_string(local) + ".kaudioevent";
        auto       written = WriteProductFile(context.StagingDirectory / file, bytes);
        if (auto* error = std::get_if<AssetError>(&written))
            return *error;
        result.Products.push_back({local, AssetType::AudioEvent, file, std::move(dependencies)});
        return result;
    }

    std::variant<Ref<AudioEventAsset>, AssetError> Loader<AudioEventAsset>::Load(const AssetLoadContext& context) const
    {
        if (!context.ID.IsValid() || context.Type != AssetType::AudioEvent || !context.Data || !context.Size ||
            context.Size > 1024 * 1024)
            return AssetError {AssetErrorCode::InvalidArgument, "Invalid audio event product"};
        auto decoded = ParseAudioEvent(std::string(reinterpret_cast<const char*>(context.Data), context.Size));
        if (const auto* error = std::get_if<AssetError>(&decoded))
            return *error;
        auto asset     = CreateRef<AudioEventAsset>(std::get<AudioEventData>(std::move(decoded)));
        asset->assetID = context.ID;
        return asset;
    }
} // namespace Kans
