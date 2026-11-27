#include "kspch.h"
#include "MusicSetImporter.h"
#include "Kans3D/Asset/Importer/LocalIDMap.h"
#include "Kans3D/Asset/Loader/Loader.h"
#include "Kans3D/Asset/Product/AudioProduct.h"
#include "Kans3D/Asset/Storage/ProductFiles.h"
#include "Kans3D/Audio/MusicSet.h"

namespace Kans
{
    namespace
    {
        std::variant<MusicSetData, AssetError> Decode(const std::vector<std::byte>& bytes)
        {
            if (bytes.empty())
                return AssetError {AssetErrorCode::InvalidData, "Music set file is empty"};
            return ParseMusicSet(std::string(reinterpret_cast<const char*>(bytes.data()), bytes.size()));
        }

        ImportInputs MusicInputs(const MusicSetData& data)
        {
            ImportInputs inputs;
            for (const auto& segment : data.Segments)
                for (const auto& layer : segment.Layers)
                    inputs.ProductsWithDependencies.emplace(layer.Clip, AssetType::AudioClip);
            for (const auto& stinger : data.Stingers)
                inputs.ProductsWithDependencies.emplace(stinger.Clip, AssetType::AudioClip);
            return inputs;
        }
    } // namespace

    std::variant<ImportInputs, AssetError> MusicSetImporter::DiscoverInputs(const ImportContext& context) const
    {
        auto read = ReadProductFile(context.SourceFile, 1024 * 1024);
        if (auto* error = std::get_if<AssetError>(&read))
            return *error;
        auto data = Decode(std::get<std::vector<std::byte>>(read));
        if (auto* error = std::get_if<AssetError>(&data))
            return *error;
        return MusicInputs(std::get<MusicSetData>(data));
    }

    std::variant<ImportResult, AssetError> MusicSetImporter::Import(const ImportContext& context) const
    {
        std::error_code ec;
        const auto      status = std::filesystem::symlink_status(context.StagingDirectory, ec);
        if (!context.SourceID.IsValid() || !context.SourceFile.is_absolute() ||
            !context.StagingDirectory.is_absolute() || !context.Settings.empty() || !context.SourceInputs.empty() ||
            ec || !std::filesystem::is_directory(status) || !std::filesystem::is_empty(context.StagingDirectory, ec) ||
            ec)
            return AssetError {AssetErrorCode::InvalidArgument, "Invalid music set import context"};

        auto read = ReadProductFile(context.SourceFile, 1024 * 1024);
        if (auto* error = std::get_if<AssetError>(&read))
            return *error;
        const auto& bytes = std::get<std::vector<std::byte>>(read);
        const auto  data  = Decode(bytes);
        if (const auto* error = std::get_if<AssetError>(&data))
            return *error;
        const auto&               music  = std::get<MusicSetData>(data);
        const auto                inputs = MusicInputs(music);
        std::map<AssetID, double> lengths;
        for (const auto& [clip, type] : inputs.ProductsWithDependencies)
        {
            const auto product = context.ProductBytes.find(clip);
            if (product == context.ProductBytes.end())
                return AssetError {AssetErrorCode::NotFound, "Missing music set clip " + clip.ToString()};
            const auto& encoded = product->second;
            const auto  preview = DecodeAudioProductPreview(encoded.data(), encoded.size(), encoded.size());
            if (const auto* error = std::get_if<AssetError>(&preview))
                return *error;
            const auto& info = std::get<AudioClipData>(preview).Info;
            lengths.emplace(clip, double(info.LengthFrames) / info.SampleRate);
        }
        if (auto error = ValidateMusicSet(music, [&](AssetID clip) -> std::optional<double> {
                const auto duration = lengths.find(clip);
                return duration == lengths.end() ? std::nullopt : std::optional<double>(duration->second);
            }))
            return *error;
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
        const auto file    = std::to_string(local) + ".kmusicset";
        auto       written = WriteProductFile(context.StagingDirectory / file, bytes);
        if (auto* error = std::get_if<AssetError>(&written))
            return *error;
        result.Products.push_back({local, AssetType::MusicSet, file, std::move(dependencies)});
        return result;
    }

    std::variant<Ref<MusicSetAsset>, AssetError> Loader<MusicSetAsset>::Load(const AssetLoadContext& context) const
    {
        if (!context.ID.IsValid() || context.Type != AssetType::MusicSet || !context.Data || !context.Size ||
            context.Size > 1024 * 1024)
            return AssetError {AssetErrorCode::InvalidArgument, "Invalid music set product"};
        auto decoded = ParseMusicSet(std::string(reinterpret_cast<const char*>(context.Data), context.Size));
        if (const auto* error = std::get_if<AssetError>(&decoded))
            return *error;
        auto asset     = CreateRef<MusicSetAsset>(std::get<MusicSetData>(std::move(decoded)));
        asset->assetID = context.ID;
        return asset;
    }
} // namespace Kans
