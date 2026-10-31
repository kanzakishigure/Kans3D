#include "PropertiesPanel.h"
#include <imgui.h>
#include <algorithm>
#include <cctype>
#include <filesystem>
#include <misc/cpp/imgui_stdlib.h>
#include "Kans3D/Asset/Runtime/MeshData.h"
#include "Kans3D/FileSystem/FileSystem.h"
#include "Kans3D/ImGui/EditorTheme.h"
#include "Kans3D/Renderer/Renderer.h"
#include "Components/ComponentEditors.h"
#include "UI/BloomSettingsWidget.h"
#include "UI/EditorIcons.h"
#include "UI/MaterialSlotRow.h"
#include "UI/PropertyGrid.h"

namespace Kans
{
    namespace
    {
        // Once an inherited editor map becomes authored scene data, copy it into the project
        // so the scene keeps a portable relative path and preserves the lighting it was showing.
        std::optional<std::filesystem::path> AuthoredEnvironmentMap(const std::filesystem::path& map)
        {
            namespace fs = std::filesystem;
            if (!map.is_absolute())
                return map;
            if (KansFileSystem::GetAssetFolder().empty())
                return std::nullopt;
            const auto directory = KansFileSystem::GetAssetFolder() / "Environment";
            std::error_code ec;
            fs::create_directories(directory, ec);
            if (ec)
            {
                CORE_ERROR("Cannot copy editor environment into project: {}", ec.message());
                return std::nullopt;
            }
            auto destination = directory / map.filename();
            if (fs::exists(destination, ec))
                destination = directory / (map.stem().string() + "-" + SourceAssetID::Generate().ToString() + map.extension().string());
            fs::copy_file(map, destination, ec);
            if (ec)
            {
                CORE_ERROR("Cannot copy editor environment into project: {}", ec.message());
                return std::nullopt;
            }
            return destination.lexically_relative(KansFileSystem::GetProjectRoot());
        }


        bool ContainsIgnoreCase(const std::string& text, const char* needle)
        {
            std::string a(text), b(needle);
            std::transform(a.begin(), a.end(), a.begin(), [](unsigned char c) { return char(std::tolower(c)); });
            std::transform(b.begin(), b.end(), b.begin(), [](unsigned char c) { return char(std::tolower(c)); });
            return a.find(b) != std::string::npos;
        }

        // Search box at the top of a popup or combo list.
        void FilterBox(std::array<char, 128>& filter)
        {
            if (ImGui::IsWindowAppearing())
            {
                filter.fill(0);
                ImGui::SetKeyboardFocusHere();
            }
            ImGui::SetNextItemWidth(-FLT_MIN);
            ImGui::InputTextWithHint("##filter", ICON_FA_MAGNIFYING_GLASS " Search", filter.data(), filter.size());
        }
    } // namespace

    // ---------------------------------------------------------------------- layout

    void PropertiesPanel::Draw()
    {
        // Rows set the hover again while drawn; pinned and isolated rows belong to one entity.
        m_HoveredSlot.clear();
        if (Scene* scene = m_Hierarchy.GetScene(); scene && m_ChainPicker.IsActive())
            if (Entity owner = SecondaryMotionEditing::FeedChainPicker(m_ChainPicker, *scene, m_Hierarchy.GetSelected()))
                m_Hierarchy.SetSelected(owner);
        const Entity selected = m_Hierarchy.GetSelected();
        const UUID   owner    = selected ? selected.GetUUID() : UUID(0);
        if (owner != m_SlotsOwner)
        {
            m_SlotsOwner = owner;
            m_HighlightedSlots.clear();
            m_IsolatedSlot.clear();
        }
        DrawSearch();
        PropertyGrid::SetFilter(m_Search, m_ModifiedOnly);
        if (ImGui::BeginChild("##Cards", {0, 0}, ImGuiChildFlags_None))
            DrawContents();
        ImGui::EndChild();
        PropertyGrid::ClearFilter();
    }

