#include "kspch.h"
#include "ProductStore.h"
#include <set>
#include <unordered_set>
#include <yaml-cpp/yaml.h>
#include "Kans3D/Asset/Loader/AssetLoaderRegistry.h"
#include "Kans3D/Asset/Product/ContentDigest.h"

namespace Kans
{
    namespace
    {
        namespace fs = std::filesystem;

        constexpr uint32_t CatalogVersion      = 4;
        constexpr uint64_t MaximumCatalogBytes = 256 * 1024 * 1024;
        constexpr uint64_t MaximumProductBytes = 512 * 1024 * 1024;

        // Product files of a generation stay on disk while a ProductRecord in this
        // process refers to it, even after a newer build replaces it.
        std::mutex                                                 leaseMutex;
        std::map<fs::path, std::weak_ptr<const ProductGeneration>> leases;

        Ref<const ProductGeneration> AcquireGeneration(const fs::path& root, const std::string& generation)
        {
            std::lock_guard             lock(leaseMutex);
            auto&                       weak = leases[root / fs::u8path(generation)];
            if (auto existing = weak.lock())
                return existing;
            auto lease       = CreateRef<ProductGeneration>();
            lease->Directory = AssetPath(fs::u8path(generation));
            weak             = lease;
            return lease;
        }

        bool GenerationInUse(const fs::path& path)
        {
            std::lock_guard lock(leaseMutex);
            for (auto it = leases.begin(); it != leases.end();)
                it = it->second.expired() ? leases.erase(it) : std::next(it);
            return leases.count(path) != 0;
        }

        std::string NewGenerationName(SourceAssetID source)
        {
            return "generations/" + source.ToString() + "-" + SourceAssetID::Generate().ToString();
        }

        std::vector<std::byte> ToBytes(const std::string& text)
        {
            std::vector<std::byte> bytes(text.size());
            std::memcpy(bytes.data(), text.data(), text.size());
            return bytes;
        }

        // Checks the import result against the identities already published for the source.
        std::variant<std::monostate, AssetError> ValidateImport(const ImportResult& imported,
                                                                const LocalIDMap&   previous)
        {
            for (const auto& [key, id] : previous)
            {
                const auto found = imported.LocalIDs.find(key);
                if (found == imported.LocalIDs.end() || found->second != id)
                    return AssetError {AssetErrorCode::InvalidData, "Importer changed or dropped a LocalID: " + key};
            }
            std::unordered_set<uint64_t> mapped;
            for (const auto& [key, id] : imported.LocalIDs)
                if (key.empty() || !id || !mapped.insert(id).second)
                    return AssetError {AssetErrorCode::InvalidData, "Invalid or duplicate LocalID mapping"};
            std::unordered_set<uint64_t>  ids;
            std::unordered_set<AssetPath> files;
            for (const auto& product : imported.Products)
            {
                const AssetPath file(product.StagedFile);
                if (!product.LocalID || product.Type == AssetType::None || !file.IsValid() ||
                    file.GetPath().has_parent_path())
                    return AssetError {AssetErrorCode::InvalidData, "Invalid Product descriptor"};
                if (!mapped.count(product.LocalID))
                    return AssetError {AssetErrorCode::InvalidData, "Product LocalID has no object key"};
                if (!ids.insert(product.LocalID).second || !files.insert(file).second)
                    return AssetError {AssetErrorCode::DuplicateID, "Duplicate Product LocalID or file"};
            }
            return std::monostate {};
        }

        // ---- Catalog encoding ----------------------------------------------

        void EmitIDs(YAML::Emitter& out, const char* key, const std::vector<std::string>& values)
        {
            out << YAML::Key << key << YAML::Value << YAML::Flow << YAML::BeginSeq;
            for (const auto& value : values)
                out << value;
            out << YAML::EndSeq;
        }

