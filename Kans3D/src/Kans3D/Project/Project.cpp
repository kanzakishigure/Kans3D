#include "kspch.h"
#include "Project.h"
#include <yaml-cpp/yaml.h>
#include "Kans3D/Asset/AssetScanner.h"
#include "Kans3D/Asset/SourceAssetDatabaseSerializer.h"
#include "Kans3D/Asset/SourceAssetMetaSerializer.h"

namespace Kans
{
    namespace
    {
        namespace fs = std::filesystem;

        bool IsWithin(const fs::path& path, const fs::path& root)
        {
            auto p = path.begin();
            for (auto r = root.begin(); r != root.end(); ++r, ++p)
                if (p == path.end() || *p != *r)
                    return false;
            return true;
        }

        fs::path Sidecar(const fs::path& file)
        {
            auto sidecar = file;
            sidecar += ".kmeta";
            return sidecar;
        }

        bool Exists(const fs::path& path)
        {
            std::error_code error;
            return fs::symlink_status(path, error).type() != fs::file_type::not_found;
        }

        // The Product a source is referenced by when another source names its file.
        AssetID PrimaryProduct(SourceAssetID source, const LocalIDMap& ids)
        {
            for (const auto& [key, local] : ids)
                if (key.size() > 5 && key.compare(key.size() - 5, 5, ":root") == 0)
                    return {source, local};
            return {};
        }
    } // namespace

    struct Project::BuildSession
    {
        const ImporterRegistry&                   Importers;
        AssetBuildOptions                         Options;
        std::map<SourceAssetID, AssetBuildResult> Done;
        std::set<SourceAssetID>                   Active;
        std::vector<AssetBuildResult>             Order;
        std::vector<SourceAssetID>                Stack;
        std::map<SourceAssetID, AssetError>       Failed;
        // Published reverse graph for dependent propagation and batch builds.
        std::map<SourceAssetID, std::vector<SourceAssetID>> Dependents;
        // Descendant -> failed ancestor; never rebuild against its stale Product.
        std::map<SourceAssetID, SourceAssetID> Blocked;
    };

    Project::~Project() = default;

    std::variant<std::unique_ptr<Project>, AssetError> Project::Open(const ProjectConfig&      config,
                                                                     const ProjectOpenOptions& options)
    {
        if (config.Name.empty() || !config.RootDirectory.is_absolute() || config.AssetDirectory.empty() ||
            config.CacheDirectory.empty() ||
            (!options.SourceAssetCachePath.empty() && !options.SourceAssetCachePath.is_absolute()))
            return AssetError {AssetErrorCode::InvalidArgument, "Invalid project configuration"};
        try
        {
            auto  project           = std::unique_ptr<Project>(new Project());
            auto& resolved          = project->m_Config;
            resolved                = config;
            resolved.RootDirectory  = fs::canonical(config.RootDirectory);
            resolved.AssetDirectory = fs::canonical(resolved.RootDirectory / config.AssetDirectory);
            resolved.CacheDirectory = fs::weakly_canonical(resolved.RootDirectory / config.CacheDirectory);
            if (!fs::is_directory(resolved.AssetDirectory) ||
                !IsWithin(resolved.AssetDirectory, resolved.RootDirectory))
                return AssetError {AssetErrorCode::InvalidArgument, "Asset directory must be within the project root"};
            if (IsWithin(resolved.CacheDirectory, resolved.AssetDirectory) ||
                IsWithin(resolved.AssetDirectory, resolved.CacheDirectory))
                return AssetError {AssetErrorCode::InvalidArgument, "Cache and asset directories must not overlap"};
            project->m_SourceAssetCachePath = fs::weakly_canonical(
                options.SourceAssetCachePath.empty() ? resolved.CacheDirectory / "SourceAssetDatabase.cache.yaml" :
                                                       options.SourceAssetCachePath);
            if (IsWithin(project->m_SourceAssetCachePath, resolved.AssetDirectory))
                return AssetError {AssetErrorCode::InvalidArgument,
                                   "Source asset cache must be outside the asset directory"};
            fs::create_directories(resolved.CacheDirectory);

            auto lock = ProductStoreLock::Acquire(resolved.CacheDirectory / "project.lock");
            if (auto* error = std::get_if<AssetError>(&lock))
                return AssetError {error->Code, "Project is in use by another process: " + error->Message};
            project->m_Lock = std::get<Scope<ProductStoreLock>>(std::move(lock));

            auto recovered = project->RecoverInterruptedMove();
            if (auto* error = std::get_if<AssetError>(&recovered))
                project->m_AssetWarnings.push_back(std::move(*error));
            auto cached = SourceAssetDatabaseSerializer::Load(project->m_SourceAssetCachePath, resolved.AssetDirectory);
            if (auto* error = std::get_if<AssetError>(&cached))
            {
                if (error->Code != AssetErrorCode::NotFound)
                    project->m_AssetWarnings.push_back(std::move(*error));
            }
            else
                project->m_SourceAssetDatabase = std::get<SourceAssetDatabase>(std::move(cached));

            auto store = ProductStore::Open(project->GetProductDirectory());
            if (auto* error = std::get_if<AssetError>(&store))
                return std::move(*error);
            project->m_ProductStore = std::get<Scope<ProductStore>>(std::move(store));
            for (const auto& warning : project->m_ProductStore->GetWarnings())
                project->m_AssetWarnings.push_back(warning);

            auto scanned = project->ScanAssets();
            if (auto* error = std::get_if<AssetError>(&scanned))
                return std::move(*error);
            return project;
        }
        catch (const std::exception& error)
        {
            return AssetError {AssetErrorCode::IoError, std::string("Cannot open project: ") + error.what()};
        }
    }