    void PropertiesPanel::DrawSearch()
    {
        // Search filters rows by label; the toggle keeps only rows that differ from their default.
        const float toggle = ImGui::GetFrameHeight();
        ImGui::SetNextItemWidth(ImGui::GetContentRegionAvail().x - toggle - ImGui::GetStyle().ItemSpacing.x);
        ImGui::InputTextWithHint("##Search", ICON_FA_MAGNIFYING_GLASS "  Search properties", &m_Search);
        ImGui::SameLine();
        ImGui::PushStyleColor(ImGuiCol_Button, m_ModifiedOnly ? EditorTheme::Colors().Selection : ImVec4(0, 0, 0, 0));
        if (ImGui::Button(ICON_FA_FILTER "##ModifiedOnly", {toggle, toggle}))
            m_ModifiedOnly = !m_ModifiedOnly;
        ImGui::PopStyleColor();
        if (ImGui::IsItemHovered())
            ImGui::SetTooltip(m_ModifiedOnly ? "Showing modified values only" : "Show modified values only");
        ImGui::Dummy({0, EditorTheme::Px(2.0f)});
    }

    void PropertiesPanel::DrawContents()
    {
        Entity entity = m_Hierarchy.GetSelected();
        if (!entity)
        {
            if (Scene* scene = GetScene())
            {
                DrawSceneSettings(*scene);
                DrawPostProcess(*scene);
            }
            ImGui::Spacing();
            ImGui::TextDisabled("Select an entity in the hierarchy to edit its components.");
            return;
        }
        DrawHeader(entity);
        DrawPrefabPart(entity);
        ImGui::Spacing();
        for (const auto& component : ComponentRegistry::Get().Entries())
            if (component.Has(entity))
                DrawSection(entity, component);
        ImGui::Spacing();
        DrawAddComponent(entity);
    }

    void PropertiesPanel::DrawSceneSettings(Scene& scene)
    {
        if (PropertyGrid::BeginCard("Environment", ICON_FA_GLOBE, "Environment"))
            DrawEnvironmentGroups(scene);
        PropertyGrid::EndCard();
    }

    void PropertiesPanel::DrawEnvironmentGroups(Scene& scene)
    {
        // The project's HDR images, listed once per panel open is enough; rescan on demand.
        if (m_EnvironmentMaps.empty() || ImGui::IsWindowAppearing())
        {
            m_EnvironmentMaps.clear();
            std::error_code error;
            const auto&     root = KansFileSystem::GetProjectRoot();
            for (auto it = std::filesystem::recursive_directory_iterator(KansFileSystem::GetAssetFolder(), error);
                 !error && it != std::filesystem::recursive_directory_iterator(); it.increment(error))
                if (it->is_regular_file(error) && it->path().extension() == ".hdr")
                    m_EnvironmentMaps.push_back(it->path().lexically_relative(root));
            std::sort(m_EnvironmentMaps.begin(), m_EnvironmentMaps.end());
        }
        static const EnvironmentSettings defaults;
        const bool authored = scene.HasEnvironmentSettings();
        auto       settings = scene.GetEnvironmentSettings();
        bool       changed  = false;
        if (PropertyGrid::BeginGroup("Sky"))
        {
            // "Editor default" leaves the scene without settings: it shows the editor's environment.
            const std::string current = !authored            ? "Editor default" :
                                        settings.Map.empty() ? "None" :
                                                               settings.Map.generic_u8string();
            if (PropertyGrid::Row("Map", "The HDR image lighting the scene and shown as the sky"))
            {
                ImGui::PushID("Map");
                if (ImGui::BeginCombo("##map", current.c_str()))
                {
                    if (ImGui::Selectable("Editor default", !authored))
                        scene.ClearEnvironmentSettings();
                    if (ImGui::Selectable("None", authored && settings.Map.empty()))
                    {
                        settings.Map.clear();
                        scene.SetEnvironmentSettings(settings);
                    }
                    for (const auto& map : m_EnvironmentMaps)
                        if (ImGui::Selectable(map.generic_u8string().c_str(), authored && settings.Map == map))
                        {
                            settings.Map = map;
                            scene.SetEnvironmentSettings(settings);
                        }
                    ImGui::EndCombo();
                }
                ImGui::PopID();
            }
            changed |= PropertyGrid::Float("Rotation", settings.Rotation, 0.5f, -360.0f, 720.0f, "%.1f°", &defaults.Rotation);
            changed |= PropertyGrid::Checkbox("Show sky", settings.ShowSkybox, &defaults.ShowSkybox);
            if (settings.ShowSkybox)
                changed |= PropertyGrid::Float("Sky brightness", settings.SkyboxIntensity, 0.01f, 0.0f, 16.0f, "%.3f",
                                               &defaults.SkyboxIntensity);
            // Scene-linear, like Blender's world colour; shown wherever no sky is drawn.
            changed |= PropertyGrid::Color3("Background", settings.BackgroundColor, &defaults.BackgroundColor);
            PropertyGrid::EndGroup();
        }
        if (PropertyGrid::BeginGroup("Lighting"))
        {
            changed |= PropertyGrid::Float("Diffuse", settings.DiffuseIntensity, 0.01f, 0.0f, 16.0f, "%.3f",
                                           &defaults.DiffuseIntensity);
            changed |= PropertyGrid::Float("Specular", settings.SpecularIntensity, 0.01f, 0.0f, 16.0f, "%.3f",
                                           &defaults.SpecularIntensity);
            PropertyGrid::EndGroup();
        }
        if (changed)
        {
            // The first edit of a scene without settings keeps the map it was showing.
            if (!authored)
            {
                auto map = AuthoredEnvironmentMap(m_DefaultEnvironment);
                if (map)
                {
                    settings.Map = *map;
                    scene.SetEnvironmentSettings(settings);
                }
            }
            else
                scene.SetEnvironmentSettings(settings);
        }
    }

