#include "ProductImportPanel.h"
#include <imgui.h>
#include <algorithm>
#include <chrono>
#include <cmath>
#include <cstdio>
#include <map>
#include <misc/cpp/imgui_stdlib.h>
#include "Kans3D/Animation/AnimationProduct.h"
#include "Kans3D/Asset/Storage/ProductFiles.h"
#include "Kans3D/ImGui/EditorTheme.h"
#include "UI/EditorIcons.h"
#include "UI/PropertyGrid.h"

namespace Kans
{
    namespace
    {
        const char* StateName(EditorAssetImportState state)
        {
            switch (state)
            {
                case EditorAssetImportState::Idle:
                    return "Idle";
                case EditorAssetImportState::Building:
                    return "Importing";
                case EditorAssetImportState::Succeeded:
                    return "Imported";
                case EditorAssetImportState::Failed:
                    return "Failed";
                case EditorAssetImportState::Cancelled:
                    return "Cancelled";
            }
            return "Unknown";
        }
        const char* PhaseName(PublicationPhase phase)
        {
            switch (phase)
            {
                case PublicationPhase::Validated:
                    return "Checking the imported data";
                case PublicationPhase::ProductsWritten:
                    return "Writing files";
                case PublicationPhase::BeforeCommit:
                    return "Publishing";
                case PublicationPhase::Committed:
                    return "Published";
            }
            return "Working";
        }
        const char* TypeName(AssetType type, size_t count)
        {
            const bool one = count == 1;
            switch (type)
            {
                case AssetType::Texture:
                    return one ? "texture" : "textures";
                case AssetType::Material:
                    return one ? "material" : "materials";
                case AssetType::StaticMesh:
                case AssetType::Mesh:
                    return one ? "mesh" : "meshes";
                case AssetType::Model:
                    return one ? "model" : "models";
                case AssetType::Shader:
                    return one ? "shader" : "shaders";
                case AssetType::Skeleton:
                    return one ? "skeleton" : "skeletons";
                case AssetType::AnimationClip:
                    return one ? "animation" : "animations";
                case AssetType::AudioClip:
                    return one ? "sound" : "sounds";
                default:
                    return one ? "file" : "files";
            }
        }
        // Text for a number setting: short and stable, so an untouched value still equals its default.
        std::string Number(double value)
        {
            char text[32];
            std::snprintf(text, sizeof(text), "%.6g", value);
            return text;
        }
        // A slot key is "material:<name length>:<name>"; show the name, or a number for an unnamed slot.
        std::string SlotName(const std::string& key, const std::string& name, size_t index)
        {
            if (!name.empty())
                return name;
            const auto colon = key.find(':', 9);
            if (key.compare(0, 9, "material:") == 0 && colon != std::string::npos && colon + 1 < key.size())
                return key.substr(colon + 1);
            return "Slot " + std::to_string(index + 1);
        }
        // The importer keys clip overrides by "clip:<name length>:<name>".
        std::string ClipKey(const std::string& name)
        {
            return "animation.compression.clip:" + std::to_string(name.size()) + ":" + name;
        }
        void Hint(const char* text)
        {
            ImGui::PushStyleColor(ImGuiCol_Text, EditorTheme::Colors().TextDim);
            ImGui::TextWrapped("%s", text);
            ImGui::PopStyleColor();
        }
        bool PrimaryButton(const char* label, ImVec2 size = {0, 0})
        {
            const auto& p = EditorTheme::Colors();
            ImGui::PushStyleColor(ImGuiCol_Button, p.Accent);
            ImGui::PushStyleColor(ImGuiCol_ButtonHovered, p.AccentHover);
            ImGui::PushStyleColor(ImGuiCol_ButtonActive, p.AccentPressed);
            const bool clicked = ImGui::Button(label, size);
            ImGui::PopStyleColor(3);
            return clicked;
        }
        // Right-aligned buttons: call with the total width first.
        void AlignRight(float width)
        {
            const float avail = ImGui::GetContentRegionAvail().x;
            if (avail > width)
                ImGui::SetCursorPosX(ImGui::GetCursorPosX() + avail - width);
        }
        float ButtonWidth(const char* label)
        {
            return ImGui::CalcTextSize(label, nullptr, true).x + ImGui::GetStyle().FramePadding.x * 2.0f;
        }
    } // namespace

    void ProductImportPanel::Bind(EditorAssetImportService* service)
    {
        m_Service = service;
        m_Sources.clear();
        m_DeletionPlan.reset();
        m_Selected = {};
        m_Draft.Reset({}, {});
        m_Error.clear();
        m_Tracking = false;
        if (service)
            ReloadSources();
    }

    void ProductImportPanel::Record(const std::variant<std::monostate, AssetError>& result)
    {
        m_Error.clear();
        if (const auto* error = std::get_if<AssetError>(&result))
            m_Error = error->Message;
    }

    void ProductImportPanel::ReloadSources()
    {
        m_DeletionPlan.reset();
        auto result = m_Service->GetSources();
        if (const auto* error = std::get_if<AssetError>(&result))
        {
            m_Error = error->Message;
            return;
        }
        m_Sources = std::get<std::vector<SourceAssetMetadata>>(std::move(result));
        std::sort(m_Sources.begin(), m_Sources.end(), [](const auto& a, const auto& b) {
            return a.Path.ToString() < b.Path.ToString();
        });
        for (const auto& source : m_Sources)
            if (source.sourceAssetID == m_Selected)
            {
                Select(source);
                return;
            }
        m_Selected = {};
        m_Draft.Reset({}, {});
    }

    void ProductImportPanel::Select(const SourceAssetMetadata& source)
    {
        m_DeletionPlan.reset();
        m_Selected = source.sourceAssetID;
        m_Importer = source.Import.Importer;
        ImportSettingsSchema schema;
        if (auto found = m_Service->GetSettingsSchema(source.sourceAssetID);
            std::holds_alternative<ImportSettingsSchema>(found))
            schema = std::get<ImportSettingsSchema>(std::move(found));
        m_Draft.Reset(std::move(schema), source.Import.Settings);
        m_NewKey.clear();
        m_NewValue.clear();
        m_MoveDestination = source.Path.ToString();
        m_SlotsStale      = true;
        m_InventoryStale  = true;
        m_Inventory.reset();
        m_ExtractReport.clear();
        // Extracted materials go next to the model by default.
        m_ExtractDirectory = (source.Path.GetPath().parent_path() / "Materials").generic_u8string();
        // A new file starts by showing what it contains; a reimport starts at its settings.
        m_Stage = IsImported() ? Stage::Configure : Stage::Inspect;
    }

    const SourceAssetMetadata* ProductImportPanel::Selected() const
    {
        for (const auto& source : m_Sources)
            if (source.sourceAssetID == m_Selected)
                return &source;
        return nullptr;
    }

    std::filesystem::path ProductImportPanel::SelectedFile() const
    {
        const auto* source = Selected();
        return source ? m_Service->GetConfig().AssetDirectory / source->Path.GetPath() : std::filesystem::path();
    }

    bool ProductImportPanel::IsImported() const
    {
        if (!m_Service || !m_Selected.IsValid())
            return false;
        const auto registry = m_Service->GetRuntimeAssetRegistry();
        if (!registry)
            return false;
        for (const auto& product : registry->GetAllProducts())
            if (product.Target == EditorAssetTarget && product.Metadata.assetID.GetSourceID() == m_Selected)
                return true;
        return false;
    }