    std::variant<std::monostate, AssetError> Project::RefreshAssets()
    {
        m_AssetWarnings.clear();
        return ScanAssets();
    }

    std::variant<std::monostate, AssetError> Project::ScanAssets()
    {
        auto scanned = AssetScanner::Scan(m_Config.AssetDirectory);
        if (auto* error = std::get_if<AssetError>(&scanned))
            return std::move(*error);
        auto& scan = std::get<AssetScanResult>(scanned);
        // Unreadable or conflicting files are skipped; the rest of the project stays usable.
        for (auto& error : scan.Errors)
            m_AssetWarnings.push_back(std::move(error));

        // Reconcile by identity: a source that disappeared stays registered as
        // Missing so references to it can be restored when it comes back.
        auto candidate = std::move(scan.Database);
        for (auto previous : m_SourceAssetDatabase.GetAllSources())
        {
            if (candidate.Contains(previous.sourceAssetID))
                continue;
            // A file that could not be read this time keeps its previous record.
            if (std::find(scan.Skipped.begin(), scan.Skipped.end(), previous.Path) != scan.Skipped.end())
            {
                auto kept = candidate.RegisterSource(previous);
                if (auto* error = std::get_if<AssetError>(&kept))
                    m_AssetWarnings.push_back(std::move(*error));
                continue;
            }
            if (candidate.Contains(previous.Path))
            {
                // A different file now occupies the path; the new identity wins.
                m_AssetWarnings.push_back({AssetErrorCode::DuplicatePath,
                                           previous.Path.ToString() + ": replaced by a file with a new identity; " +
                                               "references to the old source are now unresolved"});
                continue;
            }
            previous.Exists  = false;
            previous.Missing = true;
            auto registered  = candidate.RegisterSource(previous);
            if (auto* error = std::get_if<AssetError>(&registered))
                m_AssetWarnings.push_back(std::move(*error));
        }
        m_SourceAssetDatabase = std::move(candidate);
        auto saved            = SaveSourceCache();
        if (auto* error = std::get_if<AssetError>(&saved))
            m_AssetWarnings.push_back(std::move(*error));
        return std::monostate {};
    }

    std::variant<std::monostate, AssetError> Project::SaveAssetCache() const { return SaveSourceCache(); }

    std::variant<std::monostate, AssetError> Project::SaveSourceCache() const
    {
        return SourceAssetDatabaseSerializer::Save(
            m_SourceAssetCachePath, m_Config.AssetDirectory, m_SourceAssetDatabase);
    }

    fs::path Project::SourceFile(const SourceAssetMetadata& metadata) const
    {
        return (m_Config.AssetDirectory / metadata.Path.GetPath()).lexically_normal();
    }

