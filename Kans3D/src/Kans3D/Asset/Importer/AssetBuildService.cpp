#include "kspch.h"
#include "AssetBuildService.h"
#include "Kans3D/Asset/Loader/Loader.h"
#include "Kans3D/Asset/Product/ContentDigest.h"
#include "ImportFingerprint.h"

namespace Kans
{
    namespace fs = std::filesystem;

    std::variant<std::string, AssetError> FileDigestCache::Digest(const fs::path& file)
    {
        std::error_code error;
        const auto      size = fs::file_size(file, error);
        const auto      time = error ? fs::file_time_type {} : fs::last_write_time(file, error);
        if (error)
            return AssetError {AssetErrorCode::NotFound, "Cannot read " + file.u8string() + ": " + error.message()};
        const auto key = file.lexically_normal().u8string();
        {
            std::lock_guard<std::mutex> lock(m_Mutex);
            const auto                  found = m_Entries.find(key);
            if (found != m_Entries.end() && found->second.Size == size && found->second.Time == time)
                return found->second.Digest;
        }
        auto bytes = ReadProductFile(file, 1024ull * 1024 * 1024);
        if (auto* failure = std::get_if<AssetError>(&bytes))
            return std::move(*failure);
        auto                        digest = ComputeContentDigest(std::get<std::vector<std::byte>>(bytes));
        std::lock_guard<std::mutex> lock(m_Mutex);
        m_Entries[key] = {size, time, digest};
        return digest;
    }

    namespace
    {
        struct StageDirectory
        {
            fs::path Path;
            ~StageDirectory()
            {
                std::error_code ignored;
                if (!Path.empty())
                    fs::remove_all(Path, ignored);
            }
        };

        // Digests of the source and its file inputs, used both for the build key and
        // to detect edits made while the importer runs.
        std::variant<std::vector<SourceFingerprintInput>, AssetError> DigestSources(const AssetBuildInputs& inputs,
                                                                                    FileDigestCache&        digests)
        {
            std::vector<SourceFingerprintInput> result;
            auto                                primary = digests.Digest(inputs.SourceFile);
            if (auto* error = std::get_if<AssetError>(&primary))
                return std::move(*error);
            result.push_back({inputs.Source.sourceAssetID, std::get<std::string>(std::move(primary))});
            for (const auto& [file, id] : inputs.SourceInputs)
            {
                auto digest = digests.Digest(file);
                if (auto* error = std::get_if<AssetError>(&digest))
                    return std::move(*error);
                result.push_back({id, std::get<std::string>(std::move(digest))});
            }
            return result;
        }
    } // namespace