    void ProductImportPanel::Track(const EditorAssetImportSnapshot& status)
    {
        const bool building = status.State == EditorAssetImportState::Building;
        if (building && status.Source == m_Selected)
        {
            m_Tracking = true;
            m_Stage    = Stage::Import;
        }
        if (m_Tracking && !building && m_LastState == EditorAssetImportState::Building)
        {
            m_Tracking = false;
            ReloadSources(); // The saved settings, the slots and the contents may have changed.
            m_Stage = Stage::Result;
        }
        m_LastState = status.State;
    }

    void ProductImportPanel::Import()
    {
        SourceImportConfiguration configuration;
        configuration.Importer = m_Importer;
        configuration.Settings = m_Draft.Settings();
        if (!configuration.IsValid())
        {
            m_Error = "The settings contain an invalid key or value; fix them in the Developer section.";
            return;
        }
        Record(m_Service->SetSourceImportConfiguration(m_Selected, configuration));
        if (!m_Error.empty())
            return;
        m_Draft.Reset(m_Draft.Schema(), m_Draft.Settings()); // Saved now.
        AssetBuildOptions options;
        options.Target = EditorAssetTarget;
        options.Force  = m_Force;
        Record(m_Service->Start(m_Selected, std::move(options)));
        if (m_Error.empty())
        {
            m_Tracking = true;
            m_Stage    = Stage::Import;
        }
    }

    void ProductImportPanel::GoTo(Stage stage)
    {
        if ((stage == Stage::Inspect || stage == Stage::Configure) && !m_Tracking && m_Selected.IsValid())
            m_Stage = stage;
    }

    void ProductImportPanel::OpenWithFile(const std::filesystem::path& path)
    {
        if (!m_Service)
            return;
        std::error_code error;
        const auto      absolute = std::filesystem::absolute(path, error).lexically_normal();
        if (error)
        {
            m_Error = error.message();
            return;
        }
        ReloadSources();
        for (const auto& source : m_Sources)
        {
            const auto full = m_Service->GetConfig().AssetDirectory / source.Path.GetPath();
            error.clear();
            if (source.Exists && !source.Missing && std::filesystem::equivalent(full, absolute, error) && !error)
            {
                Select(source);
                m_Error.clear();
                return;
            }
        }
        m_Error = "File is not a registered source. Place it under the project asset directory and scan sources.";
    }

    // ---------------------------------------------------------------------- window

    void ProductImportPanel::Draw()
    {
        if (!m_Service)
        {
            ImGui::TextUnformatted("Project is unavailable.");
            return;
        }
        auto result = m_Service->GetSnapshot();
        if (const auto* error = std::get_if<AssetError>(&result))
        {
            ImGui::TextWrapped("%s", error->Message.c_str());
            return;
        }
        const auto& status = std::get<EditorAssetImportSnapshot>(result);
        const bool  busy   = status.State == EditorAssetImportState::Building;
        Track(status);
        DrawHeader(busy);
        if (!m_Selected.IsValid())
        {
            Hint("Choose a source file above, or use Import / Import Settings on a file in the Content Browser.");
            if (!m_Error.empty())
                ImGui::TextWrapped("%s", m_Error.c_str());
            return;
        }
        DrawSteps(busy);
        const float footer = ImGui::GetFrameHeightWithSpacing() + EditorTheme::Px(10.0f) +
                             (m_Error.empty() ? 0.0f : ImGui::GetTextLineHeightWithSpacing() * 2.0f);
        if (ImGui::BeginChild("##Stage", {0, -footer}, ImGuiChildFlags_None))
        {
            switch (m_Stage)
            {
                case Stage::Inspect:
                    DrawInspect(busy);
                    break;
                case Stage::Configure:
                    DrawConfigure(busy);
                    break;
                case Stage::Import:
                    DrawImport(status);
                    break;
                case Stage::Result:
                    DrawResult(status);
                    break;
            }
        }
        ImGui::EndChild();
        DrawFooter(status, busy);
    }

    void ProductImportPanel::DrawHeader(bool busy)
    {
        const auto* selected = Selected();
        const auto  label    = selected ? selected->Path.ToString() : std::string("Choose a source file");
        ImGui::BeginDisabled(busy);
        ImGui::SetNextItemWidth(-ButtonWidth(ICON_FA_ROTATE_RIGHT) - ImGui::GetStyle().ItemSpacing.x);
        if (ImGui::BeginCombo("##Source", (ICON_FA_FILE "  " + label).c_str(), ImGuiComboFlags_HeightLarge))
        {
            if (ImGui::IsWindowAppearing())
                ImGui::SetKeyboardFocusHere();
            ImGui::SetNextItemWidth(-FLT_MIN);
            ImGui::InputTextWithHint("##Filter", ICON_FA_MAGNIFYING_GLASS "  Filter", &m_SourceFilter);
            for (const auto& source : m_Sources)
            {
                const auto path = source.Path.ToString();
                if (!m_SourceFilter.empty())
                {
                    auto lower = [](std::string text) {
                        std::transform(text.begin(), text.end(), text.begin(), [](unsigned char c) {
                            return char(std::tolower(c));
                        });
                        return text;
                    };
                    if (lower(path).find(lower(m_SourceFilter)) == std::string::npos)
                        continue;
                }
                ImGui::PushID(source.sourceAssetID.ToString().c_str());
                if (ImGui::Selectable((path + (source.Missing ? "  (missing)" : "")).c_str(),
                                      source.sourceAssetID == m_Selected))
                    Select(source);
                ImGui::PopID();
            }
            ImGui::EndCombo();
        }
        ImGui::SameLine();
        if (ImGui::Button(ICON_FA_ROTATE_RIGHT))
        {
            Record(m_Service->RefreshSources());
            if (m_Error.empty())
                ReloadSources();
        }
        if (ImGui::IsItemHovered(ImGuiHoveredFlags_AllowWhenDisabled))
            ImGui::SetTooltip("Scan the project for new, moved or deleted source files");
        ImGui::EndDisabled();
    }

