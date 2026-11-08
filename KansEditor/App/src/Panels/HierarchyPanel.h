#pragma once
#include <imgui.h>
#include <algorithm>
#include <array>
#include <filesystem>
#include <functional>
#include <unordered_map>
#include <vector>
#include "Kans3D/Scene/Entity.h"
#include "KansEditorCore/Panels/EntitySelection.h"

namespace Kans
{
    class EditorVisibilityStore;

    // Scene entity tree: parent/child nesting, drag and drop to reparent or
    // reorder, search, inline rename, create/duplicate/delete. Owns the editor's
    // entity selection.
    //
    // Hiding, as in Unity: the eye column (H, Alt+H) hides entities in the editor view only
    // (HierarchyVisibility, kept in the project's EditorVisibilityStore, not undoable); Active
    // (Shift+H, the Properties checkbox) is scene data applied at runtime and undoable.
    // Inactive and hidden rows are drawn dimmed.
    class HierarchyPanel
    {
    public:
        // Display filter: hide model nodes without a mesh or components (bones), or all model parts.
        void SetHideEmptyModelNodes(bool hide) { m_HideEmptyModelNodes = hide; }
        void SetHideModelParts(bool hide) { m_HideModelParts = hide; }
        // Whether the last Draw listed the entity (for tests).
        bool IsListed(UUID id) const { return std::find(m_Order.begin(), m_Order.end(), id) != m_Order.end(); }
        using FileDrop = std::function<void(const std::filesystem::path& file, Entity parent)>;

        void   SetScene(Ref<Scene> scene);
        Scene* GetScene() const { return m_Scene.get(); }
        // The primary selected entity (what Properties and the gizmo follow), or a null
        // Entity (also after it was deleted).
        Entity GetSelected() const;
        // Selects only this entity and reveals it in the tree.
        void SetSelected(Entity entity);
        // Ctrl-click: adds the entity to the selection or removes it.
        void ToggleSelected(Entity entity);
        // Every selected entity, primary last.
        std::vector<Entity> GetSelection() const;
        size_t              SelectionSize() const { return m_Selection.Size(); }
        bool                IsSelected(Entity entity) const { return entity && m_Selection.Contains(entity.GetUUID()); }

        // Actions on the selection (also used by the viewport's shortcuts). Copy and Cut put
        // the selection on the system clipboard; Paste adds it next to the primary entity.
        void DeleteSelection();
        void DuplicateSelection();
        void CopySelection();
        void CutSelection();
        void Paste();
        bool CanPaste() const;
        // Selected entities whose ancestors are not selected, in tree order.
        std::vector<Entity> TopMostSelected() const;
        // A file dropped from the content browser; `parent` is the entity it was
        // dropped on (null for the empty area).
        void SetFileDropCallback(FileDrop callback) { m_FileDrop = std::move(callback); }

        // The editor view's visibility preferences (the eye column); null hides the column.
        // The store must outlive its use here.
        void                   SetVisibility(EditorVisibilityStore* store) { m_Visibility = store; }
        EditorVisibilityStore* GetVisibility() const { return m_Visibility; }
        // H: hides the selection in the editor view, or shows it when all of it is hidden.
        void ToggleSelectionVisibility();
        // Alt+H, and the viewport's "N hidden" button.
        void ShowAll();
        // Shift+H: every selected entity takes the opposite of the primary's own Active flag.
        void ToggleSelectionActive();

        void Draw();
        // Whether the panel was drawn with keyboard focus this frame or the last (it then takes
        // the entity keys, so the viewport under the pointer does not act on them a second time).
        bool HasFocus() const { return m_FocusFrame >= 0 && m_FocusFrame + 1 >= ImGui::GetFrameCount(); }

    private:
        enum class DropPlace
        {
            Before,
            Into,
            After
        };

        void DrawToolbar();
        void DrawEntity(Entity entity);
        void DrawCreateItems(Entity parent);
        void DrawEntityMenu(Entity entity);
        // Drop target over the last item: reparent or reorder entities, or place files.
        void AcceptDrop(Entity target);
        void HandleKeyboard();
        // The eye button at the right end of a row; `hovered` shows it on visible entities.
        void DrawEye(Entity entity, ImVec2 rowMin, ImVec2 rowMax, bool hovered);
        // Saves the visibility preferences after a change.
        void VisibilityChanged();
        // Marks entities whose name or descendants match the search.
        bool UpdateMatches(Entity entity);
        // Marks entities the display filter shows: not hidden themselves, or with a shown descendant.
        bool UpdateShown(Entity entity);
        bool IsHiddenByFilter(Entity entity) const;

        Entity Create(const char* name, Entity parent);
        void   StartRename(Entity entity);
        void   CommitRename();
        void   Move(Entity entity, Entity target, DropPlace place);

        Ref<Scene> m_Scene;
        EntitySelection m_Selection;
        UUID            m_Reveal = 0; // Expand the tree down to this entity once.
        FileDrop   m_FileDrop;

        UUID                  m_Renaming = 0;
        std::array<char, 256> m_RenameBuffer {};
        bool                  m_RenameFocus = false;

        std::array<char, 128>              m_Search {};
        std::unordered_map<uint64_t, bool> m_Matches; // Subtree matches the search.
        // Display filter (the search ignores it): model nodes without a mesh or other components
        // (bones, empty groups) are hidden by default; all model parts can be hidden too.
        bool                               m_HideEmptyModelNodes = true, m_HideModelParts = false;
        std::unordered_map<uint64_t, bool> m_Shown;
        size_t                             m_HiddenCount = 0;
        std::vector<UUID>                  m_Order;     // Entities as drawn, for arrow keys.
        std::vector<UUID>                  m_LastOrder; // Last frame's, for Shift ranges.
        UUID                               m_MenuEntity     = 0;
        bool                               m_OpenEntityMenu = false;
        bool                               m_OpenCreateMenu = false;
        // Read once when the entity menu opens, not on every frame it stays open.
        bool                   m_MenuCanPaste = false;
        EditorVisibilityStore* m_Visibility   = nullptr;
        int                    m_FocusFrame   = -1;
    };
} // namespace Kans
