#pragma once
#include <mutex>
#include <optional>
#include <unordered_map>
#include "Kans3D/Asset/SourceAssetMetaData.h"
#include "Kans3D/Asset/Storage/ProductStore.h"
#include "ImporterRegistry.h"

namespace Kans
{
    struct AssetSourceProgress
    {
        SourceAssetID             Source;
        std::string               Path; // Relative to the project asset directory.
        uint64_t                  Completed = 0, Total = 0;
        bool                      Finished = false;
        std::optional<AssetError> Error;
    };

    struct AssetBuildOptions
    {
        // Identifies the tool version producing Products; part of the build key.
        std::string Toolchain = "Kans3D";
        std::string Target    = "default";
        // Import even if the build key matches the published build.
        bool                                  Force = false;
        std::function<bool(PublicationPhase)> Progress;
        // Project coordinator: also build published dependents of the root,
        // transitively, for Target. Unrelated sources are never selected.
        bool RebuildDependents = false;
        // Checked between sources/dependencies, including fingerprint cache hits.
        std::function<bool()> CancelRequested;
        // Project coordinator; called on the build thread. Totals grow when new
        // dependencies are discovered; each source is counted once, including failures.
        std::function<void(const AssetSourceProgress&)> SourceProgress;
    };

    enum class AssetBuildDisposition
    {
        Published,
        UpToDate
    };

    struct AssetBuildResult
    {
        SourceAssetID            Source;
        AssetBuildDisposition    Disposition = AssetBuildDisposition::Published;
        std::string              Fingerprint;
        Ref<const AssetRegistry> Registry;
        // Complete object key -> LocalID mapping after the build.
        LocalIDMap LocalIDs;
        // Problems that did not stop the build.
        std::vector<std::string> Warnings;
    };

    // Content digests of files, reused while a file's size and write time are unchanged.
    class FileDigestCache
    {
    public:
        [[nodiscard]] std::variant<std::string, AssetError> Digest(const std::filesystem::path& file);

    private:
        struct Entry
        {
            uintmax_t                       Size = 0;
            std::filesystem::file_time_type Time;
            std::string                     Digest;
        };
        std::mutex                             m_Mutex;
        std::unordered_map<std::string, Entry> m_Entries;
    };

    // One source build with all inputs already resolved by the coordinator.
    struct AssetBuildInputs
    {
        SourceAssetMetadata   Source;
        std::filesystem::path SourceFile; // Absolute.
        std::filesystem::path AssetRoot;  // Absolute.
        RegisteredImporter    Importer;
        LocalIDMap            PreviousLocalIDs;
        // Other source files the import reads.
        std::map<std::filesystem::path, SourceAssetID> SourceInputs;
        // Path-referenced sources -> their published primary Product.
        std::map<std::filesystem::path, AssetID> ProductsBySource;
        // Every Product of another source the import uses (published for the target).
        std::vector<AssetID> ProductInputs;
    };

    class AssetBuildService
    {
    public:
        // Imports the source unless its build key matches the published build, then
        // publishes the complete Product set. Inputs that change while importing
        // fail the build instead of publishing mixed versions.
        [[nodiscard]] static std::variant<AssetBuildResult, AssetError> Build(const AssetBuildInputs&      inputs,
                                                                              const AssetBuildOptions&     options,
                                                                              ProductStore&                store,
                                                                              FileDigestCache&             digests,
                                                                              const std::filesystem::path& workRoot);
    };
} // namespace Kans
