#pragma once
#include <unordered_set>
#include <vector>
#include "Kans3D/Scene/Entity.h"

namespace Kans
{
    class EditorVisibilityStore;

    // The Hierarchy's two kinds of hiding.
    //
    // Editor visibility (the eye column, H): a per-project preference kept by EditorVisibilityStore,
    // never in the scene or its undo history. A hidden entity hides its descendants in the editor
    // view; the game and Play views ignore it.
    //
    // Active (the Properties checkbox, Shift+H): ActiveComponent, saved in the scene, applied at
    // runtime, an undoable edit. An inactive entity makes its descendants inactive too.
    namespace HierarchyVisibility
    {
        enum class Eye
        {
            Visible,
            Hidden,        // Hidden itself: the eye-slash stays on its row.
            HiddenByParent // Visible itself, under a hidden ancestor: a faint eye-slash.
        };
        // `hidden` is the store's explicit set (EditorVisibilityStore::Hidden).
        Eye  Resolve(const Scene& scene, const std::unordered_set<UUID>& hidden, Entity entity);
        bool IsHidden(const Scene& scene, const std::unordered_set<UUID>& hidden, Entity entity);

        // An eye click. A visible entity is hidden with its children. A hidden one is shown with
        // its children. One hidden through a parent is shown by showing its hidden ancestors,
        // while their other children stay hidden.
        void Toggle(EditorVisibilityStore& store, const Scene& scene, Entity entity);
        // H on a selection: hides every entity when any of them shows, else shows them all.
        void ToggleSelection(EditorVisibilityStore& store, const Scene& scene, const std::vector<Entity>& entities);
        // Alt+click: shows only this entity (with its ancestors and descendants); Alt+click on
        // the isolated entity again shows everything.
        void ToggleIsolate(EditorVisibilityStore& store, Entity entity);

        enum class Activity
        {
            Active,
            Inactive,        // Its own ActiveComponent is off.
            InactiveByParent // Active itself, under an inactive ancestor.
        };
        Activity ResolveActive(const Scene& scene, Entity entity);
        // The entity's own flag (not inherited); entities without an ActiveComponent are active.
        bool IsSelfActive(Entity entity);
        // Sets every entity's own flag; true when anything changed. One call is one undo step,
        // as the editor records the scene once after the interaction.
        bool SetActive(Scene& scene, const std::vector<Entity>& entities, bool active);
        // Shift+H or the checkbox over a selection: all entities take the opposite of the
        // primary's own flag (the last entity, as HierarchyPanel::GetSelection orders them).
        bool ToggleActive(Scene& scene, const std::vector<Entity>& entities);
    } // namespace HierarchyVisibility
} // namespace Kans
