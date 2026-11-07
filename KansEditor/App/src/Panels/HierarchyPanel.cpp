#include "HierarchyPanel.h"
#include <imgui.h>
#include <imgui_internal.h>
#include <algorithm>
#include <cctype>
#include "Kans3D/ImGui/EditorTheme.h"
#include "Kans3D/Renderer/Renderer.h"
#include "Kans3D/Scene/Components.h"
#include "Kans3D/Scene/SceneClipboard.h"
#include "Components/ComponentRegistry.h"
#include "UI/EditorIcons.h"
#include "UI/Widgets.h"

namespace Kans
{
    namespace
    {
        constexpr const char* EntityPayload = "entity_payload";
        constexpr ImU32       DropColor     = IM_COL32(70, 150, 230, 255);

        std::string Lowercase(std::string text)
        {
            std::transform(text.begin(), text.end(), text.begin(), [](unsigned char c) { return char(std::tolower(c)); });
            return text;
        }

        // The most telling component decides the icon.
        const char* IconFor(Entity entity) { return ComponentRegistry::Get().IconFor(entity); }

        std::string Label(const char* icon, const char* text) { return std::string(icon) + "  " + text; }
    } // namespace

    void HierarchyPanel::SetScene(Ref<Scene> scene)
    {
        m_Scene    = std::move(scene);
        m_Selection.Clear();
        m_Renaming = 0;
        m_Reveal   = 0;
    }

    Entity HierarchyPanel::GetSelected() const
    {
        return m_Scene && m_Selection.Primary() ? m_Scene->GetEntityByUUID(m_Selection.Primary()) : Entity {};
    }

    void HierarchyPanel::SetSelected(Entity entity)
    {
        m_Selection.Set(entity ? entity.GetUUID() : UUID(0));
        m_Reveal = m_Selection.Primary();
    }

    void HierarchyPanel::ToggleSelected(Entity entity)
    {
        if (!entity)
            return;
        m_Selection.Click(entity.GetUUID(), true, false, m_LastOrder);
        m_Reveal = m_Selection.Primary();
    }

    std::vector<Entity> HierarchyPanel::GetSelection() const
    {
        std::vector<Entity> entities;
        if (!m_Scene)
            return entities;
        for (auto id : m_Selection.Items())
            if (Entity entity = m_Scene->GetEntityByUUID(id); entity && id != m_Selection.Primary())
                entities.push_back(entity);
        if (Entity primary = GetSelected())
            entities.push_back(primary);
        return entities;
    }

    std::vector<Entity> HierarchyPanel::TopMostSelected() const
    {
        std::vector<Entity> result;
        if (!m_Scene)
            return result;
        // Tree order, so pasted and duplicated entities keep their relative order.
        for (auto id : m_LastOrder)
        {
            Entity entity = m_Scene->GetEntityByUUID(id);
            if (!entity || !m_Selection.Contains(id))
                continue;
            bool covered = false;
            for (Entity parent = m_Scene->GetParent(entity); parent && !covered; parent = m_Scene->GetParent(parent))
                covered = m_Selection.Contains(parent.GetUUID());
            if (!covered)
                result.push_back(entity);
        }
        // Selected entities hidden by the search are not in the drawn order.
        for (auto id : m_Selection.Items())
            if (Entity entity = m_Scene->GetEntityByUUID(id);
                entity && std::find(m_LastOrder.begin(), m_LastOrder.end(), id) == m_LastOrder.end())
            {
                bool covered = false;
                for (Entity parent = m_Scene->GetParent(entity); parent && !covered; parent = m_Scene->GetParent(parent))
                    covered = m_Selection.Contains(parent.GetUUID());
                if (!covered)
                    result.push_back(entity);
            }
        return result;
    }

    void HierarchyPanel::DeleteSelection()
    {
        CommitRename();
        for (Entity entity : TopMostSelected())
            if (Entity live = m_Scene->GetEntityByUUID(entity.GetUUID()))
            {
                if (m_Renaming == live.GetUUID())
                    m_Renaming = 0;
                m_Scene->DestroyEntity(live);
            }
        m_Selection.Clear();
    }

