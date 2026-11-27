#pragma once
#include <functional>
#include <mutex>
#include "Kans3D/Asset/AssetRegistry.h"
#include "Kans3D/Asset/Importer/ImportContext.h"
#include "Kans3D/Core/Debug/Profiler.h"
#include "ProductFiles.h"

namespace Kans
{
    enum class PublicationPhase
    {
        Validated,
        ProductsWritten,
        BeforeCommit,
        Committed
    };

    // The last successful build of one source for one target.
    struct ProductBuildRecord
    {
        // Directory below the store root holding this build's Product files.
        std::string                Generation;
        std::string                Fingerprint;
        std::vector<SourceAssetID> SourceInputs;  // Other source files read by the import.
        std::vector<AssetID>       ProductInputs; // Products of other sources the import used.
        std::vector<ProductRecord> Products;
    };

    struct SourceProducts
    {
        // Every object key ever imported from the source, including removed objects.
        LocalIDMap                                LocalIDs;
        std::map<std::string, ProductBuildRecord> Targets;
    };

    struct PublishRequest
    {
        SourceAssetID              Source;
        std::string                Target = "default";
        std::string                Fingerprint;
        std::filesystem::path      StagingDirectory;
        ImportResult               Imported;
        std::vector<SourceAssetID> SourceInputs;
        std::vector<AssetID>       ProductInputs;
        // Returning false before the commit cancels the publication.
        std::function<bool(PublicationPhase)> Progress;
    };

    // Persistent Product storage for one project.
    //
    // Each publication writes a complete Product set for one source and target
    // into a new immutable generation directory, then atomically replaces the
    // catalog. A failed or cancelled publication leaves the previous set
    // published. Old generations are deleted once nothing in this process
    // references them (ProductRecord::Location::Generation).
    //
    // The catalog is a rebuildable cache: if it is unreadable the store starts
    // empty and sources are imported again.
    class ProductStore
    {
    public:
        [[nodiscard]] static std::variant<Scope<ProductStore>, AssetError> Open(const std::filesystem::path& root);
        ~ProductStore();

        Ref<const AssetRegistry>          Snapshot() const;
        std::optional<SourceProducts>     FindSource(SourceAssetID source) const;
        std::optional<ProductBuildRecord> FindBuild(SourceAssetID source, const std::string& target) const;
        // Sources with builds that read `source` or use one of its Products.
        // Empty target considers every target (deletion planning).
        std::vector<SourceAssetID>     FindDependents(SourceAssetID source, const std::string& target = {}) const;
        const std::filesystem::path&   GetRoot() const { return m_Root; }
        const std::vector<AssetError>& GetWarnings() const { return m_Warnings; }

        [[nodiscard]] std::variant<Ref<const AssetRegistry>, AssetError> Publish(const PublishRequest& request);
        // Unpublishes every Product of the source. Its LocalIDs stay reserved.
        [[nodiscard]] std::variant<Ref<const AssetRegistry>, AssetError> RemoveSource(SourceAssetID source);
        // Deletes generation directories that are neither published nor in use.
        size_t CollectGarbage();

    private:
        struct Catalog
        {
            std::map<SourceAssetID, SourceProducts> Sources;
            Ref<const AssetRegistry>                Registry = CreateRef<const AssetRegistry>();
        };

        ProductStore() = default;
        std::variant<std::monostate, AssetError> Commit(Ref<const Catalog> catalog);
        Ref<const Catalog>                       Current() const;

        std::filesystem::path   m_Root;
        Scope<ProductStoreLock> m_WriterLock;
        std::vector<AssetError> m_Warnings;
        mutable KANS_PROFILE_LOCKABLE(std::mutex, m_Mutex, "Asset product registry");
        Ref<const Catalog>      m_Catalog;
    };
} // namespace Kans