        std::string EncodeCatalog(const std::map<SourceAssetID, SourceProducts>& sources)
        {
            YAML::Emitter out;
            out << YAML::BeginMap;
            out << YAML::Key << "format" << YAML::Value << "KansProductCatalog";
            out << YAML::Key << "version" << YAML::Value << CatalogVersion;
            out << YAML::Key << "sources" << YAML::Value << YAML::BeginMap;
            for (const auto& [source, state] : sources)
            {
                out << YAML::Key << source.ToString() << YAML::Value << YAML::BeginMap;
                out << YAML::Key << "local_ids" << YAML::Value << YAML::BeginMap;
                for (const auto& [key, id] : state.LocalIDs)
                    out << YAML::Key << key << YAML::Value << id;
                out << YAML::EndMap;
                out << YAML::Key << "targets" << YAML::Value << YAML::BeginMap;
                for (const auto& [target, build] : state.Targets)
                {
                    out << YAML::Key << target << YAML::Value << YAML::BeginMap;
                    out << YAML::Key << "generation" << YAML::Value << build.Generation;
                    out << YAML::Key << "fingerprint" << YAML::Value << build.Fingerprint;
                    std::vector<std::string> text;
                    for (auto id : build.SourceInputs)
                        text.push_back(id.ToString());
                    EmitIDs(out, "source_inputs", text);
                    text.clear();
                    for (auto id : build.ProductInputs)
                        text.push_back(id.ToString());
                    EmitIDs(out, "product_inputs", text);
                    out << YAML::Key << "products" << YAML::Value << YAML::BeginSeq;
                    for (const auto& product : build.Products)
                    {
                        out << YAML::BeginMap;
                        out << YAML::Key << "local" << YAML::Value << product.Metadata.assetID.GetLocalID();
                        out << YAML::Key << "type" << YAML::Value << static_cast<uint32_t>(product.Metadata.Type);
                        out << YAML::Key << "file" << YAML::Value
                            << product.Location.RelativeFile.GetPath().filename().generic_u8string();
                        out << YAML::Key << "size" << YAML::Value << product.Location.ByteSize;
                        out << YAML::Key << "digest" << YAML::Value << product.Location.ContentDigest;
                        text.clear();
                        for (auto id : product.Metadata.Dependencies)
                            text.push_back(id.ToString());
                        EmitIDs(out, "dependencies", text);
                        out << YAML::EndMap;
                    }
                    out << YAML::EndSeq << YAML::EndMap;
                }
                out << YAML::EndMap << YAML::EndMap;
            }
            out << YAML::EndMap << YAML::EndMap;
            if (!out.good())
                throw std::runtime_error(out.GetLastError());
            return std::string(out.c_str()) + "\n";
        }

        std::map<SourceAssetID, SourceProducts> DecodeCatalog(const std::string& text, const fs::path& root)
        {
            const auto require = [](bool valid, const char* message) {
                if (!valid)
                    throw std::runtime_error(message);
            };
            const YAML::Node document = YAML::Load(text);
            require(document.IsMap() && document["format"].as<std::string>("") == "KansProductCatalog",
                    "Not a Product catalog");
            require(document["version"].as<uint32_t>(0) == CatalogVersion, "Unsupported Product catalog version");
            std::map<SourceAssetID, SourceProducts> sources;
            for (const auto& entry : document["sources"])
            {
                const auto source = SourceAssetID::FromString(entry.first.as<std::string>());
                require(source.IsValid(), "Invalid catalog source ID");
                SourceProducts state;
                for (const auto& id : entry.second["local_ids"])
                    state.LocalIDs.emplace(id.first.as<std::string>(), id.second.as<uint64_t>());
                for (const auto& targetEntry : entry.second["targets"])
                {
                    const auto         target = targetEntry.first.as<std::string>();
                    const auto&        node   = targetEntry.second;
                    ProductBuildRecord build;
                    build.Generation  = node["generation"].as<std::string>();
                    build.Fingerprint = node["fingerprint"].as<std::string>();
                    require(IsProductTarget(target) && AssetPath(fs::u8path(build.Generation)).IsValid(),
                            "Invalid catalog build");
                    for (const auto& id : node["source_inputs"])
                        build.SourceInputs.push_back(SourceAssetID::FromString(id.as<std::string>()));
                    for (const auto& id : node["product_inputs"])
                        build.ProductInputs.push_back(AssetID::FromString(id.as<std::string>()));
                    const auto lease = AcquireGeneration(root, build.Generation);
                    for (const auto& item : node["products"])
                    {
                        ProductRecord record;
                        record.Metadata.assetID = {source, item["local"].as<uint64_t>()};
                        record.Metadata.Type    = static_cast<AssetType>(item["type"].as<uint32_t>());
                        record.Target           = target;
                        record.Location.RelativeFile =
                            AssetPath(fs::u8path(build.Generation) / fs::u8path(item["file"].as<std::string>()));
                        record.Location.ByteSize      = item["size"].as<uint64_t>();
                        record.Location.ContentDigest = item["digest"].as<std::string>();
                        record.Location.Generation    = lease;
                        for (const auto& id : item["dependencies"])
                            record.Metadata.Dependencies.push_back(AssetID::FromString(id.as<std::string>()));
                        build.Products.push_back(std::move(record));
                    }
                    state.Targets.emplace(target, std::move(build));
                }
                sources.emplace(source, std::move(state));
            }
            return sources;
        }

