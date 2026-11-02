#include "kspch.h"
#include "AssetManager.h"
#include "Kans3D/Asset/Runtime/MeshData.h"
#include "Kans3D/Asset/Runtime/ModelData.h"
#include "Kans3D/Asset/Runtime/StandardShader.h"

namespace Kans
{
    std::vector<AssetID> AssetChangeObserver::Drain()
    {
        std::lock_guard<std::mutex> lock(m_Mutex);
        std::vector<AssetID>        result;
        result.swap(m_Changes);
        return result;
    }
    std::shared_ptr<AssetChangeObserver> AssetManager::ObserveChanges()
    {
        auto                        observer = std::make_shared<AssetChangeObserver>();
        std::lock_guard<std::mutex> lock(m_Mutex);
        m_Observers.push_back(observer);
        return observer;
    }
    void AssetManager::NotifyChange(AssetID id)
    {
        for (auto it = m_Observers.begin(); it != m_Observers.end();)
            if (auto observer = it->lock())
            {
                std::lock_guard<std::mutex> lock(observer->m_Mutex);
                observer->m_Changes.push_back(id);
                ++it;
            }
            else
                it = m_Observers.erase(it);
    }

    AssetManager::AssetManager(Ref<IAssetProvider>       provider,
                               Ref<const AssetRegistry>  registry,
                               AssetManagerSpecification spec) :
        m_Provider(std::move(provider)), m_Target(std::move(spec.Target)), m_Loaders(std::move(spec.Loaders)),
        m_Registry(std::move(registry))
    {
        CORE_ASSERT(m_Provider, "AssetManager requires a Product provider");
        if (!m_Registry)
            m_Registry = CreateRef<const AssetRegistry>();
        const uint32_t workers = std::max(1u, spec.WorkerCount);
        for (uint32_t i = 0; i < workers; ++i)
            m_Workers.emplace_back([this] { WorkerLoop(); });
    }

    AssetManager::~AssetManager()
    {
        {
            std::lock_guard<std::mutex> lock(m_Mutex);
            m_Stopping = true;
            m_Queue.clear();
        }
        m_Changed.notify_all();
        for (auto& worker : m_Workers)
            worker.join();
    }

    uint64_t AssetManager::BeginLoad(AssetID id, AssetType type)
    {
        auto& entry  = m_Entries[id];
        entry        = {};
        entry.Type   = type;
        entry.Ticket = ++m_NextTicket;
        return entry.Ticket;
    }

