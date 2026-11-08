#include "HierarchyVisibility.h"
#include <algorithm>
#include "Kans3D/Scene/Components.h"
#include "Kans3D/Scene/Scene.h"
#include "KansEditorCore/EditorVisibilityStore.h"

namespace Kans::HierarchyVisibility
{
    namespace
    {
        void ClearSubtree(EditorVisibilityStore& store, const Scene& scene, Entity entity)
        {
            store.SetHidden(entity.GetUUID(), false);
            for (Entity child : scene.GetChildren(entity))
                ClearSubtree(store, scene, child);
        }

        // Shows a hidden entity, or one hidden through its ancestors, with its children.
        void Show(EditorVisibilityStore& store, const Scene& scene, Entity entity)
        {
            std::vector<Entity> path; // Root first, the entity last.
            for (Entity e = entity; e; e = scene.GetParent(e))
                path.push_back(e);
            std::reverse(path.begin(), path.end());
            bool below = false; // Under a hidden ancestor: whatever is off the path stays hidden.
            for (size_t i = 0; i + 1 < path.size(); ++i)
            {
                if (store.IsHidden(path[i].GetUUID()))
                {
                    store.SetHidden(path[i].GetUUID(), false);
                    below = true;
                }
                if (below)
                    for (Entity child : scene.GetChildren(path[i]))
                        if (child != path[i + 1])
                            store.SetHidden(child.GetUUID(), true);
            }
            ClearSubtree(store, scene, entity);
        }
    } // namespace

    Eye Resolve(const Scene& scene, const std::unordered_set<UUID>& hidden, Entity entity)
    {
        if (!entity)
            return Eye::Visible;
        if (hidden.count(entity.GetUUID()))
            return Eye::Hidden;
        for (Entity parent = scene.GetParent(entity); parent; parent = scene.GetParent(parent))
            if (hidden.count(parent.GetUUID()))
                return Eye::HiddenByParent;
        return Eye::Visible;
    }

    bool IsHidden(const Scene& scene, const std::unordered_set<UUID>& hidden, Entity entity)
    {
        return Resolve(scene, hidden, entity) != Eye::Visible;
    }

    void Toggle(EditorVisibilityStore& store, const Scene& scene, Entity entity)
    {
        if (!entity)
            return;
        if (Resolve(scene, store.Hidden(), entity) == Eye::Visible)
            store.SetHidden(entity.GetUUID(), true);
        else
            Show(store, scene, entity);
    }

    void ToggleSelection(EditorVisibilityStore& store, const Scene& scene, const std::vector<Entity>& entities)
    {
        const bool anyShown = std::any_of(entities.begin(), entities.end(), [&](Entity e) {
            return e && Resolve(scene, store.Hidden(), e) == Eye::Visible;
        });
        for (Entity entity : entities)
            if (!entity)
                continue;
            else if (anyShown)
                store.SetHidden(entity.GetUUID(), true);
            else
                Show(store, scene, entity);
    }

    void ToggleIsolate(EditorVisibilityStore& store, Entity entity)
    {
        if (!entity)
            return;
        const auto before = store.Hidden();
        store.Isolate({entity.GetUUID()});
        // Already isolated: the same set came out, so this Alt+click means "show all".
        if (store.Hidden() == before)
            store.ShowAll();
    }

    bool IsSelfActive(Entity entity)
    {
        return !entity.HasComponent<ActiveComponent>() || entity.GetComponent<ActiveComponent>().Active;
    }

    Activity ResolveActive(const Scene& scene, Entity entity)
    {
        if (!entity)
            return Activity::Active;
        if (!IsSelfActive(entity))
            return Activity::Inactive;
        return scene.IsEntityActive(entity) ? Activity::Active : Activity::InactiveByParent;
    }

    bool SetActive(Scene& scene, const std::vector<Entity>& entities, bool active)
    {
        bool changed = false;
        for (Entity entity : entities)
            if (entity && IsSelfActive(entity) != active)
            {
                scene.SetEntityActive(entity, active);
                changed = true;
            }
        return changed;
    }

    bool ToggleActive(Scene& scene, const std::vector<Entity>& entities)
    {
        if (entities.empty() || !entities.back())
            return false;
        return SetActive(scene, entities, !IsSelfActive(entities.back()));
    }
} // namespace Kans::HierarchyVisibility