    void HierarchyPanel::DuplicateSelection()
    {
        std::vector<UUID> copies;
        for (Entity entity : TopMostSelected())
            if (Entity copy = m_Scene->DuplicateEntity(entity))
                copies.push_back(copy.GetUUID());
        // The duplicates become the selection, the last one primary.
        m_Selection.Clear();
        for (auto id : copies)
            m_Selection.Click(id, true, false, {});
        m_Reveal = m_Selection.Primary();
    }

    void HierarchyPanel::CopySelection()
    {
        const auto entities = TopMostSelected();
        if (!entities.empty())
            ImGui::SetClipboardText(SceneClipboard::Copy(*m_Scene, entities).c_str());
    }

    void HierarchyPanel::CutSelection()
    {
        CopySelection();
        DeleteSelection();
    }

    bool HierarchyPanel::CanPaste() const
    {
        const char* text = ImGui::GetClipboardText();
        return m_Scene && text && SceneClipboard::IsClipboardText(text);
    }

    void HierarchyPanel::Paste()
    {
        if (!CanPaste())
            return;
        // Next to the primary entity (as its siblings), or at the top level.
        const Entity primary = GetSelected();
        const Entity parent  = primary ? m_Scene->GetParent(primary) : Entity {};
        auto         pasted  = SceneClipboard::Paste(*m_Scene, ImGui::GetClipboardText(), parent);
        if (auto* error = std::get_if<AssetError>(&pasted))
        {
            CORE_ERROR("Paste: {}", error->Message);
            return;
        }
        m_Selection.Clear();
        for (Entity entity : std::get<std::vector<Entity>>(pasted))
            m_Selection.Click(entity.GetUUID(), true, false, {});
        m_Reveal = m_Selection.Primary();
    }

    // ---------------------------------------------------------------------- drawing

    void HierarchyPanel::Draw()
    {
        if (!m_Scene)
        {
            ImGui::TextDisabled("No scene is open.");
            return;
        }
        DrawToolbar();
        ImGui::Separator();

        // Deleted entities (also by undo) leave the selection.
        m_Selection.Retain([&](UUID id) { return bool(m_Scene->GetEntityByUUID(id)); });
        m_Matches.clear();
        m_Shown.clear();
        m_HiddenCount        = 0;
        const bool filtering = m_Search[0] != 0;
        const auto roots     = m_Scene->GetRootEntities();
        if (filtering)
            for (Entity root : roots)
                UpdateMatches(root);
        else
            for (Entity root : roots)
                UpdateShown(root);

        ImGui::BeginChild("##Entities");
        if (!m_Order.empty())
            m_LastOrder = m_Order;
        m_Order.clear();
        for (Entity root : roots)
            if (filtering ? m_Matches[uint64_t(root.GetUUID())] : m_Shown[uint64_t(root.GetUUID())])
                DrawEntity(root);
        m_Reveal = 0;
        if (roots.empty())
            ImGui::TextDisabled("The scene is empty. Right-click to create an entity.");
        else if (filtering && m_Order.empty())
            ImGui::TextDisabled("No entity matches the search.");

        // Empty area: deselect, create, and drop to move to the top level.
        ImGui::Dummy(ImGui::GetContentRegionAvail());
        if (ImGui::IsItemClicked(ImGuiMouseButton_Left))
        {
            CommitRename();
            if (!ImGui::GetIO().KeyCtrl && !ImGui::GetIO().KeyShift)
                m_Selection.Clear();
        }
        if (ImGui::IsItemClicked(ImGuiMouseButton_Right))
            m_OpenCreateMenu = true;
        AcceptDrop({});
        HandleKeyboard();

        if (m_OpenEntityMenu)
            ImGui::OpenPopup("##EntityMenu");
        if (m_OpenCreateMenu)
            ImGui::OpenPopup("##CreateMenu");
        m_OpenEntityMenu = m_OpenCreateMenu = false;
        if (ImGui::BeginPopup("##EntityMenu"))
        {
            if (Entity entity = m_Scene->GetEntityByUUID(m_MenuEntity))
                DrawEntityMenu(entity);
            ImGui::EndPopup();
        }
        if (ImGui::BeginPopup("##CreateMenu"))
        {
            DrawCreateItems({});
            ImGui::EndPopup();
        }
        ImGui::EndChild();
    }