    void ProductImportPanel::DrawSteps(bool busy)
    {
        // Numbered steps; the first two can be revisited, the last two follow the import.
        static constexpr const char* names[] = {"Inspect", "Configure", "Import", "Result"};
        const auto&                  p       = EditorTheme::Colors();
        ImGui::Dummy({0, EditorTheme::Px(4.0f)});
        const float width = (ImGui::GetContentRegionAvail().x - 3 * ImGui::GetStyle().ItemSpacing.x) / 4.0f;
        for (int i = 0; i < 4; ++i)
        {
            if (i)
                ImGui::SameLine();
            const auto stage   = Stage(i);
            const bool current = stage == m_Stage;
            const bool done    = int(m_Stage) > i;
            const bool open    = !busy && (stage == Stage::Inspect || stage == Stage::Configure);
            ImGui::PushID(i);
            ImGui::BeginDisabled(!open && !current);
            const ImVec2 at      = ImGui::GetCursorScreenPos();
            const bool   clicked = ImGui::InvisibleButton("##step", {width, ImGui::GetFrameHeight()});
            ImGui::EndDisabled();
            const bool hovered = ImGui::IsItemHovered();
            ImGui::PopID();
            if (clicked && open)
                GoTo(stage);
            auto*        draw = ImGui::GetWindowDrawList();
            const ImVec2 end {at.x + width, at.y + ImGui::GetFrameHeight()};
            draw->AddRectFilled(at,
                                end,
                                ImGui::GetColorU32(current ? p.Selection :
                                                   hovered ? p.Hover :
                                                             p.Raised),
                                EditorTheme::Px(4.0f));
            // A bar under the step: accent for the current one, dim for the ones passed.
            if (current || done)
                draw->AddRectFilled({at.x, end.y - EditorTheme::Px(2.0f)},
                                    end,
                                    ImGui::GetColorU32(current ? p.Accent : p.Line),
                                    EditorTheme::Px(1.0f));
            char text[32];
            std::snprintf(text, sizeof(text), "%d  %s", i + 1, names[i]);
            const ImVec2 size = ImGui::CalcTextSize(text);
            draw->AddText({at.x + (width - size.x) * 0.5f, at.y + (ImGui::GetFrameHeight() - size.y) * 0.5f},
                          ImGui::GetColorU32(current        ? p.Text :
                                             (open || done) ? p.TextDim :
                                                              p.TextOff),
                          text);
        }
        ImGui::Dummy({0, EditorTheme::Px(4.0f)});
    }

    void ProductImportPanel::DrawFooter(const EditorAssetImportSnapshot& status, bool busy)
    {
        if (!m_Error.empty())
        {
            ImGui::PushStyleColor(ImGuiCol_Text, EditorTheme::Colors().Danger);
            ImGui::TextWrapped("%s", m_Error.c_str());
            ImGui::PopStyleColor();
        }
        ImGui::Separator();
        const auto& style = ImGui::GetStyle();
        switch (m_Stage)
        {
            case Stage::Inspect: {
                const char* next = "Next: Configure  " ICON_FA_ARROW_RIGHT;
                AlignRight(ButtonWidth(next));
                if (PrimaryButton(next))
                    m_Stage = Stage::Configure;
                break;
            }
            case Stage::Configure: {
                const auto  changes = m_Draft.Changes().size();
                const char* action  = IsImported() ? ICON_FA_FILE_IMPORT "  Reimport" : ICON_FA_FILE_IMPORT "  Import";
                if (changes)
                {
                    ImGui::AlignTextToFramePadding();
                    ImGui::TextColored(
                        EditorTheme::Colors().AccentHover, "%zu unsaved change%s", changes, changes == 1 ? "" : "s");
                    ImGui::SameLine();
                    if (ImGui::Button("Revert"))
                        m_Draft.Revert();
                    ImGui::SameLine();
                }
                AlignRight(ButtonWidth(action));
                ImGui::BeginDisabled(busy);
                if (PrimaryButton(action))
                    Import();
                ImGui::EndDisabled();
                if (ImGui::IsItemHovered(ImGuiHoveredFlags_AllowWhenDisabled))
                    ImGui::SetTooltip(busy ? "Another import is running" : "Save these settings and import the file");
                break;
            }
            case Stage::Import: {
                const char* cancel = "Cancel";
                AlignRight(ButtonWidth(cancel));
                ImGui::BeginDisabled(status.CancelRequested || !busy);
                if (ImGui::Button(cancel))
                    Record(m_Service->Cancel());
                ImGui::EndDisabled();
                break;
            }
            case Stage::Result: {
                const bool  model    = m_Draft.Schema().Find("include.meshes") != nullptr;
                const bool  material = SelectedFile().extension() == ".material";
                const bool  ok       = status.State == EditorAssetImportState::Succeeded;
                const char* back     = ICON_FA_ARROW_LEFT "  Settings";
                const char* place    = ICON_FA_CUBE "  Place in scene";
                const char* open     = ICON_FA_PALETTE "  Open material";
                float       width    = ButtonWidth(back);
                if (ok && model && m_Place)
                    width += ButtonWidth(place) + style.ItemSpacing.x;
                if (ok && material && m_OpenMaterial)
                    width += ButtonWidth(open) + style.ItemSpacing.x;
                AlignRight(width);
                if (ImGui::Button(back))
                    m_Stage = Stage::Configure;
                if (ok && model && m_Place)
                {
                    ImGui::SameLine();
                    if (PrimaryButton(place))
                        m_Place(SelectedFile());
                }
                if (ok && material && m_OpenMaterial)
                {
                    ImGui::SameLine();
                    if (PrimaryButton(open))
                        m_OpenMaterial(SelectedFile());
                }
                break;
            }
        }
    }

    // ---------------------------------------------------------------------- inspect

    void ProductImportPanel::DrawInspect(bool busy)
    {
        if (m_InventoryStale)
        {
            if (busy)
            {
                Hint("The file is read once the running import finishes.");
                return;
            }
            // Show a frame first: reading a large model takes a moment.
            if (!m_InventoryShown)
            {
                m_InventoryShown = true;
                Hint("Reading the file...");
                return;
            }
            m_InventoryShown = false;
            m_InventoryStale = false;
            m_Inventory.reset();
            m_InventoryError.clear();
            auto inspected = m_Service->InspectSource(m_Selected);
            if (auto* inventory = std::get_if<SourceInventory>(&inspected))
                m_Inventory = std::move(*inventory);
            else
                m_InventoryError = std::get<AssetError>(inspected).Message;
        }
        if (!m_Inventory)
        {
            Hint(m_InventoryError.empty() ? "Nothing to inspect." : m_InventoryError.c_str());
            Hint("Continue to Configure to choose the import settings.");
            return;
        }
        if (m_Inventory->Texture)
            DrawTextureContents(*m_Inventory->Texture);
        else
            DrawModelContents(*m_Inventory);
        if (!m_Inventory->Warnings.empty() && PropertyGrid::BeginGroup("Warnings"))
        {
            PropertyGrid::BeginFree();
            for (const auto& warning : m_Inventory->Warnings)
            {
                ImGui::TextColored(EditorTheme::Colors().Warning, ICON_FA_TRIANGLE_EXCLAMATION);
                ImGui::SameLine();
                ImGui::TextWrapped("%s", warning.c_str());
            }
            PropertyGrid::EndFree();
            PropertyGrid::EndGroup();
        }
    }

