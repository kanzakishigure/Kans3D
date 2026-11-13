#pragma once

#include <condition_variable>
#include <deque>
#include <mutex>
#include <optional>
#include <thread>
#include <type_traits>
#include <unordered_map>
#include "Kans3D/Asset/AssetRegistry.h"
#include "Kans3D/Asset/Loader/AssetLoaderRegistry.h"
#include "Kans3D/Asset/Storage/AssetProvider.h"

namespace Kans
{
    enum class AssetLoadState
    {
        Loading,
        Ready,
        Failed
    };

    template<typename T>
    struct AssetLoadStatus
    {
        AssetLoadState            State = AssetLoadState::Loading;
        Ref<const T>              Asset;
        std::optional<AssetError> Error;

        bool IsReady() const { return State == AssetLoadState::Ready; }
    };

    struct AssetManagerSpecification
    {
        std::string         Target      = "default";
        uint32_t            WorkerCount = 2;
        AssetLoaderRegistry Loaders     = AssetLoaderRegistry::CreateDefault();
    };

    // Each observer drains its own queue on the scene thread. Workers only enqueue IDs.
    class AssetChangeObserver
    {
    public:
        std::vector<AssetID> Drain();

    private:
        std::mutex           m_Mutex;
        std::vector<AssetID> m_Changes;
        friend class AssetManager;
    };

    // Runtime CPU asset cache. Resolves AssetIDs through a Registry snapshot, reads
    // Product bytes from a provider and decodes them. A source-aware provider may
    // repair stale products through its normal import path.
    //
    // Loaded assets are immutable and shared: a returned Ref stays valid after the
    // asset is unloaded or replaced. Each AssetID is loaded at most once at a time.
    class AssetManager
    {
    public:
        AssetManager(Ref<IAssetProvider>       provider,
                     Ref<const AssetRegistry>  registry,
                     AssetManagerSpecification spec = {});
        ~AssetManager();
        AssetManager(const AssetManager&)            = delete;
        AssetManager& operator=(const AssetManager&) = delete;

        // Blocking load retries transient failures. Engine-contract failures remain
        // cached until Registry invalidation or Unload.
        template<typename T>
        std::variant<Ref<const T>, AssetError> Load(AssetID id)
        {
            auto result = LoadBlocking(id, T::GetStaticType());
            if (auto* error = std::get_if<AssetError>(&result))
                return std::move(*error);
            return std::static_pointer_cast<const T>(std::get<Ref<const Asset>>(std::move(result)));
        }

        // Non-blocking load for per-frame polling. Queues the asset on first use;
        // a failure is reported until the Registry changes or the asset is unloaded.
        template<typename T>
        AssetLoadStatus<T> Request(AssetID id)
        {
            auto               status = RequestUntyped(id, T::GetStaticType());
            AssetLoadStatus<T> result;
            result.State = status.State;
            result.Asset = std::static_pointer_cast<const T>(status.Asset);
            result.Error = std::move(status.Error);
            return result;
        }

        // Returns the loaded asset without starting a load.
        template<typename T>
        Ref<const T> GetLoaded(AssetID id) const
        {
            return std::static_pointer_cast<const T>(GetLoadedUntyped(id, T::GetStaticType()));
        }

        // Replace the Registry. Cached assets whose Product content changed or was
        // removed are evicted, so the next request loads the new version.
        [[nodiscard]] std::variant<ProductChangeSet, AssetError> SetRegistry(Ref<const AssetRegistry> registry);
        Ref<const AssetRegistry>                                 GetRegistry() const;
        const std::string&                                       GetTarget() const { return m_Target; }

        std::shared_ptr<AssetChangeObserver> ObserveChanges();
        void   Unload(AssetID id);
        size_t CollectUnused();
        size_t GetResidentCount() const;

    private:
        void                                            NotifyChange(AssetID id); // m_Mutex held
        std::vector<std::weak_ptr<AssetChangeObserver>> m_Observers;
        struct Entry
        {
            AssetLoadState            State = AssetLoadState::Loading;
            AssetType                 Type  = AssetType::None;
            Ref<const Asset>          Value;
            std::optional<AssetError> Error;
            uint64_t                  Ticket = 0;
        };

        std::variant<Ref<const Asset>, AssetError> LoadBlocking(AssetID id, AssetType type);
        AssetLoadStatus<Asset>                     RequestUntyped(AssetID id, AssetType type);
        Ref<const Asset>                           GetLoadedUntyped(AssetID id, AssetType type) const;
        // Starts a load under m_Mutex; returns the ticket identifying this attempt.
        uint64_t                                   BeginLoad(AssetID id, AssetType type);
        void                                       RunLoad(AssetID id, uint64_t ticket);
        std::variant<Ref<const Asset>, AssetError> ReadAndDecode(AssetID id, AssetType type);
        void                                       WorkerLoop();

        Ref<IAssetProvider>       m_Provider;
        const std::string         m_Target;
        const AssetLoaderRegistry m_Loaders;

        mutable std::mutex                       m_Mutex;
        std::condition_variable                  m_Changed;
        Ref<const AssetRegistry>                 m_Registry;
        std::unordered_map<AssetID, Entry>       m_Entries;
        std::deque<std::pair<AssetID, uint64_t>> m_Queue;
        std::vector<std::thread>                 m_Workers;
        uint64_t                                 m_NextTicket = 0;
        bool                                     m_Stopping   = false;
    };
} // namespace Kans