    void HierarchyPanel::DrawToolbar()
    {
        if (ImGui::Button(ICON_FA_PLUS))
            ImGui::OpenPopup("##CreateToolbar");
        if (ImGui::IsItemHovered())
            ImGui::SetTooltip("Create an entity (as a child of the selection)");
        if (ImGui::BeginPopup("##CreateToolbar"))
        {
            DrawCreateItems(GetSelected());
            ImGui::EndPopup();
        }
        ImGui::SameLine();
        ImGui::SetNextItemWidth(ImGui::GetContentRegionAvail().x - ImGui::GetFrameHeight() -
                                ImGui::GetStyle().ItemSpacing.x);
        ImGui::InputTextWithHint("##search", ICON_FA_MAGNIFYING_GLASS " Search", m_Search.data(), m_Search.size());
        ImGui::SameLine();
        // The display filter; its button is lit while it hides anything.
        const bool active = m_HiddenCount > 0;
        if (active)
            ImGui::PushStyleColor(ImGuiCol_Text, EditorTheme::Colors().Accent);
        if (ImGui::Button(ICON_FA_FILTER, {ImGui::GetFrameHeight(), ImGui::GetFrameHeight()}))
            ImGui::OpenPopup("##DisplayFilter");
        if (active)
            ImGui::PopStyleColor();
        if (ImGui::IsItemHovered())
            ImGui::SetTooltip(active ? "Display filter: %zu entities hidden" : "Display filter", m_HiddenCount);
        if (ImGui::BeginPopup("##DisplayFilter"))
        {
            ImGui::TextDisabled("Hide in this list (the search still finds them)");
            ImGui::Checkbox("Bones and empty model nodes", &m_HideEmptyModelNodes);
            if (ImGui::IsItemHovered())
                ImGui::SetTooltip("Parts of models with no mesh or other component; still shown when they hold\n"
                                  "something that is shown, such as an object attached to a hand bone");
            ImGui::Checkbox("All model parts (models show as one entry)", &m_HideModelParts);
            ImGui::EndPopup();
        }
    }

    bool HierarchyPanel::IsHiddenByFilter(Entity entity) const
    {
        if (!entity.HasComponent<PrefabOwnedComponent>())
            return false;
        if (m_HideModelParts)
            return true;
        if (!m_HideEmptyModelNodes)
            return false;
        // Empty: no mesh and no registered component besides its transform (bones, grouping nodes).
        if (entity.HasComponent<MeshComponent>())
            return false;
        for (const auto& entry : ComponentRegistry::Get().Entries())
            if (entry.Type != std::type_index(typeid(TransformComponent)) && entry.Has && entry.Has(entity))
                return false;
        return true;
    }

    bool HierarchyPanel::UpdateShown(Entity entity)
    {
        bool shown = false;
        for (Entity child : m_Scene->GetChildren(entity))
            shown |= UpdateShown(child);
        const bool hidden = IsHiddenByFilter(entity);
        m_HiddenCount += hidden;
        // A hidden node holding something shown stays, so that the shown entity can be reached.
        shown |= !hidden;
        m_Shown[uint64_t(entity.GetUUID())] = shown;
        return shown;
    }