    std::variant<SourceAssetID, AssetError> Project::FindSourceByFile(const fs::path& file) const
    {
        const AssetPath logical(file.lexically_normal().lexically_relative(m_Config.AssetDirectory));
        const auto      id       = logical.IsValid() ? m_SourceAssetDatabase.FindIDByPath(logical) : SourceAssetID {};
        const auto      metadata = m_SourceAssetDatabase.FindByID(id);
        if (!metadata.IsValid() || !metadata.Exists)
            return AssetError {AssetErrorCode::NotFound,
                               "Input file is not a source in the asset directory: " + file.u8string()};
        return id;
    }

    std::variant<std::monostate, AssetError> Project::SaveSidecar(const SourceAssetMetadata& metadata)
    {
        auto saved = SourceAssetMetaSerializer::Save(Sidecar(SourceFile(metadata)), metadata);
        if (auto* error = std::get_if<AssetError>(&saved))
            return std::move(*error);
        auto updated = m_SourceAssetDatabase.UpdateSource(metadata);
        if (auto* error = std::get_if<AssetError>(&updated))
            return std::move(*error);
        return std::monostate {};
    }

    std::variant<std::monostate, AssetError>
    Project::SetSourceImportConfiguration(SourceAssetID id, const SourceImportConfiguration& configuration)
    {
        if (!id.IsValid() || !configuration.IsValid())
            return AssetError {AssetErrorCode::InvalidArgument, "Invalid source import configuration"};
        auto metadata = m_SourceAssetDatabase.FindByID(id);
        if (!metadata.IsValid() || !metadata.Exists)
            return AssetError {AssetErrorCode::NotFound, "Source is unknown or Missing"};
        // The sidecar is authoritative; refuse to overwrite one that now names another source.
        auto loaded = SourceAssetMetaSerializer::Load(Sidecar(SourceFile(metadata)), metadata.Path);
        if (auto* error = std::get_if<AssetError>(&loaded))
            return std::move(*error);
        if (std::get<SourceAssetMetadata>(loaded).sourceAssetID != id)
            return AssetError {AssetErrorCode::DuplicateID, "Source sidecar identity changed; refresh first"};
        metadata.Import      = configuration;
        metadata.MetaVersion = 3;
        auto saved           = SaveSidecar(metadata);
        if (auto* error = std::get_if<AssetError>(&saved))
            return std::move(*error);
        auto cached = SaveSourceCache();
        if (auto* error = std::get_if<AssetError>(&cached))
            m_AssetWarnings.push_back(std::move(*error));
        return std::monostate {};
    }

    std::variant<AssetBuildGraphResult, AssetError> Project::BuildAsset(SourceAssetID            id,
                                                                        const AssetBuildOptions& options)
    {
        auto importers = CreateDefaultImporterRegistry();
        if (auto* error = std::get_if<AssetError>(&importers))
            return std::move(*error);
        return BuildAsset(id, options, std::get<ImporterRegistry>(importers));
    }

    std::variant<AssetBuildGraphResult, AssetError>
    Project::BuildAsset(SourceAssetID id, const AssetBuildOptions& options, const ImporterRegistry& importers)
    {
        BuildSession session {importers, options, {}, {}, {}, {}, {}, {}, {}};
        auto         built = BuildSource(id, session);
        if (auto* error = std::get_if<AssetError>(&built))
            return std::move(*error);
        AssetBuildGraphResult result;
        result.Root = std::get<AssetBuildResult>(std::move(built));
        session.Order.pop_back();
        result.Dependencies = std::move(session.Order);
        session.Order.clear();
        if (options.RebuildDependents)
        {
            // Walk the published reverse graph, retaining one session so each
            // source builds once and forward dependencies still build first.
            std::vector<SourceAssetID> pending {id};
            std::set<SourceAssetID>    visited {id};
            for (size_t index = 0; index < pending.size(); ++index)
            {
                auto& dependents = session.Dependents[pending[index]];
                dependents       = m_ProductStore->FindDependents(pending[index], options.Target);
                for (SourceAssetID dependent : dependents)
                    if (visited.insert(dependent).second)
                        pending.push_back(dependent);
            }
            for (size_t index = 1; index < pending.size(); ++index)
            {
                const auto source    = pending[index];
                auto       dependent = BuildSource(source, session);
                if (auto* error = std::get_if<AssetError>(&dependent))
                {
                    if (error->Code == AssetErrorCode::Cancelled)
                        return *error;
                    const auto path = m_SourceAssetDatabase.FindByID(source).Path.ToString();
                    if (const auto blocked = session.Blocked.find(source); blocked != session.Blocked.end())
                        result.Root.Warnings.push_back("Skipped dependent " + path + ": dependency " +
                                                       m_SourceAssetDatabase.FindByID(blocked->second).Path.ToString() +
                                                       " failed: " + session.Failed.at(blocked->second).Message);
                    else
                        result.Root.Warnings.push_back("Dependent build failed: " + path + ": " + error->Message);
                }
            }
            result.Dependents = std::move(session.Order);
        }
        // Existing editor callers consume Root.Registry; include all publications.
        result.Root.Registry = GetRuntimeAssetRegistry();
        auto cached         = SaveSourceCache();
        if (auto* error = std::get_if<AssetError>(&cached))
            m_AssetWarnings.push_back(std::move(*error));
        return result;
    }