    std::variant<AssetBuildResult, AssetError> AssetBuildService::Build(const AssetBuildInputs&  inputs,
                                                                        const AssetBuildOptions& options,
                                                                        ProductStore&            store,
                                                                        FileDigestCache&         digests,
                                                                        const fs::path&          workRoot)
    {
        const auto source = inputs.Source.sourceAssetID;
        if (!source.IsValid() || !inputs.Importer.Importer || !inputs.SourceFile.is_absolute() ||
            options.Toolchain.empty() || !IsProductTarget(options.Target))
            return AssetError {AssetErrorCode::InvalidArgument, "Invalid asset build request"};
        try
        {
            auto sources = DigestSources(inputs, digests);
            if (auto* error = std::get_if<AssetError>(&sources))
                return std::move(*error);
            const auto&            sourceDigests = std::get<std::vector<SourceFingerprintInput>>(sources);
            const auto             registry      = store.Snapshot();
            ImportFingerprintInput key;
            key.Source          = sourceDigests.front();
            key.Sources         = {sourceDigests.begin() + 1, sourceDigests.end()};
            key.Importer        = inputs.Importer.Name;
            key.ImporterVersion = inputs.Importer.Importer->GetVersion();
            key.Toolchain       = options.Toolchain;
            const auto environment = inputs.Importer.Importer->GetEnvironmentFingerprint();
            if (!environment.empty())
                key.Toolchain += ";engine=" + environment;
            key.Target          = options.Target;
            key.Settings        = inputs.Source.Import.Settings;
            key.LocalIDs        = inputs.PreviousLocalIDs;
            for (AssetID id : inputs.ProductInputs)
            {
                const auto record = registry->Find(id, options.Target);
                if (!record)
                    return AssetError {AssetErrorCode::NotFound, "Input Product " + id.ToString() + " is not built"};
                key.Products.push_back({id, record->Location.ContentDigest});
            }

            AssetBuildResult result;
            result.Source    = source;
            auto fingerprint = ComputeImportFingerprint(key);
            if (auto* error = std::get_if<AssetError>(&fingerprint))
                return std::move(*error);
            result.Fingerprint  = std::get<std::string>(std::move(fingerprint));
            const auto previous = store.FindBuild(source, options.Target);
            const auto intact   = [&] {
                for (const auto& product : previous->Products)
                {
                    std::error_code error;
                    if (product.Metadata.Type == AssetType::Shader)
                    {
                        auto bytes = ReadRegisteredProduct(store.GetRoot(), product);
                        if (std::holds_alternative<AssetError>(bytes))
                            return false;
                        const auto& data   = std::get<std::vector<std::byte>>(bytes);
                        auto        loaded = Loader<ShaderDataAsset>().Load(
                            {product.Metadata.assetID, AssetType::Shader, data.data(), data.size()});
                        if (std::holds_alternative<AssetError>(loaded))
                            return false; // Cached contract failure is stale: use the normal import below.
                    }
                    if (fs::file_size(store.GetRoot() / product.Location.RelativeFile.GetPath(), error) !=
                            product.Location.ByteSize ||
                        error)
                        return false;
                }
                return true;
            };
            if (!options.Force && previous && previous->Fingerprint == result.Fingerprint && intact())
            {
                result.Disposition = AssetBuildDisposition::UpToDate;
                result.Registry    = registry;
                result.LocalIDs    = inputs.PreviousLocalIDs;
                return result;
            }

            StageDirectory stage;
            fs::create_directories(workRoot);
            stage.Path = workRoot / ("stage-" + SourceAssetID::Generate().ToString());
            if (!fs::create_directory(stage.Path))
                return AssetError {AssetErrorCode::IoError, "Cannot create import staging directory"};
            ImportContext context;
            context.SourceID         = source;
            context.SourceFile       = inputs.SourceFile;
            context.AssetRoot        = inputs.AssetRoot;
            context.Target           = options.Target;
            context.StagingDirectory = stage.Path;
            context.Settings         = inputs.Source.Import.Settings;
            context.PreviousLocalIDs = inputs.PreviousLocalIDs;
            context.SourceInputs     = inputs.SourceInputs;
            context.ProductsBySource = inputs.ProductsBySource;
            if (inputs.Importer.Importer->RequiresProductBytes())
                for (AssetID id : inputs.ProductInputs)
                {
                    auto bytes = ReadRegisteredProduct(store.GetRoot(), *registry->Find(id, options.Target));
                    if (auto* error = std::get_if<AssetError>(&bytes))
                        return std::move(*error);
                    context.ProductBytes.emplace(id, std::get<std::vector<std::byte>>(std::move(bytes)));
                }
            auto imported = inputs.Importer.Importer->Import(context);
            if (auto* error = std::get_if<AssetError>(&imported))
                return std::move(*error);
            auto& output = std::get<ImportResult>(imported);
            for (const auto& product : output.Products)
                if (product.Type == AssetType::Shader)
                {
                    auto bytes = ReadProductFile(stage.Path / product.StagedFile);
                    if (auto* error = std::get_if<AssetError>(&bytes))
                        return *error;
                    const auto& data   = std::get<std::vector<std::byte>>(bytes);
                    auto        loaded = Loader<ShaderDataAsset>().Load(
                        {{source, product.LocalID}, AssetType::Shader, data.data(), data.size()});
                    if (auto* error = std::get_if<AssetError>(&loaded))
                        return *error;
                }

            // Publishing Products of a half-edited source would mix versions.
            auto after = DigestSources(inputs, digests);
            if (auto* error = std::get_if<AssetError>(&after))
                return std::move(*error);
            const auto& afterDigests = std::get<std::vector<SourceFingerprintInput>>(after);
            for (size_t i = 0; i < afterDigests.size(); ++i)
                if (afterDigests[i].ContentDigest != sourceDigests[i].ContentDigest)
                    return AssetError {AssetErrorCode::InvalidData, "Source files changed during import; build again"};

            key.LocalIDs = output.LocalIDs;
            auto final   = ComputeImportFingerprint(key);
            if (auto* error = std::get_if<AssetError>(&final))
                return std::move(*error);
            result.Fingerprint = std::get<std::string>(std::move(final));

            PublishRequest request;
            request.Source           = source;
            request.Target           = options.Target;
            request.Fingerprint      = result.Fingerprint;
            request.StagingDirectory = stage.Path;
            request.Imported         = output;
            request.ProductInputs    = inputs.ProductInputs;
            request.Progress         = options.Progress;
            for (const auto& [file, id] : inputs.SourceInputs)
                request.SourceInputs.push_back(id);
            auto published = store.Publish(request);
            if (auto* error = std::get_if<AssetError>(&published))
                return std::move(*error);
            result.Registry = std::get<Ref<const AssetRegistry>>(std::move(published));
            result.LocalIDs = std::move(output.LocalIDs);
            result.Warnings = std::move(output.Warnings);
            return result;
        }
        catch (const std::exception& error)
        {
            return AssetError {AssetErrorCode::IoError, std::string("Asset build failed: ") + error.what()};
        }
    }
} // namespace Kans