        std::variant<Ref<const AssetRegistry>, AssetError>
        BuildRegistry(const std::map<SourceAssetID, SourceProducts>& sources)
        {
            std::vector<ProductRecord> records;
            for (const auto& [source, state] : sources)
                for (const auto& [target, build] : state.Targets)
                    records.insert(records.end(), build.Products.begin(), build.Products.end());
            auto registry = AssetRegistry::FromProducts(records);
            if (auto* error = std::get_if<AssetError>(&registry))
                return std::move(*error);
            return Ref<const AssetRegistry>(CreateRef<AssetRegistry>(std::get<AssetRegistry>(std::move(registry))));
        }
    } // namespace

    std::variant<Scope<ProductStore>, AssetError> ProductStore::Open(const fs::path& root)
    {
        if (!root.is_absolute())
            return AssetError {AssetErrorCode::InvalidArgument, "Product store root must be absolute"};
        try
        {
            fs::create_directories(root / "generations");
            auto store    = Scope<ProductStore>(new ProductStore());
            store->m_Root = fs::canonical(root);
            auto lock     = ProductStoreLock::Acquire(store->m_Root / "writer.lock");
            if (auto* error = std::get_if<AssetError>(&lock))
                return std::move(*error);
            store->m_WriterLock = std::get<Scope<ProductStoreLock>>(std::move(lock));

            auto            catalog = CreateRef<Catalog>();
            const auto      file    = store->m_Root / "catalog.yaml";
            std::error_code ec;
            if (fs::exists(file, ec))
            {
                try
                {
                    auto bytes = ReadProductFile(file, MaximumCatalogBytes);
                    if (auto* error = std::get_if<AssetError>(&bytes))
                        throw std::runtime_error(error->Message);
                    const auto& data = std::get<std::vector<std::byte>>(bytes);
                    catalog->Sources = DecodeCatalog(
                        std::string(reinterpret_cast<const char*>(data.data()), data.size()), store->m_Root);
                    // Drop builds whose files are gone; their sources are imported again.
                    for (auto& [source, state] : catalog->Sources)
                        for (auto it = state.Targets.begin(); it != state.Targets.end();)
                        {
                            bool complete = true;
                            for (const auto& product : it->second.Products)
                            {
                                const auto path = store->m_Root / product.Location.RelativeFile.GetPath();
                                complete &= fs::is_regular_file(path, ec) &&
                                            fs::file_size(path, ec) == product.Location.ByteSize;
                            }
                            if (!complete)
                                store->m_Warnings.push_back({AssetErrorCode::NotFound,
                                                             "Products of " + source.ToString() + " [" + it->first +
                                                                 "] are missing and will be rebuilt"});
                            it = complete ? std::next(it) : state.Targets.erase(it);
                        }
                    auto registry = BuildRegistry(catalog->Sources);
                    if (auto* error = std::get_if<AssetError>(&registry))
                        throw std::runtime_error(error->Message);
                    catalog->Registry = std::get<Ref<const AssetRegistry>>(std::move(registry));
                }
                catch (const std::exception& error)
                {
                    // The catalog is a cache of import results; start empty and rebuild.
                    store->m_Warnings.push_back(
                        {AssetErrorCode::InvalidData,
                         "Product catalog is unreadable and will be rebuilt: " + std::string(error.what())});
                    catalog = CreateRef<Catalog>();
                    fs::rename(file, store->m_Root / "catalog.invalid.yaml", ec);
                }
            }
            store->m_Catalog = std::move(catalog);
            store->CollectGarbage();
            return store;
        }
        catch (const std::exception& error)
        {
            return AssetError {AssetErrorCode::IoError, std::string("Cannot open Product store: ") + error.what()};
        }
    }

    ProductStore::~ProductStore() = default;

    Ref<const ProductStore::Catalog> ProductStore::Current() const
    {
        std::lock_guard lock(m_Mutex);
        return m_Catalog;
    }

    Ref<const AssetRegistry> ProductStore::Snapshot() const { return Current()->Registry; }

    std::optional<SourceProducts> ProductStore::FindSource(SourceAssetID source) const
    {
        const auto catalog = Current();
        const auto found   = catalog->Sources.find(source);
        return found == catalog->Sources.end() ? std::nullopt : std::optional<SourceProducts>(found->second);
    }