    void ProductImportPanel::DrawModelContents(const SourceInventory& inventory)
    {
        const auto& p = EditorTheme::Colors();
        // Sections of one mesh share its name; the importer includes or leaves out a name.
        struct Mesh
        {
            uint64_t Vertices = 0, Triangles = 0;
            bool     Skinned = false;
            uint32_t Morphs  = 0;
        };
        std::map<std::string, Mesh> meshes;
        for (const auto& mesh : inventory.Meshes)
        {
            auto& entry = meshes[mesh.Name];
            entry.Vertices += mesh.VertexCount;
            entry.Triangles += mesh.TriangleCount;
            entry.Skinned |= mesh.Skinned;
            entry.Morphs = std::max(entry.Morphs, mesh.MorphTargetCount);
        }
        const size_t missing = std::count_if(inventory.Textures.begin(),
                                             inventory.Textures.end(),
                                             [](const auto& texture) { return !texture.Found && !texture.Embedded; });

        // What to import, by category, with what the file has of each.
        if (PropertyGrid::BeginGroup("Contents"))
        {
            const auto category = [&](const char* key, const char* label, const std::string& count) {
                bool on = m_Draft.GetBool(key);
                if (!PropertyGrid::Row(label,
                                       m_Draft.Schema().Find(key) ? m_Draft.Schema().Find(key)->Description.c_str() :
                                                                    nullptr,
                                       !on))
                    return;
                ImGui::PushID(key);
                if (ImGui::Checkbox("##on", &on))
                    m_Draft.SetBool(key, on);
                ImGui::SameLine();
                ImGui::TextColored(p.TextDim, "%s", count.c_str());
                ImGui::PopID();
            };
            category("include.meshes", "Meshes", std::to_string(meshes.size()) + " in file");
            category("include.materials",
                     "Materials",
                     std::to_string(inventory.Materials.size()) + " slots, " +
                         std::to_string(inventory.Textures.size()) + " textures" +
                         (missing ? " (" + std::to_string(missing) + " missing)" : ""));
            category("include.skeleton",
                     "Skeleton",
                     inventory.Skeleton ? std::to_string(inventory.Skeleton->JointCount) + " joints" : "none");
            category("include.animations", "Animations", std::to_string(inventory.Clips.size()) + " clips");
            category("include.morph_targets",
                     "Morph targets",
                     std::to_string(inventory.MorphTargets.size()) + " shape keys");
            PropertyGrid::EndGroup();
        }

        const auto table = [](const char* id, int columns) {
            return ImGui::BeginTable(
                id, columns, ImGuiTableFlags_RowBg | ImGuiTableFlags_BordersInnerV | ImGuiTableFlags_SizingStretchProp);
        };
        if (!meshes.empty() && PropertyGrid::BeginGroup(("Meshes (" + std::to_string(meshes.size()) + ")").c_str()))
        {
            PropertyGrid::BeginFree();
            ImGui::BeginDisabled(!m_Draft.GetBool("include.meshes"));
            if (table("##Meshes", 4))
            {
                ImGui::TableSetupColumn("Mesh", ImGuiTableColumnFlags_WidthStretch, 3.0f);
                ImGui::TableSetupColumn("Vertices", ImGuiTableColumnFlags_WidthStretch, 1.0f);
                ImGui::TableSetupColumn("Triangles", ImGuiTableColumnFlags_WidthStretch, 1.0f);
                ImGui::TableSetupColumn("Deforms", ImGuiTableColumnFlags_WidthStretch, 1.2f);
                ImGui::TableHeadersRow();
                for (const auto& [name, mesh] : meshes)
                {
                    ImGui::PushID(name.c_str());
                    ImGui::TableNextRow();
                    ImGui::TableNextColumn();
                    const auto key = "include.mesh." + name;
                    bool       on  = m_Draft.GetBool(key);
                    if (ImGui::Checkbox(name.empty() ? "(unnamed)" : name.c_str(), &on))
                        m_Draft.SetBool(key, on);
                    ImGui::TableNextColumn();
                    ImGui::Text("%llu", static_cast<unsigned long long>(mesh.Vertices));
                    ImGui::TableNextColumn();
                    ImGui::Text("%llu", static_cast<unsigned long long>(mesh.Triangles));
                    ImGui::TableNextColumn();
                    std::string deforms = mesh.Skinned ? "skinned" : "";
                    if (mesh.Morphs)
                        deforms += (deforms.empty() ? "" : ", ") + std::to_string(mesh.Morphs) + " morphs";
                    ImGui::TextColored(p.TextDim, "%s", deforms.empty() ? "-" : deforms.c_str());
                    ImGui::PopID();
                }
                ImGui::EndTable();
            }
            ImGui::EndDisabled();
            PropertyGrid::EndFree();
            PropertyGrid::EndGroup();
        }
        if (!inventory.Clips.empty() &&
            PropertyGrid::BeginGroup(("Animations (" + std::to_string(inventory.Clips.size()) + ")").c_str()))
        {
            PropertyGrid::BeginFree();
            ImGui::BeginDisabled(!m_Draft.GetBool("include.animations"));
            if (table("##Clips", 3))
            {
                ImGui::TableSetupColumn("Clip", ImGuiTableColumnFlags_WidthStretch, 3.0f);
                ImGui::TableSetupColumn("Length", ImGuiTableColumnFlags_WidthStretch, 1.0f);
                ImGui::TableSetupColumn("Keys", ImGuiTableColumnFlags_WidthStretch, 1.0f);
                ImGui::TableHeadersRow();
                for (const auto& clip : inventory.Clips)
                {
                    ImGui::PushID(clip.ObjectKey.c_str());
                    ImGui::TableNextRow();
                    ImGui::TableNextColumn();
                    const auto key = "include.clip." + clip.Name;
                    bool       on  = m_Draft.GetBool(key);
                    if (ImGui::Checkbox(clip.Name.empty() ? "(unnamed)" : clip.Name.c_str(), &on))
                        m_Draft.SetBool(key, on);
                    ImGui::TableNextColumn();
                    if (clip.FrameRate > 0)
                        ImGui::Text("%.2f s @ %.0f fps", clip.DurationSeconds, clip.FrameRate);
                    else
                        ImGui::Text("%.2f s", clip.DurationSeconds);
                    ImGui::TableNextColumn();
                    ImGui::TextColored(p.TextDim, "%llu", static_cast<unsigned long long>(clip.KeyCount));
                    ImGui::PopID();
                }
                ImGui::EndTable();
            }
            ImGui::EndDisabled();
            PropertyGrid::EndFree();
            PropertyGrid::EndGroup();
        }
        if (!inventory.Textures.empty() &&
            PropertyGrid::BeginGroup(("Textures (" + std::to_string(inventory.Textures.size()) + ")").c_str()))
        {
            PropertyGrid::BeginFree();
            if (missing)
                Hint("Missing textures import as defaults. Put the files where the paths point, then reimport.");
            if (table("##Textures", 3))
            {
                ImGui::TableSetupColumn("##State", ImGuiTableColumnFlags_WidthFixed, ImGui::GetFrameHeight());
                ImGui::TableSetupColumn("Texture", ImGuiTableColumnFlags_WidthStretch, 3.0f);
                ImGui::TableSetupColumn("Material", ImGuiTableColumnFlags_WidthStretch, 1.5f);
                ImGui::TableHeadersRow();
                for (const auto& texture : inventory.Textures)
                {
                    ImGui::TableNextRow();
                    ImGui::TableNextColumn();
                    if (texture.Embedded)
                        ImGui::TextColored(p.TextDim, ICON_FA_FILE);
                    else if (texture.Found)
                        ImGui::TextColored(p.Success, ICON_FA_CHECK);
                    else
                        ImGui::TextColored(p.Danger, ICON_FA_TRIANGLE_EXCLAMATION);
                    if (ImGui::IsItemHovered())
                        ImGui::SetTooltip(texture.Embedded ? "Embedded in the file" :
                                          texture.Found    ? "Found" :
                                                             "Missing");
                    ImGui::TableNextColumn();
                    ImGui::TextUnformatted(texture.Reference.c_str());
                    if (ImGui::IsItemHovered() && !texture.ResolvedPath.empty())
                        ImGui::SetTooltip(
                            "%s%s", texture.Found ? "" : "Expected at ", texture.ResolvedPath.u8string().c_str());
                    ImGui::TableNextColumn();
                    ImGui::TextColored(p.TextDim, "%s", SlotName(texture.MaterialSlot, {}, 0).c_str());
                }
                ImGui::EndTable();
            }
            PropertyGrid::EndFree();
            PropertyGrid::EndGroup();
        }
        if (inventory.Skeleton && PropertyGrid::BeginGroup("Skeleton"))
        {
            PropertyGrid::Text("Joints", std::to_string(inventory.Skeleton->JointCount));
            PropertyGrid::Text("Root", inventory.Skeleton->RootName);
            PropertyGrid::EndGroup();
        }
    }