    void PropertiesPanel::DrawPostProcess(Scene& scene)
    {
        // Applies to every camera of the scene and to the editor view; a camera may override it.
        if (PropertyGrid::BeginCard("PostProcessing", ICON_FA_SLIDERS, "Post Processing"))
        {
            auto settings = scene.GetPostProcessSettings();
            if (DrawPostProcessSettings(
                    settings,
                    [&](AssetID& dirt) { return AssetPicker("Lens dirt", AssetType::Texture, dirt); },
                    [&](const char* label, AssetID& id) { return AssetPicker(label, AssetType::Material, id); }))
                scene.SetPostProcessSettings(settings);
        }
        PropertyGrid::EndCard();
    }

    void PropertiesPanel::DrawHeader(Entity entity)
    {
        auto& tag = entity.GetComponent<TagComponent>().Tag;
        ImGui::AlignTextToFramePadding();
        ImGui::TextUnformatted(ICON_FA_CUBE);
        ImGui::SameLine();
        char name[256] = {};
        tag.copy(name, sizeof(name) - 1);
        ImGui::SetNextItemWidth(-FLT_MIN);
        if (ImGui::InputText("##Name", name, sizeof(name)))
            tag = name;
        // With several entities selected, Properties edits the primary one (the last clicked).
        if (const size_t count = m_Hierarchy.SelectionSize(); count > 1)
            ImGui::TextColored(EditorTheme::Colors().AccentHover, ICON_FA_LAYER_GROUP "  %zu selected  |  editing %s", count,
                               tag.c_str());
        if (entity.HasComponent<IDComponent>())
        {
            const auto id = std::to_string(static_cast<uint64_t>(entity.GetUUID()));
            ImGui::TextDisabled("ID %s", id.c_str());
            ImGui::SameLine();
            if (ImGui::SmallButton(ICON_FA_COPY "##CopyID"))
                ImGui::SetClipboardText(id.c_str());
            if (ImGui::IsItemHovered())
                ImGui::SetTooltip("Copy the entity ID");
        }
    }