    std::variant<Ref<const Asset>, AssetError> AssetManager::ReadAndDecode(AssetID id, AssetType type)
    {
        if (id == StandardShaderID() || id == SixWaySmokeShaderID())
        {
            if (type != AssetType::Shader)
                return AssetError {AssetErrorCode::InvalidArgument,
                                   id == StandardShaderID() ? "Standard is a Shader asset" :
                                                              "Kans3D/SixWaySmoke is a Shader asset"};
            try
            {
                return Ref<const Asset>(id == StandardShaderID() ? GetStandardShaderAsset() :
                                                                   GetSixWaySmokeShaderAsset());
            }
            catch (const std::exception& error)
            {
                return AssetError {AssetErrorCode::InvalidData, error.what()};
            }
        }
        Ref<const AssetRegistry> registry = GetRegistry();
        const auto               record   = registry->Find(id, m_Target);
        if (!record && type == AssetType::StaticMesh)
        {
            // The editor requests old part IDs after loading their ModelAsset.
            // These are views, never additional published geometry Products.
            std::optional<ModelSectionIdentity> part;
            Ref<const MeshAsset>                source;
            {
                std::lock_guard<std::mutex> lock(m_Mutex);
                for (const auto& [modelID, entry] : m_Entries)
                    if (entry.Type == AssetType::Model && entry.Value)
                    {
                        const auto model = std::static_pointer_cast<const ModelAsset>(entry.Value);
                        for (const auto& identity : model->GetMeshData().SectionIdentities)
                            if (identity.OldMesh == id)
                            {
                                part = identity;
                                break;
                            }
                        if (part)
                            break;
                    }
                if (part)
                {
                    const auto loaded = m_Entries.find(part->Mesh);
                    if (loaded != m_Entries.end() && loaded->second.Type == AssetType::StaticMesh &&
                        loaded->second.Value)
                        source = std::static_pointer_cast<const MeshAsset>(loaded->second.Value);
                }
            }
            if (part && part->Mesh != id)
            {
                if (!source)
                {
                    auto loaded = ReadAndDecode(part->Mesh, type);
                    if (auto* error = std::get_if<AssetError>(&loaded))
                        return *error;
                    source = std::static_pointer_cast<const MeshAsset>(std::get<Ref<const Asset>>(loaded));
                    // Share the decoded group with subsequent compatibility requests.
                    // Do not replace an independently loading canonical asset.
                    {
                        std::lock_guard<std::mutex> lock(m_Mutex);
                        if (m_Registry == registry && !m_Entries.count(part->Mesh))
                        {
                            BeginLoad(part->Mesh, type);
                            auto& entry = m_Entries.at(part->Mesh);
                            entry.State = AssetLoadState::Ready;
                            entry.Value = source;
                            NotifyChange(part->Mesh);
                        }
                    }
                    m_Changed.notify_all();
                }
                const auto& shared = source->GetData();
                if (part->Section >= shared.Sections.size())
                    return AssetError {AssetErrorCode::InvalidData, "Model part refers to a missing mesh section"};
                const auto& section = shared.Sections[part->Section];
                const auto& slot    = shared.MaterialSlots[section.MaterialSlot];
                MeshData    view;
                view.VertexFormat     = shared.VertexFormat;
                view.Skeleton          = shared.Skeleton;
                view.JointBounds       = shared.JointBounds;
                view.SkinBindTransform = shared.SkinBindTransform;
                view.Material         = slot.DefaultMaterial;
                view.AuthoredMaterial = slot.AuthoredMaterial;
                view.MaterialSlots    = {slot};
                view.LocalBounds      = section.LocalBounds;
                view.Sections         = {{0, section.IndexCount, 0, section.LocalBounds}};
                std::unordered_map<uint32_t, uint32_t> vertices;
                for (uint32_t i = section.FirstIndex; i < section.FirstIndex + section.IndexCount; ++i)
                {
                    const auto index  = shared.Indices[i];
                    const auto mapped = vertices.emplace(index, static_cast<uint32_t>(view.Vertices.size()));
                    if (mapped.second)
                    {
                        view.Vertices.push_back(shared.Vertices[index]);
                        if (!shared.Skin.empty())
                            view.Skin.push_back(shared.Skin[index]);
                    }
                    view.Indices.push_back(mapped.first->second);
                }
                auto asset     = CreateRef<MeshAsset>(std::move(view));
                asset->assetID = id;
                return Ref<const Asset>(std::move(asset));
            }
        }
        if (!record)
            return AssetError {AssetErrorCode::NotFound, "Asset " + id.ToString() + " is not in the Registry"};
        if (record->Metadata.Type != type)
            return AssetError {AssetErrorCode::InvalidArgument,
                               "Asset " + id.ToString() + " has a different type than requested"};
        const auto* decoder = m_Loaders.Find(type);
        if (!decoder)
            return AssetError {AssetErrorCode::NotImplemented, "No runtime loader for asset type"};
        try
        {
            auto bytes = m_Provider->Read(*record);
            if (auto* error = std::get_if<AssetError>(&bytes))
                return std::move(*error);
            const auto&      data = std::get<std::vector<std::byte>>(bytes);
            AssetLoadContext context {id, type, data.data(), data.size()};
            auto             decoded = (*decoder)(context);
            if (auto* error = std::get_if<AssetError>(&decoded))
                return std::move(*error);
            auto asset = std::get<Ref<Asset>>(std::move(decoded));
            if (!asset || asset->assetID != id || asset->GetAssetType() != type)
                return AssetError {AssetErrorCode::InvalidData, "Loader returned an asset with the wrong identity"};
            return Ref<const Asset>(std::move(asset));
        }
        catch (const std::exception& error)
        {
            return AssetError {AssetErrorCode::IoError, std::string("Asset load failed: ") + error.what()};
        }
    }

