#include "Kans3D/Asset/SourceAssetMetaSerializer.h"
#include "Kans3D/Core/Utilities/StringUtils.h"
#include "SourceImportConfigurationYaml.h"
#include "Storage/ProductFiles.h"

#include <yaml-cpp/yaml.h>

#include <fstream>
#include <string>
#include <system_error>

namespace Kans
{
    namespace
    {

        constexpr uint32_t kCurrentMetaVersion = 3;
        constexpr uint64_t kMaximumMetaBytes   = 8 * 1024 * 1024;

        AssetError MakeMetaError(AssetErrorCode code, const std::filesystem::path& path, const std::string& message)
        {
            return AssetError {code, path.u8string() + ": " + message};
        }

        struct TemporaryMetaDirectory
        {
            std::filesystem::path Directory;
            ~TemporaryMetaDirectory()
            {
                if (Directory.empty())
                    return;
                std::error_code ignored;
                std::filesystem::remove(Directory / "payload", ignored);
                std::filesystem::remove(Directory, ignored);
            }
        };

        std::variant<std::monostate, AssetError> CheckMetaFile(const std::filesystem::path& path, bool require)
        {
            std::error_code error;
            const auto      status = std::filesystem::symlink_status(path, error);
            if (status.type() == std::filesystem::file_type::not_found)
                return require ? std::variant<std::monostate, AssetError>(
                                     MakeMetaError(AssetErrorCode::NotFound, path, "Missing .kmeta")) :
                                 std::variant<std::monostate, AssetError>(std::monostate {});
            if (error)
                return MakeMetaError(AssetErrorCode::IoError, path, error.message());
            if (!std::filesystem::is_regular_file(status))
                return MakeMetaError(AssetErrorCode::InvalidData, path, ".kmeta must be a regular file without a link");
            return std::monostate {};
        }

    } // namespace

    std::variant<SourceAssetMetadata, AssetError> SourceAssetMetaSerializer::Load(const std::filesystem::path& metaPath,
                                                                                  const AssetPath& sourcePath)
    {
        if (metaPath.empty())
            return MakeMetaError(AssetErrorCode::InvalidArgument, metaPath, "Empty .kmeta path");
        if (!sourcePath.IsValid())
            return MakeMetaError(AssetErrorCode::InvalidArgument, metaPath, "Invalid source asset path");
        if (const auto checked = CheckMetaFile(metaPath, true); std::holds_alternative<AssetError>(checked))
            return std::get<AssetError>(checked);

        try
        {
            auto bytes = ReadProductFile(metaPath, kMaximumMetaBytes);
            if (const auto* error = std::get_if<AssetError>(&bytes))
                return *error;
            const auto& data = std::get<std::vector<std::byte>>(bytes);
            if (data.empty())
                return MakeMetaError(AssetErrorCode::InvalidData, metaPath, "Empty .kmeta");
            const auto documents = YAML::LoadAll(std::string(reinterpret_cast<const char*>(data.data()), data.size()));
            if (documents.size() != 1)
                return MakeMetaError(AssetErrorCode::InvalidData, metaPath, ".kmeta requires one YAML document");
            const YAML::Node root = documents.front();
            if (!root.IsMap())
                return MakeMetaError(AssetErrorCode::InvalidData, metaPath, ".kmeta root must be a map");

            const YAML::Node versionNode = root["version"];
            const YAML::Node idNode      = root["source_id"];
            if (!versionNode.IsScalar() || !idNode.IsScalar())
                return MakeMetaError(
                    AssetErrorCode::InvalidData, metaPath, ".kmeta requires scalar version and source_id");

            const uint32_t version = versionNode.as<uint32_t>();
            if (version < 1 || version > kCurrentMetaVersion)
                return MakeMetaError(AssetErrorCode::UnsupportedVersion,
                                     metaPath,
                                     "Unsupported .kmeta version: " + std::to_string(version));
            auto fields =
                version == 1 ? SourceMetadataDetail::CheckFields(root, {"version", "source_id"}) :
                version == 2 ? SourceMetadataDetail::CheckFields(root, {"version", "source_id", "import"}) :
                               SourceMetadataDetail::CheckFields(root, {"version", "source_id", "import", "local_ids"});
            if (const auto* error = std::get_if<AssetError>(&fields))
                return *error;

            if (idNode.Scalar().size() != 16)
                return MakeMetaError(AssetErrorCode::InvalidData, metaPath, "Invalid .kmeta source_id length");
            const std::string   idText = idNode.as<std::string>();
            const SourceAssetID id     = SourceAssetID::FromString(idText);
            if (!id.IsValid())
                return MakeMetaError(AssetErrorCode::InvalidData, metaPath, "Invalid .kmeta source_id");

            SourceAssetMetadata metadata;
            metadata.sourceAssetID = id;
            metadata.Path          = sourcePath;
            metadata.MetaVersion   = version;
            if (version >= 2)
            {
                auto configuration = SourceMetadataDetail::DecodeImport(root["import"]);
                if (const auto* error = std::get_if<AssetError>(&configuration))
                    return *error;
                metadata.Import = std::get<SourceImportConfiguration>(std::move(configuration));
            }
            if (version >= 3 && root["local_ids"])
            {
                const YAML::Node ids = root["local_ids"];
                if (!ids.IsMap())
                    return MakeMetaError(AssetErrorCode::InvalidData, metaPath, ".kmeta local_ids must be a map");
                for (const auto& entry : ids)
                {
                    uint64_t   local = 0;
                    const auto key   = entry.first.as<std::string>();
                    if (key.empty() || !entry.second.IsScalar() || !Utils::HexToUint64(entry.second.Scalar(), local) ||
                        !local || !metadata.LocalIDs.emplace(key, local).second)
                        return MakeMetaError(AssetErrorCode::InvalidData, metaPath, "Invalid .kmeta local_ids entry");
                }
            }
            return metadata;
        }
        catch (const YAML::BadFile& exception)
        {
            std::error_code statusError;
            const bool      exists = std::filesystem::exists(metaPath, statusError);
            const auto      code   = !statusError && !exists ? AssetErrorCode::NotFound : AssetErrorCode::IoError;
            return MakeMetaError(code, metaPath, "Cannot open .kmeta: " + std::string(exception.what()));
        }
        catch (const YAML::Exception& exception)
        {
            return MakeMetaError(
                AssetErrorCode::InvalidData, metaPath, "Cannot parse .kmeta: " + std::string(exception.what()));
        }
        catch (const std::exception& exception)
        {
            return MakeMetaError(
                AssetErrorCode::IoError, metaPath, "Cannot load .kmeta: " + std::string(exception.what()));
        }
    }