    std::optional<ProductBuildRecord> ProductStore::FindBuild(SourceAssetID source, const std::string& target) const
    {
        const auto catalog = Current();
        const auto found   = catalog->Sources.find(source);
        if (found == catalog->Sources.end())
            return std::nullopt;
        const auto build = found->second.Targets.find(target);
        return build == found->second.Targets.end() ? std::nullopt : std::optional<ProductBuildRecord>(build->second);
    }

    std::vector<SourceAssetID> ProductStore::FindDependents(SourceAssetID      source,
                                                            const std::string& requestedTarget) const
    {
        const auto                 catalog = Current();
        std::vector<SourceAssetID> result;
        for (const auto& [id, state] : catalog->Sources)
            for (const auto& [target, build] : state.Targets)
            {
                if (!requestedTarget.empty() && target != requestedTarget)
                    continue;
                const bool reads = std::count(build.SourceInputs.begin(), build.SourceInputs.end(), source) > 0;
                const bool uses  = std::any_of(build.ProductInputs.begin(),
                                               build.ProductInputs.end(),
                                               [&](AssetID input) { return input.GetSourceID() == source; });
                if ((reads || uses) && std::find(result.begin(), result.end(), id) == result.end())
                    result.push_back(id);
            }
        return result;
    }

    std::variant<std::monostate, AssetError> ProductStore::Commit(Ref<const Catalog> catalog)
    {
        try
        {
            const auto temporary = m_Root / "catalog.yaml.tmp";
            auto       written   = WriteProductFile(temporary, ToBytes(EncodeCatalog(catalog->Sources)));
            if (auto* error = std::get_if<AssetError>(&written))
                return std::move(*error);
            auto replaced = ReplaceProductCatalog(temporary, m_Root / "catalog.yaml");
            if (auto* error = std::get_if<AssetError>(&replaced))
                return std::move(*error);
        }
        catch (const std::exception& error)
        {
            return AssetError {AssetErrorCode::IoError, std::string("Cannot write Product catalog: ") + error.what()};
        }
        std::lock_guard lock(m_Mutex);
        m_Catalog = std::move(catalog);
        return std::monostate {};
    }