    std::variant<AssetBuildGraphResult, AssetError> Project::BuildPublishedAssets(const AssetBuildOptions& options)
    {
        if (!IsProductTarget(options.Target))
            return AssetError {AssetErrorCode::InvalidArgument, "Invalid build target"};
        auto importers = CreateDefaultImporterRegistry();
        if (auto* error = std::get_if<AssetError>(&importers))
            return *error;
        BuildSession session {std::get<ImporterRegistry>(importers), options, {}, {}, {}, {}, {}, {}, {}};
        if (options.CancelRequested && options.CancelRequested())
            return AssetError {AssetErrorCode::Cancelled, "Published source check cancelled"};
        // Snapshot the roots before publishing anything. Dependencies discovered
        // during the session are still built recursively, with shared memoization.
        std::set<SourceAssetID> sources;
        for (const auto& record : GetRuntimeAssetRegistry()->GetAllProducts())
            if (record.Target == options.Target)
                sources.insert(record.Metadata.assetID.GetSourceID());
        // Preserve last-good dependent publications when a published dependency
        // fails, just as RebuildDependents does for a single-root session.
        for (auto source : sources)
            session.Dependents[source] = m_ProductStore->FindDependents(source, options.Target);
        AssetBuildGraphResult result;
        result.Root.Disposition = AssetBuildDisposition::UpToDate;
        for (auto source : sources)
        {
            if (options.CancelRequested && options.CancelRequested())
                return AssetError {AssetErrorCode::Cancelled, "Published source check cancelled"};
            auto built = BuildSource(source, session);
            if (auto* error = std::get_if<AssetError>(&built); error && error->Code == AssetErrorCode::Cancelled)
                return *error;
        }
        for (const auto& [source, error] : session.Failed)
        {
            const auto metadata = m_SourceAssetDatabase.FindByID(source);
            const auto path     = metadata.IsValid() ? metadata.Path.ToString() : source.ToString();
            result.Root.Warnings.push_back("Source build failed: " + path + ": " + error.Message);
        }
        for (const auto& [source, ancestor] : session.Blocked)
            if (sources.count(source) && !session.Done.count(source) && !session.Failed.count(source))
                result.Root.Warnings.push_back(
                    "Skipped dependent " + m_SourceAssetDatabase.FindByID(source).Path.ToString() + ": dependency " +
                    m_SourceAssetDatabase.FindByID(ancestor).Path.ToString() +
                    " failed: " + session.Failed.at(ancestor).Message);
        result.CheckedSources = std::move(session.Order);
        for (const auto& checked : result.CheckedSources)
        {
            if (checked.Disposition == AssetBuildDisposition::Published)
                result.Root.Disposition = AssetBuildDisposition::Published;
            result.Root.Warnings.insert(result.Root.Warnings.end(), checked.Warnings.begin(), checked.Warnings.end());
        }
        result.Root.Registry = GetRuntimeAssetRegistry();
        auto cached          = SaveSourceCache();
        if (auto* error = std::get_if<AssetError>(&cached))
            m_AssetWarnings.push_back(*error);
        return result;
    }