    bool HierarchyPanel::UpdateMatches(Entity entity)
    {
        bool matches = Lowercase(entity.GetComponent<TagComponent>().Tag).find(Lowercase(m_Search.data())) !=
                       std::string::npos;
        for (Entity child : m_Scene->GetChildren(entity))
            matches |= UpdateMatches(child);
        m_Matches[uint64_t(entity.GetUUID())] = matches;
        return matches;
    }

    void HierarchyPanel::DrawEntity(Entity entity)
    {
        const UUID  id        = entity.GetUUID();
        const auto  children  = m_Scene->GetChildren(entity);
        const bool  filtering = m_Search[0] != 0;
        const auto& name      = entity.GetComponent<TagComponent>().Tag;
        m_Order.push_back(id);

        ImGuiTreeNodeFlags flags = ImGuiTreeNodeFlags_OpenOnArrow | ImGuiTreeNodeFlags_SpanAvailWidth |
                                   ImGuiTreeNodeFlags_FramePadding | ImGuiTreeNodeFlags_AllowOverlap;
        if (m_Selection.Contains(id))
            flags |= ImGuiTreeNodeFlags_Selected;
        // A node whose children are all filtered out draws as a leaf.
        if (std::none_of(children.begin(), children.end(), [&](Entity child) {
                return filtering ? m_Matches[uint64_t(child.GetUUID())] : m_Shown[uint64_t(child.GetUUID())];
            }))
            flags |= ImGuiTreeNodeFlags_Leaf;
        // Open the path to a revealed entity, and every match while searching.
        const Entity revealed = m_Reveal ? m_Scene->GetEntityByUUID(m_Reveal) : Entity {};
        if (filtering || (revealed && m_Scene->IsDescendantOf(revealed, entity)))
            ImGui::SetNextItemOpen(true);

        const bool renaming = id == m_Renaming;
        const bool direct   = !filtering || Lowercase(name).find(Lowercase(m_Search.data())) != std::string::npos;
        ImGui::PushStyleVar(ImGuiStyleVar_FramePadding, {4.0f, 2.0f});
        // Prefab instances and their parts are tinted, as their structure comes from the prefab.
        const bool owned  = entity.HasComponent<PrefabOwnedComponent>();
        const bool prefab = owned || entity.HasComponent<PrefabInstanceComponent>();
        if (!direct)
            ImGui::PushStyleColor(ImGuiCol_Text, ImGui::GetStyleColorVec4(ImGuiCol_TextDisabled));
        else if (prefab)
            ImGui::PushStyleColor(ImGuiCol_Text, ImVec4(0.45f, 0.70f, 1.0f, 1.0f));
        const bool open = ImGui::TreeNodeEx(reinterpret_cast<void*>(uintptr_t(uint64_t(id))),
                                            flags,
                                            "%s  %s",
                                            IconFor(entity),
                                            renaming ? "" : name.c_str());
        if (!direct || prefab)
            ImGui::PopStyleColor();
        ImGui::PopStyleVar();

        if (ImGui::IsItemClicked(ImGuiMouseButton_Left) && !ImGui::IsItemToggledOpen())
        {
            if (m_Renaming != id)
                CommitRename();
            const ImGuiIO& io = ImGui::GetIO();
            // Shift ranges use last frame's order: entities below this one are not drawn yet.
            m_Selection.Click(id, io.KeyCtrl, io.KeyShift, m_LastOrder);
            if (ImGui::IsMouseDoubleClicked(ImGuiMouseButton_Left) && !owned && !io.KeyCtrl && !io.KeyShift)
                StartRename(entity);
        }
        if (ImGui::IsItemClicked(ImGuiMouseButton_Right))
        {
            // The menu acts on the selection; right-clicking outside it selects that entity alone.
            if (!m_Selection.Contains(id))
                m_Selection.Set(id);
            m_MenuEntity     = id;
            m_OpenEntityMenu = true;
        }
        // A prefab part stays where the prefab puts it; it cannot be moved out.
        if (!owned && ImGui::BeginDragDropSource())
        {
            const uint64_t payload = uint64_t(id);
            ImGui::SetDragDropPayload(EntityPayload, &payload, sizeof(payload));
            ImGui::Text("%s  %s", IconFor(entity), name.c_str());
            ImGui::EndDragDropSource();
        }
        AcceptDrop(entity);

        if (renaming)
        {
            // Edit the name in place, after the icon.
            ImGui::SameLine(ImGui::GetItemRectMin().x - ImGui::GetWindowPos().x + ImGui::GetTreeNodeToLabelSpacing() +
                            ImGui::CalcTextSize(IconFor(entity)).x + 8.0f);
            if (m_RenameFocus)
            {
                ImGui::SetKeyboardFocusHere();
                m_RenameFocus = false;
            }
            ImGui::SetNextItemWidth(-FLT_MIN);
            const bool submitted = ImGui::InputText("##rename",
                                                    m_RenameBuffer.data(),
                                                    m_RenameBuffer.size(),
                                                    ImGuiInputTextFlags_EnterReturnsTrue |
                                                        ImGuiInputTextFlags_AutoSelectAll);
            if (ImGui::IsItemActive() && ImGui::IsKeyPressed(ImGuiKey_Escape))
                m_Renaming = 0;
            else if (submitted || ImGui::IsItemDeactivated())
                CommitRename();
        }

        if (!open)
            return;
        for (Entity child : children)
            if (filtering ? m_Matches[uint64_t(child.GetUUID())] : m_Shown[uint64_t(child.GetUUID())])
                DrawEntity(child);
        ImGui::TreePop();
    }