    void AssetManager::RunLoad(AssetID id, uint64_t ticket)
    {
        AssetType type;
        {
            std::lock_guard<std::mutex> lock(m_Mutex);
            const auto                  found = m_Entries.find(id);
            if (found == m_Entries.end() || found->second.Ticket != ticket)
                return;
            type = found->second.Type;
        }
        auto             result = ReadAndDecode(id, type);
        Ref<const Asset> discarded;
        {
            std::lock_guard<std::mutex> lock(m_Mutex);
            const auto                  found = m_Entries.find(id);
            // The entry was evicted or restarted while loading; drop this result.
            if (found == m_Entries.end() || found->second.Ticket != ticket)
            {
                if (auto* value = std::get_if<Ref<const Asset>>(&result))
                    discarded = std::move(*value);
            }
            else if (auto* error = std::get_if<AssetError>(&result))
            {
                found->second.State = AssetLoadState::Failed;
                found->second.Error = std::move(*error);
            }
            else
            {
                found->second.State = AssetLoadState::Ready;
                found->second.Value = std::get<Ref<const Asset>>(std::move(result));
                NotifyChange(id);
            }
        }
        m_Changed.notify_all();
    }

    std::variant<Ref<const Asset>, AssetError> AssetManager::LoadBlocking(AssetID id, AssetType type)
    {
        if (!id.IsValid())
            return AssetError {AssetErrorCode::InvalidArgument, "Invalid asset ID"};
        uint64_t                     attempted = 0;
        std::unique_lock<std::mutex> lock(m_Mutex);
        while (true)
        {
            auto found = m_Entries.find(id);
            if (found != m_Entries.end())
            {
                auto& entry = found->second;
                // A failed entry of another type was a bad request; reload with this type.
                if (entry.Type != type && entry.State != AssetLoadState::Failed)
                    return AssetError {AssetErrorCode::InvalidArgument,
                                       "Asset " + id.ToString() + " has a different type than requested"};
                if (entry.State == AssetLoadState::Ready)
                    return entry.Value;
                if (entry.State == AssetLoadState::Loading)
                {
                    m_Changed.wait(lock);
                    continue;
                }
                if (entry.Ticket == attempted && entry.Type == type)
                    return *entry.Error;
            }
            attempted = BeginLoad(id, type);
            lock.unlock();
            RunLoad(id, attempted);
            lock.lock();
        }
    }

    AssetLoadStatus<Asset> AssetManager::RequestUntyped(AssetID id, AssetType type)
    {
        AssetLoadStatus<Asset> status;
        if (!id.IsValid())
        {
            status.State = AssetLoadState::Failed;
            status.Error = AssetError {AssetErrorCode::InvalidArgument, "Invalid asset ID"};
            return status;
        }
        {
            std::lock_guard<std::mutex> lock(m_Mutex);
            const auto                  found = m_Entries.find(id);
            if (found == m_Entries.end() ||
                (found->second.Type != type && found->second.State == AssetLoadState::Failed))
            {
                m_Queue.emplace_back(id, BeginLoad(id, type));
            }
            else if (found->second.Type != type)
            {
                status.State = AssetLoadState::Failed;
                status.Error = AssetError {AssetErrorCode::InvalidArgument,
                                           "Asset " + id.ToString() + " has a different type than requested"};
                return status;
            }
            else
            {
                status.State = found->second.State;
                status.Asset = found->second.Value;
                status.Error = found->second.Error;
                return status;
            }
        }
        m_Changed.notify_all();
        return status;
    }

