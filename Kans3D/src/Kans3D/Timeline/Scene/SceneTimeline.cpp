#include "kspch.h"
#include "Kans3D/Asset/Manager/AssetManager.h"
#include "Kans3D/Scene/Entity.h"
namespace Kans
{
    bool Scene::IsEntityActive(Entity e) const
    {
        for (auto current = e; current; current = GetParent(current))
            if (current.HasComponent<ActiveComponent>() && !current.GetComponent<ActiveComponent>().Active)
                return false;
        return bool(e);
    }
    void Scene::SetEntityActive(Entity e, bool active)
    {
        if (!e)
            return;
        if (e.HasComponent<ActiveComponent>())
        {
            if (e.GetComponent<ActiveComponent>().Active == active)
                return;
            e.PatchComponent<ActiveComponent>([&](auto& c) { c.Active = active; });
        }
        else
            e.AddComponent<ActiveComponent>(ActiveComponent {active});
    }
    bool Scene::PrepareTimeline(Entity owner)
    {
        if (!owner || !owner.HasComponent<TimelinePlayerComponent>())
            return false;
        auto& c = owner.GetComponent<TimelinePlayerComponent>();
        if (c.Player)
            return true;
        if (!c.Source && m_PrefabAssets && c.Timeline.IsValid())
        {
            auto loaded = m_PrefabAssets->Load<TimelineAsset>(c.Timeline);
            if (auto source = std::get_if<Ref<const TimelineAsset>>(&loaded))
                c.Source = *source;
        }
        if (!c.Source)
            return false;
        c.Player = CreateRef<TimelinePlayer>(*this, owner.GetUUID(), c.Source);
        return true;
    }
    void Scene::UpdateTimelines(double seconds)
    {
        // Evaluation can instantiate prefabs and mutate ECS storage. Iterate stable UUIDs.
        std::vector<UUID> owners;
        for (auto h : m_Registry.view<TimelinePlayerComponent>())
            owners.push_back(m_Registry.get<IDComponent>(h).ID);
        for (auto id : owners)
            if (auto e = GetEntityByUUID(id); e && PrepareTimeline(e))
            {
                auto player = e.GetComponent<TimelinePlayerComponent>().Player;
                player->Advance(seconds);
            }
    }
} // namespace Kans