    void HierarchyPanel::AcceptDrop(Entity target)
    {
        if (!ImGui::BeginDragDropTarget())
            return;
        const ImVec2 min = ImGui::GetItemRectMin(), max = ImGui::GetItemRectMax();
        // Rows: top quarter inserts before, bottom quarter after, the middle nests.
        DropPlace place = DropPlace::Into;
        if (target)
        {
            const float y = ImGui::GetMousePos().y;
            if (y < min.y + (max.y - min.y) * 0.25f)
                place = DropPlace::Before;
            else if (y > max.y - (max.y - min.y) * 0.25f)
                place = DropPlace::After;
        }
        const ImGuiDragDropFlags flags = ImGuiDragDropFlags_AcceptBeforeDelivery | ImGuiDragDropFlags_AcceptNoDrawDefaultRect;
        if (const auto* payload = ImGui::AcceptDragDropPayload(EntityPayload, flags))
        {
            Entity dragged = m_Scene->GetEntityByUUID(UUID(*static_cast<const uint64_t*>(payload->Data)));
            // An entity cannot move into (or next to an entity inside) its own subtree.
            const bool valid = dragged && dragged != target && !(target && m_Scene->IsDescendantOf(target, dragged));
            if (valid && target)
            {
                ImDrawList* draw = ImGui::GetWindowDrawList();
                if (place == DropPlace::Into)
                    draw->AddRect(min, max, DropColor, 3.0f, 0, 2.0f);
                else
                {
                    const float y = place == DropPlace::Before ? min.y : max.y;
                    draw->AddLine({min.x, y}, {max.x, y}, DropColor, 2.0f);
                }
            }
            if (valid && payload->IsDelivery())
                Move(dragged, target, place);
        }
        // Files from the content browser (e.g. models to place).
        if (const auto* payload = ImGui::AcceptDragDropPayload("asset_payload"))
            if (m_FileDrop)
                m_FileDrop(std::filesystem::u8path(std::string(static_cast<const char*>(payload->Data),
                                                               size_t(payload->DataSize))),
                           target);
        ImGui::EndDragDropTarget();
    }