    std::variant<AssetBuildResult, AssetError> Project::BuildSource(SourceAssetID id, BuildSession& session)
    {
        if (session.Options.CancelRequested && session.Options.CancelRequested())
            return AssetError {AssetErrorCode::Cancelled, "Source build cancelled"};
        if (const auto done = session.Done.find(id); done != session.Done.end())
            return done->second;
        if (const auto failed = session.Failed.find(id); failed != session.Failed.end())
            return failed->second;
        if (session.Blocked.count(id))
            return AssetError {AssetErrorCode::InvalidData, "Dependency failed during propagation"};
        auto built = [&]() -> std::variant<AssetBuildResult, AssetError> {
            try
            {
                return BuildSourceOnce(id, session);
            }
            catch (const std::exception& error)
            {
                return AssetError {AssetErrorCode::IoError, error.what()};
            }
        }();
        if (const auto* error = std::get_if<AssetError>(&built); error && error->Code != AssetErrorCode::Cancelled)
        {
            session.Failed.emplace(id, *error);
            std::vector<SourceAssetID> pending {id};
            std::set<SourceAssetID>    visited {id};
            for (size_t index = 0; index < pending.size(); ++index)
                if (const auto found = session.Dependents.find(pending[index]); found != session.Dependents.end())
                    for (SourceAssetID dependent : found->second)
                        if (visited.insert(dependent).second)
                        {
                            session.Blocked.emplace(dependent, id);
                            pending.push_back(dependent);
                        }
        }
        return built;
    }