    Ref<const Asset> AssetManager::GetLoadedUntyped(AssetID id, AssetType type) const
    {
        std::lock_guard<std::mutex> lock(m_Mutex);
        const auto                  found = m_Entries.find(id);
        if (found == m_Entries.end() || found->second.Type != type)
            return nullptr;
        return found->second.Value;
    }

    void AssetManager::WorkerLoop()
    {
        std::unique_lock<std::mutex> lock(m_Mutex);
        while (true)
        {
            m_Changed.wait(lock, [this] { return m_Stopping || !m_Queue.empty(); });
            if (m_Stopping)
                return;
            const auto [id, ticket] = m_Queue.front();
            m_Queue.pop_front();
            lock.unlock();
            RunLoad(id, ticket);
            lock.lock();
        }
    }

    std::variant<ProductChangeSet, AssetError> AssetManager::SetRegistry(Ref<const AssetRegistry> registry)
    {
        if (!registry)
            return AssetError {AssetErrorCode::InvalidArgument, "Cannot set a null Registry"};
        std::vector<Ref<const Asset>> retired;
        ProductChangeSet              changes;
        {
            std::lock_guard<std::mutex> lock(m_Mutex);
            auto                        compared = CompareAssetRegistries(*m_Registry, *registry, m_Target);
            if (auto* error = std::get_if<AssetError>(&compared))
                return std::move(*error);
            changes    = std::get<ProductChangeSet>(std::move(compared));
            auto evict = [&](AssetID id) {
                NotifyChange(id);
                const auto found = m_Entries.find(id);
                if (found == m_Entries.end())
                    return;
                retired.push_back(std::move(found->second.Value));
                m_Entries.erase(found);
            };
            for (AssetID id : changes.Modified)
                evict(id);
            for (AssetID id : changes.Removed)
                evict(id);
            // Failures may have been caused by the previous Registry; allow a retry.
            for (auto it = m_Entries.begin(); it != m_Entries.end();)
            {
                // Part views have no Registry entry. Their bindings may change on reimport.
                if (it->second.State == AssetLoadState::Failed ||
                    (it->first != StandardShaderID() && it->first != SixWaySmokeShaderID() &&
                     !registry->Find(it->first, m_Target)))
                {
                    NotifyChange(it->first);
                    retired.push_back(std::move(it->second.Value));
                    it = m_Entries.erase(it);
                }
                else
                    ++it;
            }
            m_Registry = std::move(registry);
        }
        m_Changed.notify_all();
        return changes;
    }

    Ref<const AssetRegistry> AssetManager::GetRegistry() const
    {
        std::lock_guard<std::mutex> lock(m_Mutex);
        return m_Registry;
    }

    void AssetManager::Unload(AssetID id)
    {
        Ref<const Asset> retired;
        {
            std::lock_guard<std::mutex> lock(m_Mutex);
            const auto                  found = m_Entries.find(id);
            if (found == m_Entries.end())
                return;
            NotifyChange(id);
            retired = std::move(found->second.Value);
            m_Entries.erase(found);
        }
        m_Changed.notify_all();
    }

    size_t AssetManager::CollectUnused()
    {
        std::vector<Ref<const Asset>> retired;
        std::lock_guard<std::mutex>   lock(m_Mutex);
        for (auto it = m_Entries.begin(); it != m_Entries.end();)
        {
            if (it->second.State == AssetLoadState::Ready && it->second.Value.use_count() == 1)
            {
                retired.push_back(std::move(it->second.Value));
                it = m_Entries.erase(it);
            }
            else
                ++it;
        }
        return retired.size();
    }

    size_t AssetManager::GetResidentCount() const
    {
        std::lock_guard<std::mutex> lock(m_Mutex);
        size_t                      count = 0;
        for (const auto& [id, entry] : m_Entries)
            count += entry.State == AssetLoadState::Ready;
        return count;
    }
} // namespace Kans