    void HierarchyPanel::Move(Entity entity, Entity target, DropPlace place)
    {
        if (!target)
        {
            m_Scene->SetParent(entity, {}, {}, true);
            return;
        }
        if (place == DropPlace::Into)
        {
            m_Scene->SetParent(entity, target, {}, true);
            m_Reveal = entity.GetUUID();
            return;
        }
        // Before/after the target among its siblings.
        Entity parent   = m_Scene->GetParent(target);
        auto   siblings = parent ? m_Scene->GetChildren(parent) : m_Scene->GetRootEntities();
        Entity before   = target;
        if (place == DropPlace::After)
        {
            const auto at = std::find(siblings.begin(), siblings.end(), target);
            before        = at != siblings.end() && at + 1 != siblings.end() ? *(at + 1) : Entity {};
            if (before == entity)
            {
                // Already right after the target.
                return;
            }
        }
        m_Scene->SetParent(entity, parent, before, true);
    }

    // ---------------------------------------------------------------------- menus and actions

    void HierarchyPanel::DrawCreateItems(Entity parent)
    {
        if (ImGui::MenuItem(Label(ICON_FA_VECTOR_SQUARE, "Empty Entity").c_str()))
            Create("Entity", parent);
        // Components offered by the registry, each creating a named entity with it, in sections:
        // objects, physics (wind zones, force fields, motion colliders), lights, cameras.
        const auto section = [](const ComponentEntry& c) {
            if (c.Category == "Physics")
                return 1;
            if (c.Category == "Lighting")
                return 2;
            return c.Name == "Camera" ? 3 : 0;
        };
        std::vector<const ComponentEntry*> creatable;
        for (const auto& component : ComponentRegistry::Get().Entries())
            if ((!component.CreateName.empty() && component.Create) || !component.CreateItems.empty())
                creatable.push_back(&component);
        std::stable_sort(creatable.begin(), creatable.end(), [&](const auto* a, const auto* b) {
            return std::make_pair(section(*a), a->CreateOrder) < std::make_pair(section(*b), b->CreateOrder);
        });
        const char* sections[] = {"Objects", "Physics", "Lights", "Camera"};
        int         current    = -1;
        for (const auto* component : creatable)
        {
            if (section(*component) != current)
            {
                current = section(*component);
                Widgets::MenuSection(sections[current]);
            }
            if (component->CreateItems.empty())
            {
                if (ImGui::MenuItem(Label(component->Icon, component->CreateName.c_str()).c_str()))
                    component->Create(Create(component->CreateName.c_str(), parent));
                continue;
            }
            if (!ImGui::BeginMenu(Label(component->Icon, component->CreateMenu.c_str()).c_str()))
                continue;
            for (const auto& item : component->CreateItems)
                if (ImGui::MenuItem(Label(item.Icon ? item.Icon : component->Icon, item.Name.c_str()).c_str()))
                    item.Create(Create(item.Name.c_str(), parent));
            ImGui::EndMenu();
        }
    }

    void HierarchyPanel::DrawEntityMenu(Entity entity)
    {
        if (ImGui::BeginMenu(Label(ICON_FA_PLUS, "Create Child").c_str()))
        {
            DrawCreateItems(entity);
            ImGui::EndMenu();
        }
        ImGui::Separator();
        const bool owned  = entity.HasComponent<PrefabOwnedComponent>();
        const bool single = m_Selection.Size() <= 1;
        if (ImGui::MenuItem(Label(ICON_FA_PEN, "Rename").c_str(), "F2", false, !owned && single))
            StartRename(entity);
        if (ImGui::MenuItem(Label(ICON_FA_CLONE, "Duplicate").c_str(), "Ctrl+D"))
            DuplicateSelection();
        if (ImGui::MenuItem(Label(ICON_FA_SCISSORS, "Cut").c_str(), "Ctrl+X"))
            CutSelection();
        if (ImGui::MenuItem(Label(ICON_FA_COPY, "Copy").c_str(), "Ctrl+C"))
            CopySelection();
        if (ImGui::MenuItem(Label(ICON_FA_PASTE, "Paste").c_str(), "Ctrl+V", false, CanPaste()))
            Paste();
        if (m_Scene->GetParent(entity) && !owned && single &&
            ImGui::MenuItem(Label(ICON_FA_ARROW_UP, "Move to Top Level").c_str()))
            m_Scene->SetParent(entity, {}, {}, true);
        ImGui::Separator();
        if (ImGui::MenuItem(Label(ICON_FA_COPY, "Copy ID").c_str()))
            ImGui::SetClipboardText(std::to_string(uint64_t(entity.GetUUID())).c_str());
        ImGui::PushStyleColor(ImGuiCol_Text, ImVec4(0.95f, 0.45f, 0.40f, 1.0f));
        const bool  children = !m_Scene->GetChildren(entity).empty();
        std::string remove   = !single ? "Delete " + std::to_string(m_Selection.Size()) + " Entities" :
                               children ? std::string("Delete with Children") : std::string("Delete");
        if (ImGui::MenuItem(Label(ICON_FA_TRASH, remove.c_str()).c_str(), "Del"))
            DeleteSelection();
        ImGui::PopStyleColor();
    }