    std::variant<AssetBuildResult, AssetError> Project::BuildSourceOnce(SourceAssetID id, BuildSession& session)
    {
        if (session.Active.count(id))
        {
            std::string chain;
            for (auto source : session.Stack)
                chain += m_SourceAssetDatabase.FindByID(source).Path.ToString() + " (" + source.ToString() + ") -> ";
            chain += m_SourceAssetDatabase.FindByID(id).Path.ToString() + " (" + id.ToString() + ")";
            return AssetError {AssetErrorCode::InvalidData, "Source dependency cycle: " + chain};
        }
        auto metadata = m_SourceAssetDatabase.FindByID(id);
        if (!metadata.IsValid() || !metadata.Exists)
            return AssetError {AssetErrorCode::NotFound, "Source " + id.ToString() + " is unknown or Missing"};
        const auto file     = SourceFile(metadata);
        auto       selected = session.Importers.Resolve(file, metadata.Import.Importer);
        if (auto* error = std::get_if<AssetError>(&selected))
            return AssetError {error->Code, metadata.Path.ToString() + ": " + error->Message};

        AssetBuildInputs inputs;
        inputs.Source     = metadata;
        inputs.SourceFile = file;
        inputs.AssetRoot  = m_Config.AssetDirectory;
        inputs.Importer   = std::get<RegisteredImporter>(std::move(selected));
        // Identity from the sidecar, plus any allocated by a build whose sidecar
        // write did not complete.
        inputs.PreviousLocalIDs = metadata.LocalIDs;
        if (const auto stored = m_ProductStore->FindSource(id))
            inputs.PreviousLocalIDs.insert(stored->LocalIDs.begin(), stored->LocalIDs.end());

        ImportContext discovery;
        discovery.SourceID   = id;
        discovery.SourceFile = file;
        discovery.AssetRoot  = m_Config.AssetDirectory;
        discovery.Target     = session.Options.Target;
        discovery.Settings   = metadata.Import.Settings;
        auto discovered      = inputs.Importer.Importer->DiscoverInputs(discovery);
        if (auto* error = std::get_if<AssetError>(&discovered))
            return AssetError {error->Code, metadata.Path.ToString() + ": " + error->Message};
        auto found = std::get<ImportInputs>(std::move(discovered));

        session.Active.insert(id);
        session.Stack.push_back(id);
        struct ActiveGuard
        {
            std::set<SourceAssetID>& Active;
            SourceAssetID            ID;
            std::vector<SourceAssetID>& Stack;
            ~ActiveGuard()
            {
                Active.erase(ID);
                Stack.pop_back();
            }
        } guard {session.Active, id, session.Stack};
        inputs.SourceInputs.insert(found.BuiltinSourceFiles.begin(), found.BuiltinSourceFiles.end());
        for (const auto& input : found.SourceFiles)
        {
            auto source = FindSourceByFile(input);
            if (auto* error = std::get_if<AssetError>(&source))
                return std::move(*error);
            inputs.SourceInputs.emplace(input.lexically_normal(), std::get<SourceAssetID>(source));
        }
        for (const auto& input : found.ProductSources)
        {
            const auto resolve = [&]() -> std::variant<AssetID, AssetError> {
                auto source = FindSourceByFile(input);
                if (auto* error = std::get_if<AssetError>(&source))
                    return std::move(*error);
                const auto producer = std::get<SourceAssetID>(source);
                // Suggested settings apply only to sources nobody has configured.
                if (const auto settings = found.ProductSourceSettings.find(input);
                    settings != found.ProductSourceSettings.end())
                {
                    const auto current = m_SourceAssetDatabase.FindByID(producer);
                    if (current.Import.Importer.empty() && current.Import.Settings.empty())
                    {
                        auto configured = SetSourceImportConfiguration(producer, {"", settings->second});
                        if (auto* error = std::get_if<AssetError>(&configured))
                            m_AssetWarnings.push_back(std::move(*error));
                    }
                }
                auto built = BuildSource(producer, session);
                if (auto* error = std::get_if<AssetError>(&built))
                    return std::move(*error);
                const auto primary = PrimaryProduct(producer, std::get<AssetBuildResult>(built).LocalIDs);
                if (!primary.IsValid())
                    return AssetError {AssetErrorCode::NotFound,
                                       input.u8string() + " produced no referenceable Product"};
                return primary;
            };
            auto resolved = resolve();
            if (auto* error = std::get_if<AssetError>(&resolved))
            {
                if (!found.OptionalProductSources.count(input) || error->Code == AssetErrorCode::Cancelled)
                    return std::move(*error);
                found.Warnings.push_back("Optional Product source " + input.u8string() +
                                         " is unavailable: " + error->Message + "; leaving reference unassigned");
                // Do not use an older published Product after a failed rebuild.
                inputs.ProductsBySource.emplace(input.lexically_normal(), AssetID {});
                continue;
            }
            const auto primary = std::get<AssetID>(resolved);
            inputs.ProductsBySource.emplace(input.lexically_normal(), primary);
            if (std::find(inputs.ProductInputs.begin(), inputs.ProductInputs.end(), primary) ==
                inputs.ProductInputs.end())
                inputs.ProductInputs.push_back(primary);
        }
        for (AssetID product : found.Products)
        {
            auto built = BuildSource(product.GetSourceID(), session);
            if (auto* error = std::get_if<AssetError>(&built))
                return std::move(*error);
            if (!m_ProductStore->Snapshot()->Find(product, session.Options.Target))
                return AssetError {AssetErrorCode::NotFound,
                                   "Referenced Product " + product.ToString() + " is not produced by its source"};
            if (std::find(inputs.ProductInputs.begin(), inputs.ProductInputs.end(), product) ==
                inputs.ProductInputs.end())
                inputs.ProductInputs.push_back(product);
        }

        for (const auto& [product, type] : found.ProductsWithDependencies)
        {
            const auto label = type == AssetType::Material ? "Material parent " : "Required Product ";
            auto       built = BuildSource(product.GetSourceID(), session);
            if (const auto* error = std::get_if<AssetError>(&built))
                return AssetError {
                    error->Code, metadata.Path.ToString() + ": " + label + product.ToString() + ": " + error->Message};
            const auto registry = m_ProductStore->Snapshot();
            const auto record   = registry->Find(product, session.Options.Target);
            // Exposed UI asset overrides request a closure before their target property type is known.
            if (!record || (type != AssetType::None && record->Metadata.Type != type))
                return AssetError {AssetErrorCode::InvalidData,
                                   metadata.Path.ToString() + ": " + label + product.ToString() +
                                       " is unpublished or has the wrong Product type"};
            std::vector<AssetID> pending {product};
            std::set<AssetID>    visited;
            for (size_t index = 0; index < pending.size(); ++index)
            {
                const auto input = pending[index];
                if (!visited.insert(input).second)
                    continue;
                const auto dependency = registry->Find(input, session.Options.Target);
                if (!dependency)
                    return AssetError {AssetErrorCode::NotFound,
                                       std::string("Required dependency of ") + label + product.ToString() +
                                           " is unpublished: " + input.ToString()};
                if (std::find(inputs.ProductInputs.begin(), inputs.ProductInputs.end(), input) ==
                    inputs.ProductInputs.end())
                    inputs.ProductInputs.push_back(input);
                pending.insert(
                    pending.end(), dependency->Metadata.Dependencies.begin(), dependency->Metadata.Dependencies.end());
            }
        }

        for (const auto& [product, type] : found.OptionalProducts)
        {
            auto built = BuildSource(product.GetSourceID(), session);
            if (const auto* error = std::get_if<AssetError>(&built); error && error->Code == AssetErrorCode::Cancelled)
                return *error;
            const auto record = m_ProductStore->Snapshot()->Find(product, session.Options.Target);
            if (std::holds_alternative<AssetError>(built) || !record || record->Metadata.Type != type)
            {
                found.Warnings.push_back("Material remap " + product.ToString() +
                                         " is unavailable or not a Material; using embedded material");
                continue;
            }
            if (std::find(inputs.ProductInputs.begin(), inputs.ProductInputs.end(), product) ==
                inputs.ProductInputs.end())
                inputs.ProductInputs.push_back(product);
        }

        // A recursive input can fail while this source is active. In particular,
        // optional model remaps must not publish a fallback for a blocked branch.
        if (session.Blocked.count(id))
            return AssetError {AssetErrorCode::InvalidData, "Dependency failed during propagation"};
        auto built = AssetBuildService::Build(
            inputs, session.Options, *m_ProductStore, m_Digests, m_Config.CacheDirectory / "ImportWork");
        if (auto* error = std::get_if<AssetError>(&built))
            return AssetError {error->Code, metadata.Path.ToString() + ": " + error->Message};
        auto& result = std::get<AssetBuildResult>(built);
        result.Warnings.insert(result.Warnings.end(), found.Warnings.begin(), found.Warnings.end());
        for (auto& warning : result.Warnings)
        {
            warning = metadata.Path.ToString() + ": " + warning;
            m_AssetWarnings.push_back({AssetErrorCode::NotFound, warning});
        }
        if (result.LocalIDs != metadata.LocalIDs)
        {
            metadata.LocalIDs    = result.LocalIDs;
            metadata.MetaVersion = 3;
            auto saved           = SaveSidecar(metadata);
            if (auto* error = std::get_if<AssetError>(&saved))
                m_AssetWarnings.push_back(std::move(*error));
        }
        session.Done.emplace(id, result);
        session.Order.push_back(result);
        return result;
    }