    void PropertiesPanel::DrawSection(Entity entity, const ComponentEntry& component)
    {
        Scene*     scene      = GetScene();
        const bool overridden = scene && component.Overridden && component.Overridden(entity, *scene);
        bool       remove     = false;
        PropertyGrid::CardOptions options;
        // A prefab part's card that carries an override gets the accent bar, as the Hierarchy tints it.
        options.Overridden = overridden;
        options.Note       = overridden ? "Overridden" : nullptr;
        options.Menu       = [&] {
            bool any = false;
            if (component.Reset && ImGui::MenuItem(ICON_FA_ROTATE_RIGHT "  Reset"))
                component.Reset(entity);
            any |= bool(component.Reset);
            if (component.Copy)
            {
                if (ImGui::MenuItem(ICON_FA_COPY "  Copy values"))
                {
                    m_Clipboard     = component.Copy(entity);
                    m_ClipboardType = component.Type;
                }
                if (ImGui::MenuItem(ICON_FA_PASTE "  Paste values", nullptr, false,
                                    component.Paste && m_ClipboardType == component.Type))
                    component.Paste(entity, m_Clipboard);
                any = true;
            }
            if (component.Revert && ImGui::MenuItem(ICON_FA_ROTATE_LEFT "  Revert to prefab", nullptr, false, overridden))
                component.Revert(entity, *scene);
            any |= overridden;
            const bool removable = component.CanRemove && component.CanRemove(entity);
            if (removable)
            {
                if (any)
                    ImGui::Separator();
                if (ImGui::MenuItem(ICON_FA_TRASH "  Remove Component"))
                    remove = true;
            }
            if (!any && !removable)
                ImGui::TextDisabled("No actions");
        };
        if (PropertyGrid::BeginCard(component.Name.c_str(), component.Icon, component.Name.c_str(), options))
            component.Draw(entity, *this);
        PropertyGrid::EndCard();
        if (remove)
            component.Remove(entity);
    }

    void PropertiesPanel::DrawAddComponent(Entity entity)
    {
        const auto& components = ComponentRegistry::Get().Entries();
        const auto  addable    = [&](const ComponentEntry& c) {
            return c.Add && !c.Has(entity) && (!c.CanAdd || c.CanAdd(entity));
        };
        const bool  available  = std::any_of(components.begin(), components.end(), addable);
        ImGui::BeginDisabled(!available);
        if (ImGui::Button(ICON_FA_PLUS "  Add Component", {-FLT_MIN, 0.0f}))
            ImGui::OpenPopup("##AddComponent");
        ImGui::EndDisabled();
        ImGui::SetNextWindowSizeConstraints({ImGui::GetItemRectSize().x, 0.0f}, {FLT_MAX, 400.0f});
        if (!ImGui::BeginPopup("##AddComponent"))
            return;
        FilterBox(m_ComponentFilter);
        // Grouped by category, in the order the categories first appear.
        std::vector<std::string> categories;
        for (const auto& c : components)
            if (std::find(categories.begin(), categories.end(), c.Category) == categories.end())
                categories.push_back(c.Category);
        for (const auto& category : categories)
        {
            bool heading = false;
            for (const auto& c : components)
            {
                if (c.Category != category || !addable(c) || !ContainsIgnoreCase(c.Name, m_ComponentFilter.data()))
                    continue;
                if (!heading)
                {
                    ImGui::Separator();
                    ImGui::TextDisabled("%s", category.c_str());
                    heading = true;
                }
                if (ImGui::Selectable((std::string(c.Icon) + "  " + c.Name).c_str()))
                {
                    c.Add(entity);
                    ImGui::CloseCurrentPopup();
                }
            }
        }
        ImGui::EndPopup();
    }

    // ---------------------------------------------------------------------- prefab parts

    void PropertiesPanel::DrawPrefabPart(Entity entity)
    {
        // A part of a prefab instance: its structure comes from the prefab; transform and
        // material edits are kept as overrides on the instance.
        if (!entity.HasComponent<PrefabOwnedComponent>())
            return;
        Scene*      scene = GetScene();
        const auto& owned = entity.GetComponent<PrefabOwnedComponent>();
        Entity      root  = scene ? scene->GetEntityByUUID(owned.Instance) : Entity {};
        if (!root)
            return;
        ImGui::Spacing();
        ImGui::TextDisabled(ICON_FA_CUBES "  Part of %s", root.GetComponent<TagComponent>().Tag.c_str());
        ImGui::SameLine();
        if (ImGui::SmallButton("Select instance"))
            m_Hierarchy.SetSelected(root);
        const auto& overrides = root.GetComponent<PrefabInstanceComponent>().Overrides;
        if (overrides.count(owned.NodeKey))
        {
            ImGui::SameLine();
            if (ImGui::SmallButton(ICON_FA_ROTATE_RIGHT " Revert part"))
                scene->ClearPrefabOverride(root, owned.NodeKey);
            if (ImGui::IsItemHovered())
                ImGui::SetTooltip("Restore this part's transform and materials from the prefab");
        }
    }

