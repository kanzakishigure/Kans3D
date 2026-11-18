#include "AssetScanner.h"
#include "Importer/ImporterRegistry.h"
#include "SourceAssetMetaSerializer.h"

#include <algorithm>
#include <cctype>
#include <string>
#include <system_error>
#include <unordered_map>

namespace Kans
{
    namespace
    {
        namespace fs = std::filesystem;

        AssetError Error(AssetErrorCode code, const fs::path& path, const std::string& message)
        {
            return {code, path.generic_u8string() + ": " + message};
        }

        bool Ignore(const fs::path& path, bool directory)
        {
            std::string name = path.filename().u8string();
            if (!name.empty() && name.front() == '.')
                return true;
            std::transform(name.begin(), name.end(), name.begin(), [](unsigned char c) {
                return static_cast<char>(std::tolower(c));
            });
            if (directory && (name == "cache" || name == "intermediate" || name == "products"))
                return true;
            return (name.size() >= 6 && name.compare(name.size() - 6, 6, ".kmeta") == 0) ||
                   name.find(".kmeta.tmp-") != std::string::npos;
        }

        std::variant<SourceAssetMetadata, AssetError>
        ReadSource(const fs::path& file, const fs::path& root, bool readOnly)
        {
            const AssetPath logicalPath(file.lexically_relative(root));
            if (!logicalPath.IsValid())
                return Error(AssetErrorCode::InvalidArgument, file, "Invalid source path");

            std::error_code ec;
            const auto      size = fs::file_size(file, ec);
            if (ec)
                return Error(AssetErrorCode::IoError, file, ec.message());
            const auto modified = fs::last_write_time(file, ec);
            if (ec)
                return Error(AssetErrorCode::IoError, file, ec.message());

            fs::path metaPath = file;
            metaPath += ".kmeta";
            const auto metaStatus = fs::symlink_status(metaPath, ec);
            if (ec && ec != std::errc::no_such_file_or_directory)
                return Error(AssetErrorCode::IoError, metaPath, ec.message());
            const bool missing = metaStatus.type() == fs::file_type::not_found;
            // In particular, do not follow a sidecar link or replace a dangling link.
            if (!missing && !fs::is_regular_file(metaStatus))
                return Error(
                    AssetErrorCode::InvalidData, metaPath, "Sidecar must be a regular file, not a link or directory");

            auto                loaded = SourceAssetMetaSerializer::Load(metaPath, logicalPath);
            SourceAssetMetadata metadata;
            if (const auto* error = std::get_if<AssetError>(&loaded))
            {
                if (error->Code != AssetErrorCode::NotFound || !missing)
                    return *error;
                if (readOnly)
                    return Error(
                        AssetErrorCode::NotFound, metaPath, "Plugin content requires a shipped .kmeta identity");
                metadata.Path          = logicalPath;
                metadata.sourceAssetID = SourceAssetID::Generate();
                static const auto registry = CreateDefaultImporterRegistry();
                if (const auto* error = std::get_if<AssetError>(&registry))
                    return *error;
                auto importer = std::get<ImporterRegistry>(registry).Resolve(file);
                if (const auto* model = std::get_if<RegisteredImporter>(&importer);
                    model && model->Name == "ModelImporter")
                    metadata.Import.Settings["animation.compression"] = "optimal";
                auto saved             = SourceAssetMetaSerializer::Save(metaPath, metadata);
                if (const auto* saveError = std::get_if<AssetError>(&saved))
                    return *saveError;
            }
            else
            {
                metadata = std::get<SourceAssetMetadata>(loaded);
            }
            metadata.Exists         = true;
            metadata.Missing        = false;
            metadata.FileSize       = size;
            metadata.last_edit_time = modified;
            return metadata;
        }
    } // namespace

    bool AssetScanner::IsDiscoverablePath(const AssetPath& path)
    {
        if (!path.IsValid() || path.GetPath().filename().empty() || path.ToString().find('\0') != std::string::npos)
            return false;
        const auto end = path.GetPath().end();
        for (auto it = path.GetPath().begin(); it != end; ++it)
        {
            auto next = it;
            ++next;
            if (Ignore(*it, next != end))
                return false;
        }
        return true;
    }

    std::variant<AssetScanResult, AssetError> AssetScanner::Scan(const std::filesystem::path& assetRoot, bool readOnly)
    {
        if (assetRoot.empty())
            return Error(AssetErrorCode::InvalidArgument, assetRoot, "Empty asset root");
        std::error_code ec;
        const auto      root = fs::canonical(assetRoot, ec);
        if (ec)
            return Error(ec == std::errc::no_such_file_or_directory ? AssetErrorCode::NotFound :
                                                                      AssetErrorCode::IoError,
                         assetRoot,
                         ec.message());
        const bool directory = fs::is_directory(root, ec);
        if (ec)
            return Error(AssetErrorCode::IoError, root, ec.message());
        if (!directory)
            return Error(AssetErrorCode::InvalidArgument, root, "Asset root must be a directory");
        return ScanNode(root, readOnly);
    }

    std::variant<AssetScanResult, AssetError> AssetScanner::ScanNode(const std::filesystem::path& assetRoot,
                                                                     bool                         readOnly)
    {
        AssetScanResult                  result;
        std::vector<fs::path>            files;
        std::error_code                  ec;
        fs::recursive_directory_iterator it(assetRoot, fs::directory_options::none, ec);
        if (ec)
            return Error(AssetErrorCode::IoError, assetRoot, ec.message());
        const fs::recursive_directory_iterator end;
        // Enumerate before creating sidecars, and fail rather than silently skipping
        // inaccessible subtrees: an incomplete traversal is not a complete snapshot.
        while (it != end)
        {
            const fs::path path   = it->path();
            const auto     status = it->symlink_status(ec);
            if (ec)
                return Error(AssetErrorCode::IoError, path, ec.message());
            if (fs::is_symlink(status) || Ignore(path, fs::is_directory(status)))
            {
                it.disable_recursion_pending();
            }
            else if (fs::is_regular_file(status))
            {
                files.push_back(path);
            }
            it.increment(ec);
            if (ec)
                return Error(AssetErrorCode::IoError, path, ec.message());
        }
        std::sort(files.begin(), files.end());

        std::vector<SourceAssetMetadata>          records;
        std::unordered_map<SourceAssetID, size_t> occurrences;
        for (const auto& file : files)
        {
            auto loaded = ReadSource(file, assetRoot, readOnly);
            if (const auto* error = std::get_if<AssetError>(&loaded))
            {
                result.Errors.push_back(*error);
                result.Skipped.emplace_back(file.lexically_relative(assetRoot));
                continue;
            }
            auto metadata = std::get<SourceAssetMetadata>(loaded);
            ++occurrences[metadata.sourceAssetID];
            records.push_back(std::move(metadata));
        }
        for (const auto& metadata : records)
        {
            const auto path = assetRoot / metadata.Path.GetPath();
            if (occurrences.at(metadata.sourceAssetID) > 1)
            {
                result.Errors.push_back(Error(
                    AssetErrorCode::DuplicateID, path, "Conflicting source ID " + metadata.sourceAssetID.ToString()));
                result.Skipped.push_back(metadata.Path);
                continue;
            }
            auto registered = result.Database.RegisterSource(metadata);
            if (const auto* error = std::get_if<AssetError>(&registered))
                result.Errors.push_back(Error(error->Code, path, error->Message));
        }
        return result;
    }
} // namespace Kans