    void ProductImportPanel::DrawTextureContents(const InspectedTexture& texture)
    {
        if (PropertyGrid::BeginGroup("Image"))
        {
            PropertyGrid::Text("Size", std::to_string(texture.Width) + " x " + std::to_string(texture.Height));
            PropertyGrid::Text("Channels",
                               std::to_string(texture.Channels) + (texture.HasAlpha ? " (with alpha)" : ""));
            PropertyGrid::Text("Format", texture.Format + ", " + std::to_string(texture.BitDepth) + "-bit");
            PropertyGrid::EndGroup();
        }
        // Offer the matching preset when the file looks like a normal map but is not set up as one.
        if (texture.LikelyNormalMap && m_Draft.Get("usage") != "normal")
            for (const auto& preset : m_Draft.Schema().Presets)
                if (preset.Settings.count("usage") && preset.Settings.at("usage") == "normal")
                {
                    ImGui::TextColored(EditorTheme::Colors().AccentHover, ICON_FA_CIRCLE_INFO);
                    ImGui::SameLine();
                    ImGui::TextWrapped("This looks like a normal map (%s).", texture.NormalMapHeuristic.c_str());
                    if (ImGui::Button(("Use the " + preset.Name + " preset").c_str()))
                        m_Draft.ApplyPreset(preset);
                    break;
                }
    }

    // ---------------------------------------------------------------------- configure

    void ProductImportPanel::DrawSetting(const ImportSettingDesc& setting)
    {
        const auto& key      = setting.Key;
        const auto  value    = m_Draft.Get(key);
        const bool  modified = !m_Draft.IsDefault(key);
        const auto& label    = setting.Label.empty() ? key : setting.Label;
        const char* tooltip  = setting.Description.empty() ? nullptr : setting.Description.c_str();
        if (!PropertyGrid::Row(label.c_str(), tooltip, modified))
            return;
        ImGui::PushID(key.c_str());
        using Type = ImportSettingDesc::Type;
        if (setting.Kind == Type::Bool)
        {
            bool on = value == "true";
            if (ImGui::Checkbox("##value", &on))
                m_Draft.SetBool(key, on);
        }
        else if (!setting.Options.empty())
        {
            std::string preview = value;
            for (const auto& option : setting.Options)
                if (option.Value == value)
                    preview = option.Label;
            if (ImGui::BeginCombo("##value", preview.c_str()))
            {
                for (const auto& option : setting.Options)
                {
                    if (ImGui::Selectable(option.Label.c_str(), option.Value == value))
                        m_Draft.Set(key, option.Value);
                    if (!option.Description.empty() && ImGui::IsItemHovered())
                        ImGui::SetTooltip("%s", option.Description.c_str());
                }
                ImGui::EndCombo();
            }
        }
        else if (setting.Kind == Type::Int || setting.Kind == Type::Float)
        {
            const double min = setting.Min.value_or(0.0), max = setting.Max.value_or(0.0);
            if (setting.Kind == Type::Int)
            {
                int number = std::atoi(value.c_str());
                if (ImGui::DragInt("##value", &number, 1.0f, int(min), int(max)))
                    m_Draft.Set(key, std::to_string(number));
            }
            else
            {
                float number = std::strtof(value.c_str(), nullptr);
                if (ImGui::DragFloat(
                        "##value", &number, float(setting.Step.value_or(0.01)), float(min), float(max), "%.6g") &&
                    std::isfinite(number))
                    m_Draft.Set(key, Number(number));
            }
        }
        else
        {
            std::string text = value;
            if (ImGui::InputText("##value", &text))
                m_Draft.Set(key, text);
        }
        if (PropertyGrid::RowReset(modified))
            m_Draft.Set(key, setting.Default);
        ImGui::PopID();
    }

    void ProductImportPanel::DrawConfigure(bool busy)
    {
        ImGui::BeginDisabled(busy);
        const auto& schema = m_Draft.Schema();
        if (!schema.Presets.empty() && PropertyGrid::Begin("##Preset") &&
            PropertyGrid::Row("Preset", "Sets several settings at once; you can change them after."))
        {
            const auto* match = m_Draft.MatchingPreset();
            if (ImGui::BeginCombo("##Preset", match ? match->Name.c_str() : "Custom"))
            {
                for (const auto& preset : schema.Presets)
                {
                    if (ImGui::Selectable(preset.Name.c_str(), &preset == match))
                        m_Draft.ApplyPreset(preset);
                    if (!preset.Description.empty() && ImGui::IsItemHovered())
                        ImGui::SetTooltip("%s", preset.Description.c_str());
                }
                ImGui::EndCombo();
            }
        }
        if (!schema.Presets.empty())
            PropertyGrid::End();
        if (schema.Settings.empty())
            Hint("This file type has no import settings.");
        const bool model = schema.Find("include.meshes") != nullptr;
        for (const auto& group : m_Draft.Groups())
        {
            // A model's contents are chosen in Inspect, next to what the file has.
            if (model && group == "Contents")
                continue;
            const auto shown = [&](const ImportSettingDesc& s) {
                return !s.KeyPattern && !s.Advanced && s.Group == group;
            };
            if (std::none_of(schema.Settings.begin(), schema.Settings.end(), shown))
                continue;
            if (PropertyGrid::BeginGroup(group.c_str()))
            {
                for (const auto& setting : schema.Settings)
                    if (shown(setting))
                        DrawSetting(setting);
                PropertyGrid::EndGroup();
            }
        }
        if (model)
        {
            DrawModelMaterials(busy);
            DrawClipOverrides(busy);
        }
        const auto advanced = [](const ImportSettingDesc& s) { return !s.KeyPattern && s.Advanced; };
        if (std::any_of(schema.Settings.begin(), schema.Settings.end(), advanced) &&
            PropertyGrid::BeginGroup("Advanced", nullptr, nullptr, false))
        {
            for (const auto& setting : schema.Settings)
                if (advanced(setting))
                    DrawSetting(setting);
            PropertyGrid::EndGroup();
        }
        ImGui::EndDisabled();
        DrawDeveloper(busy);
    }