    // ---------------------------------------------------------------------- pickers

    bool PropertiesPanel::AssetPicker(const char* label, AssetType type, AssetID& id)
    {
        if (!PropertyGrid::Row(label))
            return false;
        ImGui::PushID(label);
        bool        changed = false;
        std::string preview = id.IsValid() ? "(missing) " + id.ToString() : "(none)";
        for (const auto& choice : m_AssetChoices)
            if (choice.ID == id)
                preview = choice.Name;
        if (ImGui::BeginCombo("##asset", preview.c_str(), ImGuiComboFlags_HeightLarge))
        {
            FilterBox(m_AssetFilter);
            for (const auto& choice : m_AssetChoices)
            {
                if (choice.Type != type || !ContainsIgnoreCase(choice.Name, m_AssetFilter.data()))
                    continue;
                const bool selected = choice.ID == id;
                if (ImGui::Selectable(choice.Name.c_str(), selected) && !selected)
                {
                    id      = choice.ID;
                    changed = true;
                }
            }
            ImGui::EndCombo();
        }
        if (ImGui::IsItemHovered() && id.IsValid())
            ImGui::SetTooltip("%s\nDrop a source file here to replace it.", preview.c_str());
        // Accept a source file dragged from the content browser.
        if (ImGui::BeginDragDropTarget())
        {
            if (const auto* payload = ImGui::AcceptDragDropPayload("asset_payload"))
                if (const AssetID dropped = ChoiceFor(std::filesystem::u8path(std::string(
                                                          static_cast<const char*>(payload->Data), payload->DataSize)),
                                                      type);
                    dropped.IsValid())
                {
                    changed = dropped != id;
                    id      = dropped;
                }
            ImGui::EndDragDropTarget();
        }
        ImGui::PopID();
        return changed;
    }

    std::string PropertiesPanel::MaterialName(AssetID material)
    {
        if (!material.IsValid())
            return "(default)";
        std::string name = material.ToString();
        if (m_Assets)
        {
            const auto status = m_Assets->Request<MaterialDataAsset>(material);
            if (status.IsReady())
                name = status.Asset->GetData().Name.empty() ? "(unnamed)" : status.Asset->GetData().Name;
            else if (status.State == AssetLoadState::Loading)
                name = "(loading)";
        }
        for (const auto& choice : m_AssetChoices)
            if (choice.ID == material)
                return name + "  [" + choice.SourcePath.filename().u8string() + "]";
        return name;
    }

    AssetID PropertiesPanel::ChoiceFor(const std::filesystem::path& file, AssetType type) const
    {
        std::error_code error;
        const auto      path = std::filesystem::weakly_canonical(file, error);
        if (error)
            return {};
        for (const auto& choice : m_AssetChoices)
            if (choice.Type == type && choice.SourcePath == path)
                return choice.ID;
        return {};
    }

    bool PropertiesPanel::MaterialPicker(const char* label, AssetID imported, AssetID current, AssetID& chosen)
    {
        return PropertyGrid::Row(label) && MaterialCombo(imported, current, chosen);
    }

    bool PropertiesPanel::MaterialCombo(AssetID imported, AssetID current, AssetID& chosen)
    {
        bool       picked     = false;
        const bool overridden = current != imported;
        if (overridden)
            ImGui::PushStyleColor(ImGuiCol_FrameBg, EditorTheme::Colors().Selection);
        const bool open = ImGui::BeginCombo("##material", MaterialName(current).c_str(), ImGuiComboFlags_HeightLarge);
        if (overridden)
            ImGui::PopStyleColor();
        if (ImGui::IsItemHovered())
            ImGui::SetTooltip(overridden ? "Replaced; imported: %s" : "Imported material: %s",
                              MaterialName(imported).c_str());
        if (!open)
            return false;
        FilterBox(m_AssetFilter);
        if (ImGui::Selectable(("Imported: " + MaterialName(imported)).c_str(), !overridden))
        {
            chosen = {};
            picked = true;
        }
        ImGui::Separator();
        for (const auto& choice : m_AssetChoices)
        {
            if (choice.Type != AssetType::Material)
                continue;
            const auto name = MaterialName(choice.ID);
            if (!ContainsIgnoreCase(name, m_AssetFilter.data()))
                continue;
            ImGui::PushID(choice.ID.ToString().c_str());
            if (ImGui::Selectable(name.c_str(), choice.ID == current))
            {
                chosen = choice.ID;
                picked = true;
            }
            ImGui::PopID();
        }
        ImGui::EndCombo();
        return picked;
    }