    std::variant<std::monostate, AssetError> SourceAssetMetaSerializer::Save(const std::filesystem::path& metaPath,
                                                                             const SourceAssetMetadata&   metadata)
    {
        if (metaPath.empty())
            return MakeMetaError(AssetErrorCode::InvalidArgument, metaPath, "Empty .kmeta path");
        if (!metadata.IsValid())
            return MakeMetaError(AssetErrorCode::InvalidArgument, metaPath, "Invalid source asset metadata");
        if (metadata.MetaVersion < 1 || metadata.MetaVersion > kCurrentMetaVersion)
            return MakeMetaError(AssetErrorCode::UnsupportedVersion,
                                 metaPath,
                                 "Unsupported .kmeta version: " + std::to_string(metadata.MetaVersion));
        if (const auto checked = CheckMetaFile(metaPath, false); std::holds_alternative<AssetError>(checked))
            return std::get<AssetError>(checked);
        TemporaryMetaDirectory temporary;
        try
        {
            YAML::Emitter emitter;
            emitter << YAML::BeginMap;
            emitter << YAML::Key << "version" << YAML::Value << kCurrentMetaVersion;
            emitter << YAML::Key << "source_id" << YAML::Value << metadata.sourceAssetID.ToString();
            emitter << YAML::Key << "import" << YAML::Value;
            SourceMetadataDetail::EncodeImport(emitter, metadata.Import);
            if (!metadata.LocalIDs.empty())
            {
                emitter << YAML::Key << "local_ids" << YAML::Value << YAML::BeginMap;
                for (const auto& [key, local] : metadata.LocalIDs)
                    emitter << YAML::Key << key << YAML::Value << Utils::Uint64ToHex(local);
                emitter << YAML::EndMap;
            }
            emitter << YAML::EndMap;
            if (!emitter.good())
                return MakeMetaError(AssetErrorCode::InvalidData,
                                     metaPath,
                                     ".kmeta YAML emission failed: " + std::string(emitter.GetLastError()));

            const std::string text = std::string(emitter.c_str()) + '\n';
            if (text.size() > kMaximumMetaBytes)
                return MakeMetaError(AssetErrorCode::InvalidData, metaPath, "Encoded .kmeta exceeds byte limit");
            // Own an exclusively created directory beside the destination.
            for (int attempt = 0; attempt < 8; ++attempt)
            {
                auto candidate = metaPath;
                candidate += ".tmp-" + SourceAssetID::Generate().ToString();
                if (std::filesystem::create_directory(candidate))
                {
                    temporary.Directory = std::move(candidate);
                    break;
                }
            }
            if (temporary.Directory.empty())
                return MakeMetaError(AssetErrorCode::IoError, metaPath, "Cannot allocate temporary .kmeta filename");

            std::vector<std::byte> bytes;
            bytes.reserve(text.size());
            for (unsigned char value : text)
                bytes.push_back(static_cast<std::byte>(value));
            const auto payload = temporary.Directory / "payload";
            auto       written = WriteProductFile(payload, bytes);
            if (const auto* error = std::get_if<AssetError>(&written))
                return *error;
            // Recheck the destination immediately before the atomic replacement.
            auto checked = CheckMetaFile(metaPath, false);
            if (const auto* error = std::get_if<AssetError>(&checked))
                return *error;
            auto replaced = ReplaceProductCatalog(payload, metaPath);
            if (const auto* error = std::get_if<AssetError>(&replaced))
                return *error;
            return std::monostate {};
        }
        catch (const std::exception& exception)
        {
            return MakeMetaError(
                AssetErrorCode::IoError, metaPath, "Cannot save .kmeta: " + std::string(exception.what()));
        }
    }

} // namespace Kans