    void ProductImportPanel::LoadSlots()
    {
        m_SlotsStale = false;
        m_Slots.reset();
        m_SlotsError.clear();
        m_ClipsLoading.reset();
        m_Clips.clear();
        if (!m_Selected.IsValid() || !IsImported())
            return;
        auto slots = m_Service->GetModelMaterialSlots(m_Selected);
        if (const auto* error = std::get_if<AssetError>(&slots))
            m_SlotsError = error->Message;
        else
            m_Slots = std::get<std::vector<ModelMaterialSlot>>(std::move(slots));
        if (!m_Slots)
            return;
        // Decoding a large clip takes a moment; the registry snapshot keeps its files alive.
        m_ClipsLoading = std::async(
            std::launch::async,
            [registry = m_Service->GetRuntimeAssetRegistry(),
             root     = m_Service->GetProductDirectory(),
             source   = m_Selected]() {
                std::vector<ClipSummary> clips;
                for (const auto& record : registry->GetAllProducts())
                {
                    const auto& id = record.Metadata.assetID;
                    if (record.Target != EditorAssetTarget || record.Metadata.Type != AssetType::AnimationClip ||
                        !(id.GetSourceID() == source))
                        continue;
                    ClipSummary clip;
                    clip.Name  = id.ToString();
                    auto bytes = ReadRegisteredProduct(root, record);
                    if (const auto* error = std::get_if<AssetError>(&bytes))
                        clip.Error = error->Message;
                    else
                    {
                        const auto& data    = std::get<std::vector<std::byte>>(bytes);
                        auto        decoded = DecodeAnimationClipProduct(data.data(), data.size());
                        if (const auto* error = std::get_if<AssetError>(&decoded))
                            clip.Error = error->Message;
                        else
                        {
                            auto& c         = std::get<AnimationClipData>(decoded);
                            clip.Name       = c.Name;
                            clip.Statistics = std::move(c.Compression);
                        }
                    }
                    clips.push_back(std::move(clip));
                }
                std::sort(clips.begin(), clips.end(), [](const auto& a, const auto& b) { return a.Name < b.Name; });
                return clips;
            });
    }

    void ProductImportPanel::DrawModelMaterials(bool busy)
    {
        if (!busy && m_SlotsStale)
            LoadSlots();
        // The slots of the built model, or before the first import the ones the file declares.
        struct Slot
        {
            std::string            Key, Name;
            std::optional<AssetID> Suggested;
        };
        std::vector<Slot> slots;
        if (m_Slots)
            for (const auto& slot : *m_Slots)
                slots.push_back({slot.Key, slot.DisplayName, {}});
        else if (m_Inventory)
            for (const auto& slot : m_Inventory->Materials)
                slots.push_back({slot.Key, slot.SourceName, {}});
        if (m_Inventory)
            for (auto& slot : slots)
                for (const auto& inspected : m_Inventory->Materials)
                    if (inspected.Key == slot.Key)
                        slot.Suggested = inspected.SuggestedMaterial;
        if (slots.empty() || !PropertyGrid::BeginGroup(("Materials (" + std::to_string(slots.size()) + ")").c_str()))
            return;

        // Project materials a slot can use: published Material Products of .material sources.
        std::vector<std::pair<AssetID, std::string>> choices;
        const auto                                   registry = m_Service->GetRuntimeAssetRegistry();
        for (const auto& source : m_Sources)
        {
            if (source.Missing || source.Path.GetPath().extension() != ".material")
                continue;
            for (const auto& product : registry->GetAllProducts())
                if (product.Metadata.assetID.GetSourceID() == source.sourceAssetID &&
                    product.Metadata.Type == AssetType::Material && product.Target == EditorAssetTarget)
                    choices.emplace_back(product.Metadata.assetID, source.Path.GetPath().filename().u8string());
        }
        const auto nameOf = [&](AssetID id) {
            for (const auto& [choice, name] : choices)
                if (choice == id)
                    return name;
            // A remap to a source that is not published (yet): name it by its file.
            for (const auto& source : m_Sources)
                if (source.sourceAssetID == id.GetSourceID())
                    return source.Path.GetPath().filename().u8string() +
                           (source.Missing ? " (missing)" : " (not built)");
            return id.ToString() + " (unavailable)";
        };
        Hint("Each slot imports the file's material, or uses a project material for every use of this model.");
        for (const auto& slot : slots)
        {
            const auto    key      = "material." + slot.Key;
            const auto    value    = m_Draft.Get(key);
            const AssetID remap    = value.empty() ? AssetID {} : AssetID::FromString(value);
            const bool    modified = !value.empty();
            const auto    name     = SlotName(slot.Key, slot.Name, size_t(&slot - slots.data()));
            if (!PropertyGrid::Row(name.c_str(), nullptr, modified))
                continue;
            ImGui::PushID(slot.Key.c_str());
            const bool suggest = slot.Suggested && !modified;
            if (suggest)
                ImGui::SetNextItemWidth(ImGui::GetContentRegionAvail().x - ImGui::GetFrameHeight() -
                                        ImGui::GetStyle().ItemSpacing.x);
            const std::string current = remap.IsValid() ? nameOf(remap) : "Imported from the file";
            if (ImGui::BeginCombo("##material", current.c_str(), ImGuiComboFlags_HeightLarge))
            {
                if (ImGui::Selectable("Imported from the file", !remap.IsValid()))
                    m_Draft.Set(key, "");
                if (!choices.empty())
                    ImGui::Separator();
                for (const auto& [id, name] : choices)
                    if (ImGui::Selectable(name.c_str(), remap == id))
                        m_Draft.Set(key, id.ToString());
                ImGui::EndCombo();
            }
            if (suggest)
            {
                ImGui::SameLine();
                if (ImGui::Button(ICON_FA_LIGHTBULB, {ImGui::GetFrameHeight(), 0}))
                    m_Draft.Set(key, slot.Suggested->ToString());
                if (ImGui::IsItemHovered())
                    ImGui::SetTooltip("Use %s (same name)", nameOf(*slot.Suggested).c_str());
            }
            if (PropertyGrid::RowReset(modified))
                m_Draft.Set(key, "");
            ImGui::PopID();
        }

        // Extraction writes .material files and saves remaps itself, so it waits for a saved draft.
        PropertyGrid::BeginFree();
        if (m_Slots)
        {
            const bool remaining =
                std::any_of(m_Slots->begin(), m_Slots->end(), [](const auto& slot) { return !slot.Remap; });
            ImGui::BeginDisabled(busy || !remaining || m_Draft.IsDirty());
            ImGui::AlignTextToFramePadding();
            ImGui::TextUnformatted("Extract to");
            ImGui::SameLine();
            ImGui::SetNextItemWidth(ImGui::GetContentRegionAvail().x - ButtonWidth("Extract all") -
                                    ImGui::GetStyle().ItemSpacing.x);
            ImGui::InputText("##ExtractDirectory", &m_ExtractDirectory);
            if (ImGui::IsItemHovered(ImGuiHoveredFlags_AllowWhenDisabled))
                ImGui::SetTooltip("Folder for extracted materials, relative to the asset directory.");
            ImGui::SameLine();
            if (ImGui::Button("Extract all"))
            {
                m_ExtractReport.clear();
                auto result = m_Service->ExtractModelMaterials(m_Selected, std::filesystem::u8path(m_ExtractDirectory));
                if (const auto* error = std::get_if<AssetError>(&result))
                    m_Error = error->Message;
                else
                {
                    const auto& extracted = std::get<ModelMaterialExtractionResult>(result);
                    for (const auto& created : extracted.Created)
                        m_ExtractReport.push_back("Created " + created.File.filename().u8string());
                    if (!extracted.Skipped.empty())
                        m_ExtractReport.push_back(std::to_string(extracted.Skipped.size()) +
                                                  " slot(s) already use project materials");
                    m_Error = extracted.Error ? extracted.Error->Message : std::string();
                    if (extracted.RemapsApplied && !extracted.Created.empty())
                    {
                        ReloadSources(); // The remaps are saved; rebuild with them.
                        Import();
                    }
                }
                m_SlotsStale = true;
            }
            ImGui::EndDisabled();
            if (ImGui::IsItemHovered(ImGuiHoveredFlags_AllowWhenDisabled))
                ImGui::SetTooltip(m_Draft.IsDirty() ? "Import or revert the unsaved changes first." :
                                                      "Write the imported materials to editable .material files "
                                                      "and use them.");
            for (const auto& line : m_ExtractReport)
                ImGui::TextDisabled("%s", line.c_str());
        }
        PropertyGrid::EndFree();
        PropertyGrid::EndGroup();
    }