    Ref<Texture2D> PropertiesPanel::MaterialThumbnail(AssetID material)
    {
        if (!m_Thumbnail || !material.IsValid())
            return nullptr;
        for (const auto& choice : m_AssetChoices)
            if (choice.ID == material && choice.Type == AssetType::Material)
                return m_Thumbnail(choice.SourcePath, 128);
        return nullptr;
    }

    std::filesystem::path PropertiesPanel::SourceOf(AssetID asset) const
    {
        for (const auto& choice : m_AssetChoices)
            if (choice.ID == asset)
                return choice.SourcePath;
        return {};
    }

    bool PropertiesPanel::CanOpenMaterial(AssetID material)
    {
        return m_MaterialActions.Open && SourceOf(material).extension() == ".material";
    }

    void PropertiesPanel::OpenMaterial(AssetID material)
    {
        if (CanOpenMaterial(material))
            m_MaterialActions.Open(SourceOf(material));
    }

    void PropertiesPanel::RevealAsset(AssetID asset)
    {
        if (const auto file = SourceOf(asset); !file.empty() && m_MaterialActions.Reveal)
            m_MaterialActions.Reveal(file);
    }

    void PropertiesPanel::CreateMaterialInstance(AssetID parent, const SlotParts& parts)
    {
        if (const auto file = SourceOf(parent); !file.empty() && m_MaterialActions.CreateInstance)
            m_MaterialActions.CreateInstance(parent, file, parts);
    }

    AssetID PropertiesPanel::BrowserMaterial()
    {
        if (!m_BrowserSelection)
            return {};
        // The single selected material; several leave the choice open.
        AssetID found;
        for (const auto& file : m_BrowserSelection())
            if (file.extension() == ".material")
            {
                if (found.IsValid())
                    return {};
                found = ChoiceFor(file, AssetType::Material);
            }
        return found;
    }

    AssetID PropertiesPanel::AcceptMaterialDrop()
    {
        AssetID dropped;
        if (!ImGui::BeginDragDropTarget())
            return dropped;
        // Only materials light the row up; other files are left to the targets that take them.
        const auto* dragged = ImGui::GetDragDropPayload();
        if (dragged && dragged->IsDataType("asset_payload"))
        {
            const auto file =
                std::filesystem::u8path(std::string(static_cast<const char*>(dragged->Data), dragged->DataSize));
            if (file.extension() == ".material" && ImGui::AcceptDragDropPayload("asset_payload"))
                dropped = ChoiceFor(file, AssetType::Material);
        }
        ImGui::EndDragDropTarget();
        return dropped;
    }

    bool PropertiesPanel::IsSlotHighlighted(const SlotParts& parts)
    {
        return std::find(m_HighlightedSlots.begin(), m_HighlightedSlots.end(), parts) != m_HighlightedSlots.end();
    }

    void PropertiesPanel::ToggleSlotHighlight(const SlotParts& parts)
    {
        const auto found = std::find(m_HighlightedSlots.begin(), m_HighlightedSlots.end(), parts);
        if (found != m_HighlightedSlots.end())
            m_HighlightedSlots.erase(found);
        else if (!parts.empty())
            m_HighlightedSlots.push_back(parts);
    }

    void PropertiesPanel::ToggleSlotIsolate(const SlotParts& parts)
    {
        m_IsolatedSlot = IsSlotIsolated(parts) ? SlotParts {} : parts;
    }

    void PropertiesPanel::AppendSlotHighlights(ViewHighlights& highlights) const
    {
        const auto add = [&](const SlotParts& parts, glm::vec4 color) {
            for (const auto& [entity, slot] : parts)
                highlights.Highlights.push_back({entity, slot, color});
        };
        for (const auto& parts : m_HighlightedSlots)
            add(parts, Widgets::SlotPinnedColor);
        add(m_HoveredSlot, Widgets::SlotHoverColor);
        highlights.Isolate = m_IsolatedSlot;
    }
} // namespace Kans
