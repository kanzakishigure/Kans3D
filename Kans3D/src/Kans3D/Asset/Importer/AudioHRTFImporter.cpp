#include "kspch.h"
#include "AudioHRTFImporter.h"
#include "Kans3D/Asset/Loader/Loader.h"
#include "Kans3D/Asset/Storage/ProductFiles.h"
#include "Kans3D/Audio/AudioHRTF.h"
#include "Kans3D/Audio/SteamAudio.h"

#include <array>
#include <cstring>

namespace Kans
{
    namespace
    {
        constexpr size_t                   MaxSOFABytes = 128 * 1024 * 1024;
        constexpr std::array<std::byte, 5> Header {std::byte {'K'},
                                                   std::byte {'H'},
                                                   std::byte {'R'},
                                                   std::byte {'F'},
                                                   std::byte {1}};
        constexpr size_t                   ProductPrefix = Header.size() + 1; // One byte for RMS normalization.

        std::variant<bool, AssetError> NormalizeVolume(const std::map<std::string, std::string>& settings)
        {
            if (settings.empty())
                return false;
            const auto setting = settings.find("normalizeVolume");
            if (settings.size() != 1 || setting == settings.end() ||
                (setting->second != "true" && setting->second != "false"))
                return AssetError {AssetErrorCode::InvalidArgument,
                                   "AudioHRTF import expects normalizeVolume=true or false"};
            return setting->second == "true";
        }
    } // namespace

    ImportSettingsSchema AudioHRTFImporter::GetSettingsSchema() const
    {
        ImportSettingDesc setting;
        setting.Key         = "normalizeVolume";
        setting.Kind        = ImportSettingDesc::Type::Bool;
        setting.Default     = "false";
        setting.Label       = "Normalize volume";
        setting.Description = "Normalize the RMS gain of the measured head-related impulse responses.";
        setting.Group       = "Audio";
        return {{std::move(setting)}, {}};
    }

    std::variant<std::monostate, AssetError>
    AudioHRTFImporter::ValidateSettings(const std::map<std::string, std::string>& settings) const
    {
        const auto parsed = NormalizeVolume(settings);
        if (const auto* error = std::get_if<AssetError>(&parsed))
            return *error;
        return std::monostate {};
    }

    std::variant<ImportResult, AssetError> AudioHRTFImporter::Import(const ImportContext& context) const
    {
        std::error_code ec;
        const auto      status = std::filesystem::symlink_status(context.StagingDirectory, ec);
        if (!context.SourceID.IsValid() || !context.SourceFile.is_absolute() ||
            !context.StagingDirectory.is_absolute() || !context.SourceInputs.empty() || ec ||
            !std::filesystem::is_directory(status) || !std::filesystem::is_empty(context.StagingDirectory, ec) || ec)
            return AssetError {AssetErrorCode::InvalidArgument, "Invalid AudioHRTF import context"};

        const auto parsed = NormalizeVolume(context.Settings);
        if (const auto* error = std::get_if<AssetError>(&parsed))
            return *error;
        const bool normalize = std::get<bool>(parsed);
        auto       read      = ReadProductFile(context.SourceFile, MaxSOFABytes);
        if (const auto* error = std::get_if<AssetError>(&read))
            return *error;
        AudioHRTFAsset asset(std::get<std::vector<std::byte>>(std::move(read)), normalize);
        if (auto error = SteamAudio::Validate(asset))
            return AssetError {AssetErrorCode::InvalidData, std::move(*error)};

        auto ids = AllocateLocalIDs(context.PreviousLocalIDs, {RootObjectKey});
        if (const auto* error = std::get_if<AssetError>(&ids))
            return *error;
        ImportResult result;
        result.LocalIDs              = std::get<LocalIDMap>(std::move(ids));
        const auto             local = result.LocalIDs.at(RootObjectKey);
        const auto             file  = std::to_string(local) + ".kaudiohrtf";
        const auto&            sofa  = asset.GetSOFA();
        std::vector<std::byte> bytes;
        bytes.reserve(ProductPrefix + sofa.size());
        bytes.insert(bytes.end(), Header.begin(), Header.end());
        bytes.push_back(normalize ? std::byte {1} : std::byte {0});
        bytes.insert(bytes.end(), sofa.begin(), sofa.end());
        auto written = WriteProductFile(context.StagingDirectory / file, bytes);
        if (const auto* error = std::get_if<AssetError>(&written))
            return *error;
        result.Products.push_back({local, AssetType::AudioHRTF, file, {}});
        return result;
    }

    std::variant<Ref<AudioHRTFAsset>, AssetError> Loader<AudioHRTFAsset>::Load(const AssetLoadContext& context) const
    {
        if (!context.ID.IsValid() || context.Type != AssetType::AudioHRTF || !context.Data ||
            context.Size <= ProductPrefix || context.Size > MaxSOFABytes + ProductPrefix)
            return AssetError {AssetErrorCode::InvalidData, "Invalid AudioHRTF product size"};
        const auto* bytes = context.Data;
        if (std::memcmp(bytes, Header.data(), Header.size()) || bytes[Header.size()] > std::byte {1})
            return AssetError {AssetErrorCode::InvalidData, "Invalid AudioHRTF product header"};
        auto asset     = CreateRef<AudioHRTFAsset>(std::vector<std::byte>(bytes + ProductPrefix, bytes + context.Size),
                                                   bytes[Header.size()] == std::byte {1});
        asset->assetID = context.ID;
        return asset;
    }
} // namespace Kans