    void ProductImportPanel::DrawClipOverrides(bool busy)
    {
        using namespace std::chrono_literals;
        if (m_ClipsLoading && m_ClipsLoading->wait_for(0s) == std::future_status::ready)
        {
            m_Clips = m_ClipsLoading->get();
            m_ClipsLoading.reset();
        }
        if ((m_Clips.empty() && !m_ClipsLoading) ||
            !PropertyGrid::BeginGroup(
                ("Clips (" + std::to_string(m_Clips.size()) + ")").c_str(), nullptr, nullptr, false))
            return;
        const auto* modes = m_Draft.Schema().Find("animation.compression.clip:1:x");
        if (m_ClipsLoading)
            PropertyGrid::Text("Clips", "Reading...", true);
        const auto megabytes = [](uint64_t bytes) { return double(bytes) / (1024.0 * 1024.0); };
        for (const auto& clip : m_Clips)
        {
            ImGui::PushID(clip.Name.c_str());
            if (!clip.Error.empty())
                PropertyGrid::Text(clip.Name.c_str(), "Cannot read: " + clip.Error, true);
            else if (modes)
            {
                // The override, with the last import's result under it.
                const auto key      = ClipKey(clip.Name);
                const auto value    = m_Draft.Get(key);
                const bool modified = !m_Draft.IsDefault(key);
                if (PropertyGrid::Row(clip.Name.c_str(), modes->Description.c_str(), modified))
                {
                    std::string preview = value;
                    for (const auto& option : modes->Options)
                        if (option.Value == value)
                            preview = option.Label;
                    if (ImGui::BeginCombo("##mode", preview.c_str()))
                    {
                        for (const auto& option : modes->Options)
                        {
                            if (ImGui::Selectable(option.Label.c_str(), option.Value == value))
                                m_Draft.Set(key, option.Value);
                            if (!option.Description.empty() && ImGui::IsItemHovered())
                                ImGui::SetTooltip("%s", option.Description.c_str());
                        }
                        ImGui::EndCombo();
                    }
                    if (PropertyGrid::RowReset(modified))
                        m_Draft.Set(key, modes->Default);
                }
                const auto& s = clip.Statistics;
                char        summary[160];
                if (s.Mode == AnimationCompressionMode::Off)
                    std::snprintf(summary, sizeof(summary), "Uncompressed, %.2f MB", megabytes(s.CompressedBytes));
                else
                    std::snprintf(summary,
                                  sizeof(summary),
                                  "%.2f MB -> %.2f MB (%.1f%%), max error %.3g",
                                  megabytes(s.UncompressedBytes),
                                  megabytes(s.CompressedBytes),
                                  s.UncompressedBytes ? 100.0 * s.CompressedBytes / s.UncompressedBytes : 0.0,
                                  s.MaxPositionError);
                PropertyGrid::Text("", summary, true);
            }
            ImGui::PopID();
        }
        (void)busy;
        PropertyGrid::EndGroup();
    }

    void ProductImportPanel::DrawDeveloper(bool busy)
    {
        if (!PropertyGrid::BeginGroup("Developer", nullptr, nullptr, false))
            return;
        PropertyGrid::BeginFree();
        ImGui::BeginDisabled(busy);
        Hint("Raw import settings as saved in the .kmeta file, and the file's product IDs.");
        ImGui::SetNextItemWidth(-FLT_MIN);
        ImGui::InputTextWithHint("##Importer", "Importer (empty: the default for this file type)", &m_Importer);
        if (ImGui::BeginTable("##Raw", 3, ImGuiTableFlags_RowBg | ImGuiTableFlags_BordersInnerV))
        {
            ImGui::TableSetupColumn("Key", ImGuiTableColumnFlags_WidthStretch, 1.0f);
            ImGui::TableSetupColumn("Value", ImGuiTableColumnFlags_WidthStretch, 1.0f);
            ImGui::TableSetupColumn("##Remove", ImGuiTableColumnFlags_WidthFixed, ImGui::GetFrameHeight());
            std::optional<std::string> erase;
            for (const auto& [key, value] : m_Draft.Settings())
            {
                ImGui::PushID(key.c_str());
                ImGui::TableNextRow();
                ImGui::TableNextColumn();
                ImGui::AlignTextToFramePadding();
                ImGui::TextUnformatted(key.c_str());
                if (!m_Draft.Schema().Find(key) && ImGui::IsItemHovered())
                    ImGui::SetTooltip("Not a setting this importer describes");
                ImGui::TableNextColumn();
                // Preserve persisted binary/escaped values; the single-line UI
                // cannot round-trip embedded NUL. Do not silently truncate them.
                if (value.find('\0') != std::string::npos)
                    ImGui::TextDisabled("(embedded NUL; edit the sidecar)");
                else
                {
                    std::string text = value;
                    ImGui::SetNextItemWidth(-FLT_MIN);
                    if (ImGui::InputText("##value", &text))
                        m_Draft.SetRaw(key, text);
                }
                ImGui::TableNextColumn();
                if (ImGui::Button(ICON_FA_XMARK))
                    erase = key;
                ImGui::PopID();
            }
            ImGui::EndTable();
            if (erase)
                m_Draft.EraseRaw(*erase);
        }
        const float add = ButtonWidth("Add");
        ImGui::SetNextItemWidth((ImGui::GetContentRegionAvail().x - add) * 0.5f - ImGui::GetStyle().ItemSpacing.x);
        ImGui::InputTextWithHint("##NewKey", "key", &m_NewKey);
        ImGui::SameLine();
        ImGui::SetNextItemWidth(ImGui::GetContentRegionAvail().x - add - ImGui::GetStyle().ItemSpacing.x);
        ImGui::InputTextWithHint("##NewValue", "value", &m_NewValue);
        ImGui::SameLine();
        if (ImGui::Button("Add"))
        {
            if (m_NewKey.empty() || m_Draft.Settings().count(m_NewKey))
                m_Error = "Enter a new key; existing values were preserved.";
            else
            {
                m_Draft.SetRaw(m_NewKey, m_NewValue);
                m_NewKey.clear();
                m_NewValue.clear();
                m_Error.clear();
            }
        }
        ImGui::Checkbox("Force a full rebuild on the next import", &m_Force);

        if (const auto registry = m_Service->GetRuntimeAssetRegistry())
        {
            ImGui::SeparatorText("Products");
            for (const auto& product : registry->GetAllProducts())
                if (product.Metadata.assetID.GetSourceID() == m_Selected && product.Target == EditorAssetTarget)
                {
                    const auto id = product.Metadata.assetID.ToString();
                    ImGui::PushID(id.c_str());
                    if (ImGui::SmallButton(ICON_FA_COPY))
                        ImGui::SetClipboardText(id.c_str());
                    if (ImGui::IsItemHovered())
                        ImGui::SetTooltip("Copy the ID");
                    ImGui::SameLine();
                    ImGui::Text("%s  %s", TypeName(product.Metadata.Type, 1), id.c_str());
                    ImGui::PopID();
                }
        }

        ImGui::SeparatorText("Move / rename");
        ImGui::SetNextItemWidth(ImGui::GetContentRegionAvail().x - ButtonWidth("Move") -
                                ImGui::GetStyle().ItemSpacing.x);
        ImGui::InputText("##MoveDestination", &m_MoveDestination);
        if (ImGui::IsItemHovered())
            ImGui::SetTooltip("Asset-relative destination. The folder must exist. Moves keep the asset's identity, "
                              "settings and products;\npaths inside other source files are not rewritten.");
        ImGui::SameLine();
        if (ImGui::Button("Move"))
        {
            try
            {
                if (m_MoveDestination.size() > 4096)
                    m_Error = "Destination path exceeds 4096 bytes.";
                else
                {
                    Record(
                        m_Service->MoveSourceAsset(m_Selected, AssetPath(std::filesystem::u8path(m_MoveDestination))));
                    if (m_Error.empty())
                        ReloadSources();
                }
            }
            catch (const std::exception& error)
            {
                m_Error = error.what();
            }
        }
        ImGui::EndDisabled();
        DrawDeletion();
        PropertyGrid::EndFree();
        PropertyGrid::EndGroup();
    }