    Entity HierarchyPanel::Create(const char* name, Entity parent)
    {
        Entity entity = m_Scene->CreateEntity(name);
        if (parent)
            m_Scene->SetParent(entity, parent, {}, false);
        SetSelected(entity);
        return entity;
    }

    void HierarchyPanel::StartRename(Entity entity)
    {
        // A part's name comes from the prefab and is restored on every expansion.
        if (entity.HasComponent<PrefabOwnedComponent>())
            return;
        m_Renaming = entity.GetUUID();
        m_RenameBuffer.fill(0);
        const auto& name = entity.GetComponent<TagComponent>().Tag;
        std::copy_n(name.begin(), std::min(name.size(), m_RenameBuffer.size() - 1), m_RenameBuffer.begin());
        m_RenameFocus = true;
        m_Selection.Set(m_Renaming);
    }

    void HierarchyPanel::CommitRename()
    {
        if (!m_Renaming)
            return;
        if (Entity entity = m_Scene->GetEntityByUUID(m_Renaming); entity && m_RenameBuffer[0])
            entity.GetComponent<TagComponent>().Tag = m_RenameBuffer.data();
        m_Renaming = 0;
    }

    void HierarchyPanel::HandleKeyboard()
    {
        const ImGuiIO& io = ImGui::GetIO();
        if (!ImGui::IsWindowFocused(ImGuiFocusedFlags_RootAndChildWindows) || io.WantTextInput || m_Renaming)
            return;
        Entity selected = GetSelected();
        if (io.KeyCtrl && ImGui::IsKeyPressed(ImGuiKey_V, false))
            Paste(); // Also works with nothing selected.
        if (!selected)
            return;
        if (ImGui::IsKeyPressed(ImGuiKey_F2, false))
            StartRename(selected);
        else if (ImGui::IsKeyPressed(ImGuiKey_Delete, false))
            DeleteSelection();
        else if (io.KeyCtrl && ImGui::IsKeyPressed(ImGuiKey_D, false))
            DuplicateSelection();
        else if (io.KeyCtrl && ImGui::IsKeyPressed(ImGuiKey_C, false))
            CopySelection();
        else if (io.KeyCtrl && ImGui::IsKeyPressed(ImGuiKey_X, false))
            CutSelection();
        else if (ImGui::IsKeyPressed(ImGuiKey_UpArrow) || ImGui::IsKeyPressed(ImGuiKey_DownArrow))
        {
            // Move through the entities as shown.
            const auto at = std::find(m_Order.begin(), m_Order.end(), m_Selection.Primary());
            if (at == m_Order.end())
                return;
            if (ImGui::IsKeyPressed(ImGuiKey_UpArrow) && at != m_Order.begin())
                m_Selection.Set(*(at - 1));
            else if (ImGui::IsKeyPressed(ImGuiKey_DownArrow) && at + 1 != m_Order.end())
                m_Selection.Set(*(at + 1));
        }
    }
} // namespace Kans