    Ref<const AssetRegistry> Project::GetRuntimeAssetRegistry() const { return m_ProductStore->Snapshot(); }

    // ---- Source file operations -----------------------------------------------

    std::variant<std::monostate, AssetError> Project::MoveSourceAsset(SourceAssetID id, const AssetPath& destination)
    {
        const auto metadata = m_SourceAssetDatabase.FindByID(id);
        if (!metadata.IsValid() || !metadata.Exists)
            return AssetError {AssetErrorCode::NotFound, "Source is unknown or Missing"};
        if (!destination.IsValid() || !AssetScanner::IsDiscoverablePath(destination))
            return AssetError {AssetErrorCode::InvalidArgument, "Invalid move destination"};
        if (destination == metadata.Path)
            return std::monostate {};
        const auto      from = SourceFile(metadata);
        const auto      to   = (m_Config.AssetDirectory / destination.GetPath()).lexically_normal();
        std::error_code error;
        if (!fs::is_directory(to.parent_path(), error))
            return AssetError {AssetErrorCode::NotFound, "Move destination directory does not exist"};
        if (Exists(to) || Exists(Sidecar(to)) || m_SourceAssetDatabase.Contains(destination))
            return AssetError {AssetErrorCode::DuplicatePath, "Move destination already exists"};

        // The journal lets Open finish a move interrupted between the two renames.
        const auto journal = m_Config.CacheDirectory / "source-move.yaml";
        {
            YAML::Emitter out;
            out << YAML::BeginMap << YAML::Key << "from" << YAML::Value << metadata.Path.ToString() << YAML::Key << "to"
                << YAML::Value << destination.ToString() << YAML::EndMap;
            const std::string      text = out.c_str();
            std::vector<std::byte> bytes(text.size());
            std::memcpy(bytes.data(), text.data(), text.size());
            auto written = WriteProductFile(journal, bytes);
            if (auto* failure = std::get_if<AssetError>(&written))
                return std::move(*failure);
        }
        fs::rename(from, to, error);
        if (!error)
        {
            fs::rename(Sidecar(from), Sidecar(to), error);
            if (error)
            {
                std::error_code ignored;
                fs::rename(to, from, ignored);
            }
        }
        std::error_code ignored;
        fs::remove(journal, ignored);
        if (error)
            return AssetError {AssetErrorCode::IoError, "Cannot move source: " + error.message()};

        auto changed = m_SourceAssetDatabase.ChangeSourcePath(id, destination);
        if (auto* failure = std::get_if<AssetError>(&changed))
            return std::move(*failure);
        auto cached = SaveSourceCache();
        if (auto* failure = std::get_if<AssetError>(&cached))
            m_AssetWarnings.push_back(std::move(*failure));
        return std::monostate {};
    }