    std::variant<Ref<const AssetRegistry>, AssetError> ProductStore::Publish(const PublishRequest& request)
    {
        if (!request.Source.IsValid() || !IsProductTarget(request.Target) || request.Fingerprint.empty() ||
            !request.StagingDirectory.is_absolute())
            return AssetError {AssetErrorCode::InvalidArgument, "Invalid Product publication request"};
        const auto progress = [&](PublicationPhase phase) { return !request.Progress || request.Progress(phase); };
        const auto current  = Current();
        const auto previous = current->Sources.find(request.Source);
        auto valid = ValidateImport(request.Imported,
                                    previous == current->Sources.end() ? LocalIDMap {} : previous->second.LocalIDs);
        if (auto* error = std::get_if<AssetError>(&valid))
            return std::move(*error);

        // Read and decode every staged Product before writing anything.
        const auto                          loaders = AssetLoaderRegistry::CreateDefault();
        std::vector<std::vector<std::byte>> payloads;
        std::set<AssetID>                   produced;
        for (const auto& product : request.Imported.Products)
            produced.insert({request.Source, product.LocalID});
        for (const auto& product : request.Imported.Products)
        {
            auto bytes = ReadProductFile(request.StagingDirectory / product.StagedFile, MaximumProductBytes);
            if (auto* error = std::get_if<AssetError>(&bytes))
                return std::move(*error);
            auto& data = std::get<std::vector<std::byte>>(bytes);
            if (data.empty())
                return AssetError {AssetErrorCode::InvalidData, "Staged Product is empty"};
            const AssetID id {request.Source, product.LocalID};
            if (const auto* decode = loaders.Find(product.Type))
            {
                auto decoded = (*decode)({id, product.Type, data.data(), data.size()});
                if (auto* error = std::get_if<AssetError>(&decoded))
                    return AssetError {AssetErrorCode::InvalidData,
                                       "Staged Product " + id.ToString() + " is invalid: " + error->Message};
            }
            else
                return AssetError {AssetErrorCode::NotImplemented, "No loader for staged Product type"};
            for (AssetID dependency : product.Dependencies)
                if (dependency.GetSourceID() == request.Source ? !produced.count(dependency) :
                                                                 !current->Registry->Find(dependency, request.Target))
                    return AssetError {AssetErrorCode::NotFound,
                                       "Product dependency " + dependency.ToString() + " is not published"};
            payloads.push_back(std::move(data));
        }
        if (!progress(PublicationPhase::Validated))
            return AssetError {AssetErrorCode::Cancelled, "Publication cancelled"};

        // Write the new generation. It is invisible until the catalog commit.
        const auto generation = NewGenerationName(request.Source);
        const auto directory  = m_Root / fs::u8path(generation);
        struct Cleanup
        {
            fs::path Directory;
            bool     Keep = false;
            ~Cleanup()
            {
                std::error_code ignored;
                if (!Keep)
                    fs::remove_all(Directory, ignored);
            }
        } cleanup {directory};
        ProductBuildRecord build;
        build.Generation    = generation;
        build.Fingerprint   = request.Fingerprint;
        build.SourceInputs  = request.SourceInputs;
        build.ProductInputs = request.ProductInputs;
        try
        {
            fs::create_directories(directory);
            const auto lease = AcquireGeneration(m_Root, generation);
            for (size_t i = 0; i < request.Imported.Products.size(); ++i)
            {
                const auto& product = request.Imported.Products[i];
                const auto  file    = product.StagedFile.filename();
                auto        written = WriteProductFile(directory / file, payloads[i]);
                if (auto* error = std::get_if<AssetError>(&written))
                    return std::move(*error);
                ProductRecord record;
                record.Metadata.assetID       = {request.Source, product.LocalID};
                record.Metadata.Type          = product.Type;
                record.Metadata.Dependencies  = product.Dependencies;
                record.Target                 = request.Target;
                record.Location.RelativeFile  = AssetPath(fs::u8path(generation) / file);
                record.Location.ByteSize      = payloads[i].size();
                record.Location.ContentDigest = ComputeContentDigest(payloads[i]);
                record.Location.Generation    = lease;
                build.Products.push_back(std::move(record));
            }
            auto synced = SyncProductDirectory(directory);
            if (auto* error = std::get_if<AssetError>(&synced))
                return std::move(*error);
        }
        catch (const std::exception& error)
        {
            return AssetError {AssetErrorCode::IoError, std::string("Cannot write Products: ") + error.what()};
        }
        if (!progress(PublicationPhase::ProductsWritten) || !progress(PublicationPhase::BeforeCommit))
            return AssetError {AssetErrorCode::Cancelled, "Publication cancelled"};

        auto  next                    = CreateRef<Catalog>(*current);
        auto& state                   = next->Sources[request.Source];
        state.LocalIDs                = request.Imported.LocalIDs;
        state.Targets[request.Target] = std::move(build);
        auto registry                 = BuildRegistry(next->Sources);
        if (auto* error = std::get_if<AssetError>(&registry))
            return std::move(*error);
        next->Registry = std::get<Ref<const AssetRegistry>>(std::move(registry));
        auto committed = Commit(next);
        if (auto* error = std::get_if<AssetError>(&committed))
            return std::move(*error);
        cleanup.Keep = true;
        progress(PublicationPhase::Committed);
        CollectGarbage();
        return next->Registry;
    }

    std::variant<Ref<const AssetRegistry>, AssetError> ProductStore::RemoveSource(SourceAssetID source)
    {
        const auto current = Current();
        const auto found   = current->Sources.find(source);
        if (found == current->Sources.end() || found->second.Targets.empty())
            return current->Registry;
        auto next = CreateRef<Catalog>(*current);
        next->Sources[source].Targets.clear();
        auto registry = BuildRegistry(next->Sources);
        if (auto* error = std::get_if<AssetError>(&registry))
            return std::move(*error);
        next->Registry = std::get<Ref<const AssetRegistry>>(std::move(registry));
        auto committed = Commit(next);
        if (auto* error = std::get_if<AssetError>(&committed))
            return std::move(*error);
        CollectGarbage();
        return next->Registry;
    }

    size_t ProductStore::CollectGarbage()
    {
        const auto            catalog = Current();
        std::set<std::string> published;
        for (const auto& [source, state] : catalog->Sources)
            for (const auto& [target, build] : state.Targets)
                published.insert(fs::u8path(build.Generation).filename().u8string());
        std::vector<fs::path> unused;
        std::error_code       error;
        for (fs::directory_iterator it(m_Root / "generations", error), end; !error && it != end; it.increment(error))
            if (!published.count(it->path().filename().u8string()) &&
                !GenerationInUse(m_Root / "generations" / it->path().filename()))
                unused.push_back(it->path());
        size_t removed = 0;
        for (const auto& path : unused)
        {
            std::error_code ignored;
            removed += fs::remove_all(path, ignored) > 0;
        }
        return removed;
    }
} // namespace Kans