    void ProductImportPanel::DrawDeletion()
    {
        bool review = false;
        ImGui::SeparatorText("Delete");
        if (ImGui::Button("Review deletion..."))
        {
            auto planned = m_Service->PlanSourceAssetDeletion(m_Selected);
            if (const auto* error = std::get_if<AssetError>(&planned))
                m_Error = error->Message;
            else
            {
                m_DeletionPlan = std::get<SourceAssetDeletionPlan>(std::move(planned));
                m_Error.clear();
                review = true;
            }
        }
        if (ImGui::IsItemHovered())
            ImGui::SetTooltip("Deletes the source file, its sidecar and all of its published products.");
        if (review)
            ImGui::OpenPopup("Confirm Asset Deletion");
        if (!ImGui::BeginPopupModal("Confirm Asset Deletion", nullptr, ImGuiWindowFlags_AlwaysAutoResize))
            return;
        if (m_DeletionPlan)
        {
            const auto& plan = *m_DeletionPlan;
            ImGui::TextWrapped("Source: %s", plan.Source.Path.ToString().c_str());
            if (!plan.Dependents.empty())
            {
                ImGui::TextWrapped("These assets use it and will show it as missing until reimported:");
                for (const auto& dependent : plan.Dependents)
                    ImGui::BulletText("%s", dependent.Path.ToString().c_str());
            }
            ImGui::TextWrapped("Scene references to its Products become unresolved.");
            if (ImGui::Button("Delete asset and files"))
            {
                Record(m_Service->DeleteSourceAsset(plan.Source.sourceAssetID));
                if (m_Error.empty())
                {
                    m_DeletionPlan.reset();
                    ReloadSources();
                    ImGui::CloseCurrentPopup();
                }
            }
        }
        else
            ImGui::TextUnformatted("The source selection changed. Close and review deletion again.");
        if (!m_Error.empty())
            ImGui::TextWrapped("%s", m_Error.c_str());
        ImGui::SameLine();
        if (ImGui::Button("Close"))
        {
            m_DeletionPlan.reset();
            ImGui::CloseCurrentPopup();
        }
        ImGui::EndPopup();
    }

    // ---------------------------------------------------------------------- import and result

    void ProductImportPanel::DrawImport(const EditorAssetImportSnapshot& status)
    {
        const auto* source = Selected();
        ImGui::Dummy({0, EditorTheme::Px(12.0f)});
        ImGui::TextUnformatted(("Importing " + (source ? source->Path.ToString() : std::string())).c_str());
        // An indeterminate bar: the build reports checkpoints, not a fraction.
        const float t = float(ImGui::GetTime());
        ImGui::ProgressBar(-1.0f * t, {-FLT_MIN, 0}, status.Phase ? PhaseName(*status.Phase) : "Reading the source");
        if (status.CancelRequested)
            Hint("Cancelling at the next safe point...");
        else
            Hint("Dependencies (textures, materials) are imported first. You can keep working meanwhile.");
    }

    void ProductImportPanel::DrawResult(const EditorAssetImportSnapshot& status)
    {
        const auto& p = EditorTheme::Colors();
        ImGui::Dummy({0, EditorTheme::Px(8.0f)});
        const bool ok = status.State == EditorAssetImportState::Succeeded;
        if (ok)
            ImGui::TextColored(p.Success,
                               ICON_FA_CIRCLE_CHECK "  %s",
                               status.Result && status.Result->Root.Disposition == AssetBuildDisposition::UpToDate ?
                                   "Already up to date" :
                                   "Imported");
        else
            ImGui::TextColored(status.State == EditorAssetImportState::Cancelled ? p.Warning : p.Danger,
                               ICON_FA_CIRCLE_XMARK "  %s",
                               StateName(status.State));
        if (status.Error)
            ImGui::TextWrapped("%s", status.Error->Message.c_str());

        // What the file became, by type.
        if (const auto registry = m_Service->GetRuntimeAssetRegistry(); ok && registry)
        {
            std::map<AssetType, size_t> counts;
            for (const auto& product : registry->GetAllProducts())
                if (product.Metadata.assetID.GetSourceID() == m_Selected && product.Target == EditorAssetTarget)
                    ++counts[product.Metadata.Type];
            std::string summary;
            for (const auto& [type, count] : counts)
                summary += (summary.empty() ? "" : ", ") + std::to_string(count) + " " + TypeName(type, count);
            PropertyGrid::Begin("##Created");
            if (!summary.empty())
                PropertyGrid::Text("Created", summary);
            if (status.Result && !status.Result->Dependencies.empty())
                PropertyGrid::Text("Also imported", std::to_string(status.Result->Dependencies.size()) + " used files");
            PropertyGrid::End();
        }
        std::vector<std::string> warnings;
        if (status.Result)
        {
            warnings = status.Result->Root.Warnings;
            for (const auto& dependency : status.Result->Dependencies)
                warnings.insert(warnings.end(), dependency.Warnings.begin(), dependency.Warnings.end());
        }
        if (!warnings.empty() &&
            PropertyGrid::BeginGroup(("Warnings (" + std::to_string(warnings.size()) + ")").c_str()))
        {
            PropertyGrid::BeginFree();
            for (const auto& warning : warnings)
            {
                ImGui::TextColored(p.Warning, ICON_FA_TRIANGLE_EXCLAMATION);
                ImGui::SameLine();
                ImGui::TextWrapped("%s", warning.c_str());
            }
            PropertyGrid::EndFree();
            PropertyGrid::EndGroup();
        }
        if (const auto project = m_Service->GetProjectWarnings();
            std::holds_alternative<std::vector<AssetError>>(project))
            for (const auto& warning : std::get<std::vector<AssetError>>(project))
                if (std::none_of(warnings.begin(), warnings.end(), [&](const auto& listed) {
                        return warning.Message.find(listed) != std::string::npos;
                    }))
                    Hint(("Project: " + warning.Message).c_str());
    }
} // namespace Kans