    std::variant<std::monostate, AssetError> Project::RecoverInterruptedMove()
    {
        const auto journal = m_Config.CacheDirectory / "source-move.yaml";
        if (!Exists(journal))
            return std::monostate {};
        try
        {
            const auto      node = YAML::LoadFile(journal.string());
            const AssetPath fromPath(fs::u8path(node["from"].as<std::string>()));
            const AssetPath toPath(fs::u8path(node["to"].as<std::string>()));
            if (fromPath.IsValid() && toPath.IsValid())
            {
                const auto from = m_Config.AssetDirectory / fromPath.GetPath();
                const auto to   = m_Config.AssetDirectory / toPath.GetPath();
                // The source moved but its sidecar did not: move the sidecar too.
                if (Exists(to) && !Exists(from) && Exists(Sidecar(from)) && !Exists(Sidecar(to)))
                    fs::rename(Sidecar(from), Sidecar(to));
            }
            fs::remove(journal);
            return std::monostate {};
        }
        catch (const std::exception& error)
        {
            return AssetError {AssetErrorCode::IoError,
                               std::string("Cannot recover an interrupted source move: ") + error.what()};
        }
    }

    std::variant<SourceAssetDeletionPlan, AssetError> Project::PlanSourceAssetDeletion(SourceAssetID id) const
    {
        SourceAssetDeletionPlan plan;
        plan.Source = m_SourceAssetDatabase.FindByID(id);
        if (!plan.Source.IsValid())
            return AssetError {AssetErrorCode::NotFound, "Source is unknown"};
        for (SourceAssetID dependent : m_ProductStore->FindDependents(id))
            if (dependent != id)
                plan.Dependents.push_back(m_SourceAssetDatabase.FindByID(dependent));
        return plan;
    }

    std::variant<Ref<const AssetRegistry>, AssetError> Project::DeleteSourceAsset(SourceAssetID id)
    {
        const auto metadata = m_SourceAssetDatabase.FindByID(id);
        if (!metadata.IsValid())
            return AssetError {AssetErrorCode::NotFound, "Source is unknown"};
        // Unpublish first: if deleting the files then fails, the source is rebuilt on demand.
        auto removed = m_ProductStore->RemoveSource(id);
        if (auto* error = std::get_if<AssetError>(&removed))
            return std::move(*error);
        const auto      file = SourceFile(metadata);
        std::error_code error;
        fs::remove(file, error);
        if (!error)
            fs::remove(Sidecar(file), error);
        if (error)
            return AssetError {AssetErrorCode::IoError,
                               "Products were removed but the source file could not be: " + error.message()};
        auto unregistered = m_SourceAssetDatabase.UnregisterSource(id);
        if (auto* failure = std::get_if<AssetError>(&unregistered))
            return std::move(*failure);
        auto cached = SaveSourceCache();
        if (auto* failure = std::get_if<AssetError>(&cached))
            m_AssetWarnings.push_back(std::move(*failure));
        return removed;
    }
} // namespace Kans
