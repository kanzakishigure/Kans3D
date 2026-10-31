#include "ContentBrowserPanel.h"
#include "Kans3D/ImGui/EditorTheme.h"
#include <algorithm>
#include <cstdlib>
#include <cstring>
#include <ctime>
#include <fstream>
#include <imgui_internal.h>
#include "Kans3D/Asset/Importer/ImporterRegistry.h"
#include "Kans3D/Editor/EditorResources.h"
#include "Kans3D/FileSystem/FileSystem.h"
#include "UI/EditorIcons.h"
#include "UI/Widgets.h"
#include "UI/PropertyGrid.h"

namespace Kans
{
    namespace fs = std::filesystem;
    using Browser::Entry;

    namespace
    {
        constexpr ImU32 ImportedColor     = IM_COL32(90, 200, 120, 255);
        constexpr ImU32 NotImportedColor  = IM_COL32(150, 150, 150, 255);
        constexpr ImVec4 WarningColor {0.95f, 0.75f, 0.30f, 1.0f};
        constexpr ImVec4 DangerColor {0.80f, 0.25f, 0.22f, 1.0f};

        fs::path Normal(const fs::path& path)
        {
            std::error_code error;
            auto            result = fs::weakly_canonical(path, error);
            return error ? path.lexically_normal() : result;
        }

        std::string Key(const fs::path& path) { return Normal(path).generic_u8string(); }

        std::string FormatSize(uint64_t bytes)
        {
            const char* units[] = {"B", "KB", "MB", "GB", "TB"};
            double      size    = double(bytes);
            int         unit    = 0;
            while (size >= 1024.0 && unit < 4)
                size /= 1024.0, ++unit;
            char text[32];
            std::snprintf(text, sizeof(text), unit ? "%.1f %s" : "%.0f %s", size, units[unit]);
            return text;
        }

        std::string FormatTime(fs::file_time_type time)
        {
            if (time == fs::file_time_type {})
                return "";
            const auto system = std::chrono::time_point_cast<std::chrono::system_clock::duration>(
                time - fs::file_time_type::clock::now() + std::chrono::system_clock::now());
            const std::time_t value = std::chrono::system_clock::to_time_t(system);
            std::tm           local {};
#ifdef _WIN32
            localtime_s(&local, &value);
#else
            localtime_r(&value, &local);
#endif
            char text[32];
            std::strftime(text, sizeof(text), "%Y-%m-%d %H:%M", &local);
            return text;
        }

        const char* ProductTypeName(AssetType type)
        {
            switch (type)
            {
                case AssetType::Texture:
                    return "Texture";
                case AssetType::Material:
                    return "Material";
                case AssetType::StaticMesh:
                case AssetType::Mesh:
                    return "Mesh";
                case AssetType::Model:
                    return "Model";
                case AssetType::Shader:
                    return "Shader";
                case AssetType::Scene:
                    return "Scene";
                default:
                    return "Asset";
            }
        }

        // `text` shortened with "..." to fit `width` pixels.
        std::string Ellipsis(const std::string& text, float width)
        {
            if (ImGui::CalcTextSize(text.c_str()).x <= width)
                return text;
            const float dots = ImGui::CalcTextSize("...").x;
            size_t      lo = 0, hi = text.size();
            while (lo < hi)
            {
                const size_t mid = (lo + hi + 1) / 2;
                if (ImGui::CalcTextSize(text.substr(0, mid).c_str()).x + dots <= width)
                    lo = mid;
                else
                    hi = mid - 1;
            }
            // Do not cut inside a UTF-8 sequence.
            while (lo > 0 && (static_cast<unsigned char>(text[lo]) & 0xC0) == 0x80)
                --lo;
            return text.substr(0, lo) + "...";
        }

        bool IsTextFile(const std::string& extension)
        {
            static const char* const texts[] = {".txt",  ".md",   ".json", ".yaml", ".yml", ".xml", ".ini",
                                                ".csv",  ".glsl", ".vert", ".frag", ".comp", ".shaderprog",
                                                ".material", ".mtl", ".kans", ".lua", ".cs", ".h", ".cpp"};
            return std::find(std::begin(texts), std::end(texts), extension) != std::end(texts);
        }

        // Opens the system file manager at `path` (its folder for files).
        void Reveal(const fs::path& path)
        {
            std::error_code error;
            const auto      folder = fs::is_directory(path, error) ? path : path.parent_path();
#ifdef _WIN32
            const std::string command = "explorer \"" + folder.string() + "\"";
#else
            std::string quoted = "'";
            for (char c : folder.u8string())
                quoted += c == '\'' ? std::string("'\\''") : std::string(1, c);
            quoted += "'";
#ifdef __APPLE__
            const std::string command = "open " + quoted + " &";
#else
            const std::string command = "xdg-open " + quoted + " >/dev/null 2>&1 &";
#endif
#endif
            std::system(command.c_str());
        }

        const ImporterRegistry& Importers()
        {
            static const ImporterRegistry registry = [] {
                auto created = CreateDefaultImporterRegistry();
                return std::holds_alternative<ImporterRegistry>(created) ? std::get<ImporterRegistry>(created) :
                                                                           ImporterRegistry {};
            }();
            return registry;
        }

        // Transparent icon button.
        bool IconButton(const char* icon, const char* tooltip, bool active = false, bool enabled = true)
        {
            ImGui::PushStyleColor(ImGuiCol_Button, active ? EditorTheme::Colors().Selection : ImVec4(0, 0, 0, 0));
            ImGui::PushStyleColor(ImGuiCol_ButtonHovered, EditorTheme::Colors().Hover);
            ImGui::BeginDisabled(!enabled);
            const bool clicked = ImGui::Button(icon, {ImGui::GetFrameHeight(), 0.0f});
            ImGui::EndDisabled();
            ImGui::PopStyleColor(2);
            if (ImGui::IsItemHovered(ImGuiHoveredFlags_AllowWhenDisabled))
                ImGui::SetTooltip("%s", tooltip);
            return clicked;
        }

        std::string Label(const char* icon, const char* text) { return std::string(icon) + "  " + text; }

        // Spaces at least `width` wide, leaving room in a label for an image.
        std::string IconSpace(float width)
        {
            std::string space = " ";
            while (ImGui::CalcTextSize(space.c_str()).x < width && space.size() < 64)
                space += ' ';
            return space;
        }

        ImVec4 KindColor(Browser::AssetKind kind)
        {
            const auto& p = EditorTheme::Colors();
            switch (kind)
            {
                case Browser::AssetKind::Model:
                    return p.TypeModel;
                case Browser::AssetKind::Texture:
                    return p.TypeImage;
                case Browser::AssetKind::Material:
                    return p.TypeMaterial;
                case Browser::AssetKind::Shader:
                    return p.TypeShader;
                case Browser::AssetKind::Scene:
                    return p.TypeScene;
                case Browser::AssetKind::Audio:
                    return p.TypeAudio;
                case Browser::AssetKind::Font:
                    return p.TypeFont;
                case Browser::AssetKind::UI:
                    return p.TypeUI;
                case Browser::AssetKind::Timeline:
                    return p.TypeTimeline;
                case Browser::AssetKind::Action:
                    return p.TypeAction;
                case Browser::AssetKind::Expression:
                    return p.TypeExpression;
                case Browser::AssetKind::PhysicsMaterial:
                case Browser::AssetKind::Cloth:
                case Browser::AssetKind::Ragdoll:
                    return p.TypePhysics;
                default:
                    return p.TextDim;
            }
        }

        // A rounded toggle with the type's colour dot; tinted while on.
        bool KindPill(const char* name, ImVec4 color, bool on)
        {
            using EditorTheme::Px;
            const float  dot  = Px(8.0f);
            const ImVec2 text = ImGui::CalcTextSize(name);
            const ImVec2 size {Px(10.0f) + dot + Px(6.0f) + text.x + Px(10.0f), ImGui::GetFrameHeight()};
            const ImVec2 at   = ImGui::GetCursorScreenPos();
            const bool   clicked = ImGui::InvisibleButton(name, size);
            const bool   hovered = ImGui::IsItemHovered();
            auto*        draw    = ImGui::GetWindowDrawList();
            ImVec4       fill    = on ? ImVec4 {color.x, color.y, color.z, 0.22f} : EditorTheme::Colors().Raised;
            if (hovered)
                fill = on ? ImVec4 {color.x, color.y, color.z, 0.32f} : EditorTheme::Colors().Hover;
            draw->AddRectFilled(at, {at.x + size.x, at.y + size.y}, ImGui::GetColorU32(fill), size.y * 0.5f);
            draw->AddCircleFilled({at.x + Px(10.0f) + dot * 0.5f, at.y + size.y * 0.5f}, dot * 0.5f, ImGui::GetColorU32(color));
            draw->AddText({at.x + Px(10.0f) + dot + Px(6.0f), at.y + (size.y - text.y) * 0.5f},
                          ImGui::GetColorU32(on ? EditorTheme::Colors().Text : EditorTheme::Colors().TextDim), name);
            return clicked;
        }
    } // namespace

    // ====================================================================== setup

    ContentBrowserPanel::ContentBrowserPanel()
    {
        m_Root    = KansFileSystem::GetAssetFolder();
        m_Current = m_Root;
    }

    void ContentBrowserPanel::Bind(EditorAssetImportService* service)
    {
        m_Service = service;
        m_Operations.SetService(service);
        if (service)
        {
            m_Root = m_Operations.AssetRoot();
            const auto saved = m_SavedDirectory.empty() ? m_Root : Normal(m_Root / fs::u8path(m_SavedDirectory));
            std::error_code error;
            m_Current = Browser::IsWithin(saved, m_Root) && fs::is_directory(saved, error) ? saved : m_Root;
        }
        else
        {
            m_Root.clear();
            m_Current.clear();
        }
        m_Back.clear();
        m_Forward.clear();
        m_Selection.Clear();
        m_RevealCurrent = true;
        m_Registry.reset();
        Refresh();
    }

    void ContentBrowserPanel::Refresh()
    {
        m_Cache.Invalidate();
        m_SourcesDirty = true;
    }

    void ContentBrowserPanel::InstallSettingsHandler()
    {
        m_Settings.Install(
            "KansContentBrowser",
            [this](const char* key, const char* value) {
                const std::string name(key);
                if (name == "View")
                    m_View = std::atoi(value) == 1 ? View::List : View::Grid;
                else if (name == "IconSize")
                    m_IconSize = std::clamp(float(std::atof(value)), 48.0f, 192.0f);
                else if (name == "Sort")
                    m_SortKey = Browser::SortKey(std::clamp(std::atoi(value), 0, 3));
                else if (name == "Ascending")
                    m_Ascending = std::atoi(value) != 0;
                else if (name == "Preview")
                    m_ShowPreview = std::atoi(value) != 0;
                else if (name == "Directory")
                {
                    // imgui.ini is read on the first frame, after Bind() chose the root.
                    m_SavedDirectory = value;
                    std::error_code error;
                    if (m_Root.empty())
                        return;
                    const auto      saved = Normal(m_Root / fs::u8path(m_SavedDirectory));
                    if (*value && Browser::IsWithin(saved, m_Root) && fs::is_directory(saved, error))
                    {
                        m_Current       = saved;
                        m_RevealCurrent = true;
                    }
                }
                else if (name == "Favorite" && *value)
                    m_Favorites.push_back(value);
            },
            [this](ImGuiTextBuffer& buffer) {
                buffer.appendf("View=%d\nIconSize=%.0f\nSort=%d\nAscending=%d\nPreview=%d\n",
                               m_View == View::List ? 1 : 0,
                               m_IconSize,
                               int(m_SortKey),
                               m_Ascending ? 1 : 0,
                               m_ShowPreview ? 1 : 0);
                const auto relative = m_Current.lexically_relative(m_Root).generic_u8string();
                buffer.appendf("Directory=%s\n", relative == "." ? "" : relative.c_str());
                for (const auto& favorite : m_Favorites)
                    buffer.appendf("Favorite=%s\n", favorite.c_str());
            });
    }

    void ContentBrowserPanel::UninstallSettingsHandler() { m_Settings.Detach(); }

    // ====================================================================== data

    void ContentBrowserPanel::Navigate(const fs::path& directory, bool record)
    {
        const auto target = Normal(directory);
        if (target == m_Current || !Browser::IsWithin(target, m_Root))
            return;
        CommitRename();
        if (record)
        {
            m_Back.push_back(m_Current);
            m_Forward.clear();
        }
        m_Current = target;
        m_Selection.Clear();
        m_RevealCurrent = true;
    }

    void ContentBrowserPanel::UpdateSources(bool force)
    {
        if (!m_Service)
            return;
        auto       registry        = m_Service->GetRuntimeAssetRegistry();
        const bool registryChanged = registry != m_Registry;
        const bool listingChanged  = m_Cache.Generation() != m_SourcesGeneration;
        const double now           = ImGui::GetTime();
        if (!force && !m_SourcesDirty && !registryChanged && !listingChanged && now - m_SourcesTime < 2.0)
            return;
        m_SourcesDirty      = false;
        m_SourcesGeneration = m_Cache.Generation();
        m_SourcesTime       = now;
        m_Registry          = registry;

        const auto read = [&] {
            m_Sources.clear();
            auto sources = m_Service->GetSources();
            if (const auto* list = std::get_if<std::vector<SourceAssetMetadata>>(&sources))
                for (const auto& source : *list)
                    if (source.Exists)
                        m_Sources[Key(m_Root / source.Path.GetPath())] = {source, 0};
        };
        read();
        // New files in the shown folder are registered as sources (when no import runs).
        if (listingChanged && std::any_of(m_Visible.begin(), m_Visible.end(), [&](const Entry& entry) {
                return !entry.Directory && !m_Sources.count(Key(entry.Path));
            }))
            if (std::holds_alternative<std::monostate>(m_Service->RefreshSources()))
                read();

        if (m_Registry)
        {
            std::unordered_map<SourceAssetID, size_t> products;
            for (const auto& record : m_Registry->GetAllProducts())
                if (record.Target == EditorAssetTarget)
                    ++products[record.Metadata.assetID.GetSourceID()];
            for (auto& [path, status] : m_Sources)
                if (const auto found = products.find(status.Source.sourceAssetID); found != products.end())
                    status.Products = found->second;
        }
    }

    const ContentBrowserPanel::SourceStatus* ContentBrowserPanel::StatusOf(const fs::path& file) const
    {
        const auto found = m_Sources.find(Key(file));
        return found == m_Sources.end() ? nullptr : &found->second;
    }

    bool ContentBrowserPanel::CanImport(const fs::path& file) const { return Importers().Supports(file); }

    void ContentBrowserPanel::RebuildVisible()
    {
        m_Filter.Text = m_Search.data();
        m_Visible.clear();
        for (const auto& entry : m_Cache.List(m_Current))
            if (m_Filter.Matches(entry))
                m_Visible.push_back(entry);
        Browser::SortEntries(m_Visible, m_SortKey, m_Ascending);
        m_Selection.Retain(VisiblePaths());
    }

    std::vector<fs::path> ContentBrowserPanel::VisiblePaths() const
    {
        std::vector<fs::path> paths;
        paths.reserve(m_Visible.size());
        for (const auto& entry : m_Visible)
            paths.push_back(entry.Path);
        return paths;
    }

    std::vector<fs::path> ContentBrowserPanel::Targets(const fs::path& clicked) const
    {
        if (m_Selection.Contains(clicked))
            return m_Selection.Ordered(VisiblePaths());
        return {clicked};
    }

    // ====================================================================== frame

    void ContentBrowserPanel::onImGuiRender()
    {
        if (m_Root.empty())
        {
            ImGui::TextDisabled("No project is open.");
            return;
        }
        if (m_FocusRequested)
        {
            ImGui::SetWindowFocus(); // Brings a docked tab to the front.
            m_FocusRequested = false;
        }
        m_Thumbnails.Update();
        std::error_code error;
        if (!fs::is_directory(m_Current, error))
            m_Current = m_Root;
        RebuildVisible();
        UpdateSources(false);

        DrawToolbar();
        DrawFilterRow();
        const ImGuiTableFlags flags   = ImGuiTableFlags_Resizable | ImGuiTableFlags_BordersInnerV;
        // The preview steps aside when the panel is too narrow for three columns.
        const bool            preview = m_ShowPreview && ImGui::GetContentRegionAvail().x >= EditorTheme::Px(720.0f);
        const int             columns = preview ? 3 : 2;
        const float           status  = ImGui::GetFrameHeight();
        // Children in table cells size against the window, not the cell: give them the height.
        m_ColumnHeight = std::max(1.0f, ImGui::GetContentRegionAvail().y - status - ImGui::GetStyle().ItemSpacing.y);
        if (ImGui::BeginTable("##BrowserLayout", columns, flags, {0.0f, m_ColumnHeight}))
        {
            m_ColumnHeight -= 2.0f * ImGui::GetStyle().CellPadding.y; // Cells pad their content.
            ImGui::TableSetupColumn("Folders", ImGuiTableColumnFlags_WidthFixed, 210.0f);
            ImGui::TableSetupColumn("Items", ImGuiTableColumnFlags_WidthStretch);
            if (preview)
                ImGui::TableSetupColumn("Preview", ImGuiTableColumnFlags_WidthFixed, 280.0f);
            ImGui::TableNextRow();
            ImGui::TableSetColumnIndex(0);
            ImGui::BeginChild("##Tree", {0, m_ColumnHeight});
            DrawTree();
            ImGui::EndChild();
            ImGui::TableSetColumnIndex(1);
            DrawItems();
            if (preview)
            {
                ImGui::TableSetColumnIndex(2);
                ImGui::BeginChild("##Preview", {0, m_ColumnHeight}, ImGuiChildFlags_AlwaysUseWindowPadding);
                DrawPreview();
                ImGui::EndChild();
            }
            ImGui::EndTable();
        }
        DrawStatusBar();
        DrawContextMenus();
        DrawDeleteDialog();
    }

    // ---------------------------------------------------------------------- toolbar

    void ContentBrowserPanel::DrawToolbar()
    {
        using EditorTheme::Px;
        ImGui::PushStyleVar(ImGuiStyleVar_ItemSpacing, {Px(3.0f), Px(4.0f)});
        // Create and import: the main actions, as text buttons.
        if (Widgets::TextButton(ICON_FA_PLUS, "Add", "Create in this folder", false, true))
            ImGui::OpenPopup("##Add");
        if (ImGui::BeginPopup("##Add"))
        {
            if (ImGui::MenuItem(Label(ICON_FA_FOLDER_PLUS, "Folder").c_str()))
                NewFolder(m_Current);
            if (ImGui::MenuItem(Label(ICON_FA_PALETTE, "Material").c_str(), nullptr, false, bool(m_CreateMaterial)))
                m_CreateMaterial(m_Current);
            if (ImGui::MenuItem(Label(ICON_FA_DESKTOP, "UI").c_str(), nullptr, false, bool(m_CreateUI)))
                m_CreateUI(m_Current), Refresh();
            if (ImGui::MenuItem(Label(ICON_FA_FILM, "Timeline").c_str(), nullptr, false, bool(m_CreateTimeline)))
                m_CreateTimeline(m_Current), Refresh();
            if (ImGui::MenuItem(
                    Label(ICON_FA_ATOM, "Physics Material").c_str(), nullptr, false, bool(m_CreatePhysicsMaterial)))
                m_CreatePhysicsMaterial(m_Current), Refresh();
            ImGui::EndPopup();
        }
        ImGui::SameLine();
        // Imports the selected source files (or reimports them).
        std::vector<fs::path> importable;
        for (const auto& item : m_Selection.Items())
            if (CanImport(item))
                importable.push_back(item);
        ImGui::BeginDisabled(importable.empty() || !m_Import);
        if (Widgets::TextButton(ICON_FA_DOWNLOAD, "Import",
                                importable.empty() ? "Select source files to import" : "Import the selected source files"))
            for (const auto& item : importable)
                m_Import(item);
        ImGui::EndDisabled();
        ImGui::SameLine(0, Px(12.0f));

        if (IconButton(ICON_FA_ARROW_LEFT, "Back (Alt+Left)", false, !m_Back.empty()))
        {
            m_Forward.push_back(m_Current);
            const auto target = m_Back.back();
            m_Back.pop_back();
            Navigate(target, false);
        }
        ImGui::SameLine();
        if (IconButton(ICON_FA_ARROW_RIGHT, "Forward (Alt+Right)", false, !m_Forward.empty()))
        {
            m_Back.push_back(m_Current);
            const auto target = m_Forward.back();
            m_Forward.pop_back();
            Navigate(target, false);
        }
        ImGui::SameLine();
        if (IconButton(ICON_FA_ARROW_UP, "Parent folder (Backspace)", false, m_Current != m_Root))
            Navigate(m_Current.parent_path());
        ImGui::SameLine();
        if (IconButton(ICON_FA_ROTATE_RIGHT, "Refresh"))
        {
            Refresh();
            if (m_Service)
                (void)m_Service->RefreshSources();
        }
        ImGui::SameLine(0, Px(8.0f));

        // Breadcrumbs take the rest, up to the view menu.
        const float menu = ImGui::GetFrameHeight();
        DrawBreadcrumbs(std::max(Px(80.0f), ImGui::GetContentRegionAvail().x - menu - Px(8.0f)));
        ImGui::SameLine(std::max(ImGui::GetCursorPosX(), ImGui::GetCursorPosX() + ImGui::GetContentRegionAvail().x - menu));
        if (IconButton(ICON_FA_GEAR, "View options"))
            ImGui::OpenPopup("##ViewMenu");
        if (ImGui::BeginPopup("##ViewMenu"))
        {
            if (ImGui::MenuItem(Label(ICON_FA_TABLE_CELLS_LARGE, "Grid").c_str(), nullptr, m_View == View::Grid))
                m_View = View::Grid;
            if (ImGui::MenuItem(Label(ICON_FA_LIST, "List").c_str(), nullptr, m_View == View::List))
                m_View = View::List;
            if (m_View == View::Grid)
            {
                ImGui::SetNextItemWidth(Px(160.0f));
                ImGui::SliderFloat("Icon size", &m_IconSize, 48.0f, 192.0f, "%.0f px");
            }
            ImGui::Separator();
            if (ImGui::BeginMenu(Label(ICON_FA_ARROW_DOWN_A_Z, "Sort by").c_str()))
            {
                const char* keys[] = {"Name", "Type", "Size", "Date modified"};
                for (int i = 0; i < 4; ++i)
                    if (ImGui::MenuItem(keys[i], nullptr, int(m_SortKey) == i))
                        m_SortKey = Browser::SortKey(i);
                ImGui::Separator();
                if (ImGui::MenuItem("Ascending", nullptr, m_Ascending))
                    m_Ascending = true;
                if (ImGui::MenuItem("Descending", nullptr, !m_Ascending))
                    m_Ascending = false;
                ImGui::EndMenu();
            }
            ImGui::Separator();
            ImGui::MenuItem(Label(ICON_FA_TABLE_COLUMNS, "Preview pane").c_str(), nullptr, &m_ShowPreview);
            ImGui::EndPopup();
        }
        ImGui::PopStyleVar();
    }

    void ContentBrowserPanel::DrawFilterRow()
    {
        using EditorTheme::Px;
        ImGui::SetNextItemWidth(std::min(Px(240.0f), ImGui::GetContentRegionAvail().x * 0.4f));
        ImGui::InputTextWithHint("##search", ICON_FA_MAGNIFYING_GLASS "  Search", m_Search.data(), m_Search.size());
        const bool all = m_Filter.ShowsAllTypes();
        for (int i = 1; i < Browser::AssetKindCount; ++i)
        {
            const auto kind = Browser::AssetKind(i);
            ImGui::SameLine(0, i == 1 ? Px(10.0f) : Px(4.0f));
            ImGui::PushID(i);
            // With every type shown no pill is "on": the first click narrows to one type.
            if (KindPill(Browser::KindName(kind), KindColor(kind), !all && m_Filter.Kinds[i]))
                m_Filter.TogglePill(kind);
            ImGui::PopID();
        }
        if (!all)
        {
            ImGui::SameLine(0, Px(6.0f));
            if (IconButton(ICON_FA_XMARK, "Show every type"))
                std::fill(std::begin(m_Filter.Kinds), std::end(m_Filter.Kinds), true);
        }
    }

    void ContentBrowserPanel::DrawStatusBar()
    {
        size_t folders = 0;
        for (const auto& entry : m_Visible)
            folders += entry.Directory;
        std::string text = std::to_string(m_Visible.size() - folders) + " items";
        if (folders)
            text += ", " + std::to_string(folders) + (folders == 1 ? " folder" : " folders");
        if (!m_Selection.Empty())
            text += "  |  " + std::to_string(m_Selection.Size()) + " selected";
        ImGui::AlignTextToFramePadding();
        ImGui::TextDisabled("%s", text.c_str());
        // Import progress, on the right.
        if (m_Service)
            if (auto snapshot = m_Service->GetSnapshot(); std::holds_alternative<EditorAssetImportSnapshot>(snapshot))
            {
                const auto& state = std::get<EditorAssetImportSnapshot>(snapshot);
                if (state.State == EditorAssetImportState::Building)
                {
                    std::string importing = ICON_FA_SPINNER "  Importing";
                    for (const auto& [path, status] : m_Sources)
                        if (status.Source.sourceAssetID == state.Source)
                            importing += " " + fs::u8path(path).filename().u8string();
                    const float width = ImGui::CalcTextSize(importing.c_str()).x;
                    ImGui::SameLine(std::max(ImGui::GetCursorPosX(),
                                             ImGui::GetCursorPosX() + ImGui::GetContentRegionAvail().x - width));
                    ImGui::TextColored(EditorTheme::Colors().Accent, "%s", importing.c_str());
                }
            }
    }

    void ContentBrowserPanel::DrawBreadcrumbs(float width)
    {
        std::vector<fs::path> segments = {m_Root};
        for (const auto& part : m_Current.lexically_relative(m_Root))
            if (part != ".")
                segments.push_back(segments.back() / part);
        const auto name = [&](size_t i) {
            return i == 0 ? std::string(ICON_FA_HOUSE " ") + m_Root.filename().u8string() :
                            segments[i].filename().u8string();
        };
        // Hide leading folders (after the root) until the rest fits.
        const float separator = ImGui::CalcTextSize(">").x + 10.0f;
        float       total     = 0.0f;
        std::vector<float> widths;
        for (size_t i = 0; i < segments.size(); ++i)
        {
            widths.push_back(ImGui::CalcTextSize(name(i).c_str()).x + 12.0f + separator);
            total += widths.back();
        }
        size_t hidden = 0;
        while (total > width && hidden + 2 < segments.size())
            total -= widths[1 + hidden++];

        ImGui::BeginGroup();
        ImGui::PushStyleColor(ImGuiCol_Button, ImVec4(0, 0, 0, 0));
        for (size_t i = 0; i < segments.size(); ++i)
        {
            if (i >= 1 && i <= hidden)
            {
                if (i == 1)
                {
                    ImGui::SameLine(0, 2.0f);
                    if (ImGui::Button("..."))
                        ImGui::OpenPopup("##HiddenCrumbs");
                    if (ImGui::BeginPopup("##HiddenCrumbs"))
                    {
                        for (size_t h = 1; h <= hidden; ++h)
                            if (ImGui::MenuItem(name(h).c_str()))
                                Navigate(segments[h]);
                        ImGui::EndPopup();
                    }
                }
                continue;
            }
            if (i > 0)
                ImGui::SameLine(0, 0);
            ImGui::PushID(int(i));
            const bool last = i + 1 == segments.size();
            if (last)
                ImGui::PushStyleColor(ImGuiCol_Text, EditorTheme::Colors().Text);
            if (ImGui::Button(name(i).c_str()) && !last)
                Navigate(segments[i]);
            if (last)
                ImGui::PopStyleColor();
            AcceptDrop(segments[i]);
            // Each segment's subfolders, one click away.
            if (m_Cache.HasSubdirectories(segments[i]))
            {
                ImGui::SameLine(0, 0);
                ImGui::PushStyleColor(ImGuiCol_Text, EditorTheme::Colors().TextDim);
                if (ImGui::Button(ICON_FA_CHEVRON_RIGHT "##sub"))
                    ImGui::OpenPopup("##Subfolders");
                ImGui::PopStyleColor();
                if (ImGui::BeginPopup("##Subfolders"))
                {
                    for (const auto& entry : m_Cache.List(segments[i]))
                        if (entry.Directory &&
                            ImGui::MenuItem(Label(ICON_FA_FOLDER, entry.Name.c_str()).c_str(), nullptr,
                                            i + 1 < segments.size() && entry.Path == segments[i + 1]))
                            Navigate(entry.Path);
                    ImGui::EndPopup();
                }
            }
            ImGui::PopID();
        }
        ImGui::PopStyleColor();
        ImGui::EndGroup();
    }

    // ---------------------------------------------------------------------- tree

    void ContentBrowserPanel::DrawTree()
    {
        const auto section = [](const char* title) {
            ImGui::PushStyleColor(ImGuiCol_Text, EditorTheme::Colors().TextDim);
            ImGui::PushFont(nullptr, ImGui::GetStyle().FontSizeBase * 0.85f);
            ImGui::TextUnformatted(title);
            ImGui::PopFont();
            ImGui::PopStyleColor();
        };
        if (!m_Favorites.empty())
        {
            section("FAVORITES");
            std::string remove;
            for (const auto& favorite : m_Favorites)
            {
                const auto path = Normal(m_Root / fs::u8path(favorite));
                std::error_code error;
                if (!fs::is_directory(path, error))
                    continue;
                ImGui::PushID(favorite.c_str());
                // Parent/name tells apart favorites with the same name.
                const auto        parent = path.parent_path() == m_Root ? std::string() :
                                                                            path.parent_path().filename().u8string() + "/";
                const std::string label  = std::string(ICON_FA_FOLDER "  ") + parent + path.filename().u8string();
                if (ImGui::Selectable(label.c_str(), path == m_Current))
                    Navigate(path);
                if (ImGui::IsItemHovered())
                    ImGui::SetTooltip("%s", favorite.c_str());
                AcceptDrop(path);
                if (ImGui::BeginPopupContextItem("##Favorite"))
                {
                    if (ImGui::MenuItem(Label(ICON_FA_STAR, "Remove from Favorites").c_str()))
                        remove = favorite;
                    if (ImGui::MenuItem(Label(ICON_FA_UP_RIGHT_FROM_SQUARE, "Show in File Manager").c_str()))
                        Reveal(path);
                    ImGui::EndPopup();
                }
                ImGui::PopID();
            }
            if (!remove.empty())
                m_Favorites.erase(std::remove(m_Favorites.begin(), m_Favorites.end(), remove), m_Favorites.end());
            ImGui::Dummy({0, EditorTheme::Px(4.0f)});
        }
        section("FOLDERS");
        ImGui::SetNextItemOpen(true, ImGuiCond_Once);
        DrawTreeNode(m_Root, m_Root.filename().u8string());
        m_RevealCurrent = false;
    }

    void ContentBrowserPanel::DrawTreeNode(const fs::path& directory, const std::string& label)
    {
        const bool         children = m_Cache.HasSubdirectories(directory);
        ImGuiTreeNodeFlags flags    = ImGuiTreeNodeFlags_OpenOnArrow | ImGuiTreeNodeFlags_OpenOnDoubleClick |
                                   ImGuiTreeNodeFlags_SpanFullWidth;
        if (directory == m_Current)
            flags |= ImGuiTreeNodeFlags_Selected;
        if (!children)
            flags |= ImGuiTreeNodeFlags_Leaf;
        if (m_RevealCurrent && directory != m_Current && Browser::IsWithin(m_Current, directory))
            ImGui::SetNextItemOpen(true);
        const std::string id    = directory.generic_u8string();
        const float       icon  = ImGui::GetFontSize();
        const float       start = ImGui::GetCursorScreenPos().x;
        const bool        open  = ImGui::TreeNodeEx(
            id.c_str(), flags, "%s%s", IconSpace(icon + EditorTheme::Px(6.0f)).c_str(), label.c_str());
        // The current folder and expanded folders show the open folder.
        const auto& image = directory == m_Current || (open && children) ? EditorResources::FolderOpenIcon :
                                                                           EditorResources::FolderIcon;
        if (image)
        {
            const ImVec2 at(start + ImGui::GetTreeNodeToLabelSpacing(),
                            (ImGui::GetItemRectMin().y + ImGui::GetItemRectMax().y - icon) * 0.5f);
            ImGui::GetWindowDrawList()->AddImage(static_cast<ImTextureID>(uintptr_t(image->GetImGuiTextureID())),
                                                 at,
                                                 {at.x + icon, at.y + icon},
                                                 {0, 1},
                                                 {1, 0});
        }
        if (ImGui::IsItemClicked(ImGuiMouseButton_Left) && !ImGui::IsItemToggledOpen())
            Navigate(directory);
        if (ImGui::IsItemClicked(ImGuiMouseButton_Right))
        {
            m_MenuFolder     = directory;
            m_MenuTargets    = directory == m_Root ? std::vector<fs::path> {} : std::vector<fs::path> {directory};
            m_OpenFolderMenu = true;
        }
        if (directory != m_Root && ImGui::BeginDragDropSource())
        {
            m_DragItems                = {directory};
            const std::string payload = directory.generic_u8string();
            ImGui::SetDragDropPayload("asset_payload", payload.data(), payload.size());
            ImGui::Text("%s  %s", ICON_FA_FOLDER, label.c_str());
            ImGui::EndDragDropSource();
        }
        AcceptDrop(directory);
        if (!open)
            return;
        for (const auto& entry : m_Cache.List(directory))
            if (entry.Directory)
                DrawTreeNode(entry.Path, entry.Name);
        ImGui::TreePop();
    }

    // ---------------------------------------------------------------------- items

    void ContentBrowserPanel::DrawItems()
    {
        ImGui::PushStyleColor(ImGuiCol_ChildBg, EditorTheme::Colors().Inset);
        ImGui::BeginChild("##Items", {0, m_ColumnHeight}, ImGuiChildFlags_AlwaysUseWindowPadding);
        ImGui::PopStyleColor();
        if (m_Visible.empty())
        {
            const char* text = m_Filter.IsActive() ? "No items match the filter." : "This folder is empty.";
            ImGui::SetCursorPosY(ImGui::GetCursorPosY() + 20.0f);
            ImGui::SetCursorPosX((ImGui::GetWindowWidth() - ImGui::CalcTextSize(text).x) * 0.5f);
            ImGui::TextDisabled("%s", text);
        }
        else if (m_View == View::Grid)
            DrawGrid();
        else
            DrawList();

        // Background: click clears the selection, right-click opens the folder menu.
        const ImGuiIO& io = ImGui::GetIO();
        if (ImGui::IsWindowHovered(ImGuiHoveredFlags_ChildWindows) && !ImGui::IsAnyItemHovered())
        {
            if (ImGui::IsMouseClicked(ImGuiMouseButton_Left) && !io.KeyCtrl && !io.KeyShift)
            {
                CommitRename();
                m_Selection.Clear();
            }
            if (ImGui::IsMouseClicked(ImGuiMouseButton_Right))
            {
                m_MenuFolder     = m_Current;
                m_MenuTargets    = {};
                m_OpenFolderMenu = true;
            }
        }
        if (m_View == View::Grid && ImGui::IsWindowHovered() && io.KeyCtrl && io.MouseWheel != 0.0f)
            m_IconSize = std::clamp(m_IconSize + io.MouseWheel * 8.0f, 48.0f, 192.0f);
        HandleKeyboard(m_View == View::Grid ? m_Columns : 1);
        ImGui::EndChild();
    }

    void ContentBrowserPanel::DrawGrid()
    {
        const float lineHeight = ImGui::GetTextLineHeight();
        const bool  details    = m_IconSize >= 80.0f;
        const float cardWidth  = m_IconSize + 16.0f;
        const float cardHeight = m_IconSize + 14.0f + lineHeight * (details ? 2.0f : 1.0f) + 4.0f;
        const float gap        = 10.0f;
        m_Columns = std::max(1, int((ImGui::GetContentRegionAvail().x + gap) / (cardWidth + gap)));
        ImDrawList* draw = ImGui::GetWindowDrawList();

        // Row spacing equals the clipper's row height.
        ImGui::PushStyleVar(ImGuiStyleVar_ItemSpacing, {ImGui::GetStyle().ItemSpacing.x, gap});
        ImGuiListClipper clipper;
        const int        rows = int((m_Visible.size() + m_Columns - 1) / m_Columns);
        clipper.Begin(rows, cardHeight + gap);
        const auto scrollTo = ScrollTarget();
        if (scrollTo >= 0)
            clipper.IncludeItemByIndex(scrollTo / m_Columns);
        while (clipper.Step())
            for (int row = clipper.DisplayStart; row < clipper.DisplayEnd; ++row)
                for (int column = 0; column < m_Columns; ++column)
                {
                    const size_t index = size_t(row) * m_Columns + column;
                    if (index >= m_Visible.size())
                        break;
                    const Entry& entry = m_Visible[index];
                    if (column)
                        ImGui::SameLine(0, gap);
                    ImGui::PushID(entry.Path.generic_u8string().c_str());
                    const ImVec2 min = ImGui::GetCursorScreenPos();
                    const ImVec2 max(min.x + cardWidth, min.y + cardHeight);
                    ImGui::SetNextItemAllowOverlap();
                    ImGui::InvisibleButton("##card", {cardWidth, cardHeight});
                    if (index == size_t(scrollTo))
                        ScrolledTo();
                    const bool hovered  = ImGui::IsItemHovered();
                    const bool selected = m_Selection.Contains(entry.Path);
                    HandleItem(entry);

                    const auto& palette = EditorTheme::Colors();
                    const float radius  = EditorTheme::Px(6.0f);
                    if (!entry.Directory)
                    {
                        // A card in the raised tone with the type's colour along its bottom edge.
                        draw->AddRectFilled(min, max, ImGui::GetColorU32(hovered ? palette.Hover : palette.Raised), radius);
                        draw->AddRectFilled({min.x, max.y - EditorTheme::Px(3.0f)}, max,
                                            ImGui::GetColorU32(KindColor(entry.Kind)), radius, ImDrawFlags_RoundCornersBottom);
                    }
                    else if (hovered)
                        draw->AddRectFilled(min, max, ImGui::GetColorU32(palette.Hover), radius);
                    if (selected)
                    {
                        draw->AddRectFilled(min, max, ImGui::GetColorU32(ImVec4 {palette.Accent.x, palette.Accent.y, palette.Accent.z, 0.18f}), radius);
                        draw->AddRect(min, max, ImGui::GetColorU32(palette.Accent), radius, 0, EditorTheme::Px(1.5f));
                    }
                    const ImVec2 iconMin(min.x + 8.0f, min.y + 7.0f);
                    const ImVec2 iconMax(iconMin.x + m_IconSize, iconMin.y + m_IconSize);
                    DrawIcon(entry, iconMin, iconMax);
                    DrawStatusBadge(entry, {iconMax.x, iconMin.y});

                    const float  textWidth = cardWidth - 8.0f;
                    const ImVec2 nameAt(min.x + 4.0f, iconMax.y + 5.0f);
                    if (m_Renaming == entry.Path)
                    {
                        ImGui::SetCursorScreenPos(nameAt);
                        DrawRenameField(entry, textWidth);
                    }
                    else
                    {
                        const auto name = Ellipsis(entry.Name, textWidth);
                        draw->AddText({min.x + (cardWidth - ImGui::CalcTextSize(name.c_str()).x) * 0.5f, nameAt.y},
                                      ImGui::GetColorU32(EditorTheme::Colors().Text),
                                      name.c_str());
                    }
                    if (details)
                    {
                        const std::string info =
                            entry.Directory ? std::string("Folder") :
                                              std::string(Browser::KindName(entry.Kind)) + "  " + FormatSize(entry.Size);
                        const auto text = Ellipsis(info, textWidth);
                        draw->AddText({min.x + (cardWidth - ImGui::CalcTextSize(text.c_str()).x) * 0.5f,
                                       nameAt.y + lineHeight + 1.0f},
                                      ImGui::GetColorU32(EditorTheme::Colors().TextDim),
                                      text.c_str());
                    }
                    // Keep the layout cursor after the card for SameLine.
                    ImGui::SetCursorScreenPos(min);
                    ImGui::Dummy({cardWidth, cardHeight});
                    ImGui::PopID();
                }
        ImGui::PopStyleVar();
    }

    void ContentBrowserPanel::DrawList()
    {
        const ImGuiTableFlags flags = ImGuiTableFlags_Resizable | ImGuiTableFlags_RowBg | ImGuiTableFlags_ScrollY |
                                      ImGuiTableFlags_Sortable | ImGuiTableFlags_BordersInnerV |
                                      ImGuiTableFlags_SizingFixedFit;
        if (!ImGui::BeginTable("##List", 5, flags))
            return;
        ImGui::TableSetupScrollFreeze(0, 1);
        ImGui::TableSetupColumn("Name", ImGuiTableColumnFlags_WidthStretch | ImGuiTableColumnFlags_DefaultSort);
        ImGui::TableSetupColumn("Type", ImGuiTableColumnFlags_WidthFixed, 80.0f);
        ImGui::TableSetupColumn("Size", ImGuiTableColumnFlags_WidthFixed, 80.0f);
        ImGui::TableSetupColumn("Modified", ImGuiTableColumnFlags_WidthFixed, 130.0f);
        ImGui::TableSetupColumn("Status", ImGuiTableColumnFlags_WidthFixed | ImGuiTableColumnFlags_NoSort, 100.0f);
        ImGui::TableHeadersRow();
        if (auto* specs = ImGui::TableGetSortSpecs(); specs && specs->SpecsDirty && specs->SpecsCount > 0)
        {
            m_SortKey   = Browser::SortKey(std::clamp(int(specs->Specs[0].ColumnIndex), 0, 3));
            m_Ascending = specs->Specs[0].SortDirection != ImGuiSortDirection_Descending;
            specs->SpecsDirty = false;
            Browser::SortEntries(m_Visible, m_SortKey, m_Ascending);
        }

        const float     rowHeight = ImGui::GetFrameHeight();
        ImGuiListClipper clipper;
        clipper.Begin(int(m_Visible.size()), rowHeight);
        const auto scrollTo = ScrollTarget();
        if (scrollTo >= 0)
            clipper.IncludeItemByIndex(scrollTo);
        while (clipper.Step())
            for (int i = clipper.DisplayStart; i < clipper.DisplayEnd; ++i)
            {
                const Entry& entry = m_Visible[i];
                ImGui::TableNextRow(0, rowHeight);
                ImGui::TableSetColumnIndex(0);
                ImGui::PushID(entry.Path.generic_u8string().c_str());
                const ImVec2 rowStart = ImGui::GetCursorScreenPos();
                ImGui::Selectable("##row",
                                  m_Selection.Contains(entry.Path),
                                  ImGuiSelectableFlags_SpanAllColumns | ImGuiSelectableFlags_AllowOverlap,
                                  {0, rowHeight});
                if (i == scrollTo)
                    ScrolledTo();
                HandleItem(entry);
                const float icon = rowHeight - 4.0f;
                DrawIcon(entry, {rowStart.x, rowStart.y + 2.0f}, {rowStart.x + icon, rowStart.y + 2.0f + icon});
                ImGui::SetCursorScreenPos({rowStart.x + icon + 6.0f, rowStart.y});
                if (m_Renaming == entry.Path)
                    DrawRenameField(entry, ImGui::GetContentRegionAvail().x);
                else
                {
                    ImGui::AlignTextToFramePadding();
                    ImGui::TextUnformatted(entry.Name.c_str());
                }
                ImGui::TableSetColumnIndex(1);
                ImGui::AlignTextToFramePadding();
                ImGui::TextDisabled("%s", Browser::KindName(entry.Kind));
                ImGui::TableSetColumnIndex(2);
                ImGui::AlignTextToFramePadding();
                if (!entry.Directory)
                    ImGui::TextDisabled("%s", FormatSize(entry.Size).c_str());
                ImGui::TableSetColumnIndex(3);
                ImGui::AlignTextToFramePadding();
                ImGui::TextDisabled("%s", FormatTime(entry.Modified).c_str());
                ImGui::TableSetColumnIndex(4);
                ImGui::AlignTextToFramePadding();
                if (const auto* status = entry.Directory ? nullptr : StatusOf(entry.Path))
                {
                    if (status->Products)
                        ImGui::TextColored(ImGui::ColorConvertU32ToFloat4(ImportedColor), ICON_FA_CHECK " Imported");
                    else if (CanImport(entry.Path))
                        ImGui::TextDisabled("Not imported");
                }
                ImGui::PopID();
            }
        ImGui::EndTable();
    }

    void ContentBrowserPanel::DrawIcon(const Entry& entry, ImVec2 min, ImVec2 max)
    {
        ImDrawList*    draw    = ImGui::GetWindowDrawList();
        Ref<Texture2D> texture = entry.Directory ? EditorResources::FolderIcon : nullptr;
        if (!entry.Directory && Browser::ThumbnailCache::Supports(entry.Path))
            texture = m_Thumbnails.Get(entry.Path, entry.Modified, max.x - min.x <= 32.0f ? 64u : 192u);
        else if (!entry.Directory && m_MaterialThumbnail && entry.Path.extension() == ".material")
            texture = m_MaterialThumbnail(entry.Path, 128);
        if (!texture)
            texture = entry.Directory ? EditorResources::FolderIcon : EditorResources::GetFileIcon(entry.Path);
        if (!texture)
            return;
        // Keep the image's aspect ratio inside the square.
        const float width  = float(texture->GetWidth()), height = float(texture->GetHeight());
        const float side   = max.x - min.x;
        const float scale  = side / std::max(width, height);
        const ImVec2 size(width * scale, height * scale);
        const ImVec2 at(min.x + (side - size.x) * 0.5f, min.y + (side - size.y) * 0.5f);
        draw->AddImage(static_cast<ImTextureID>(uintptr_t(texture->GetImGuiTextureID())),
                       at,
                       {at.x + size.x, at.y + size.y},
                       {0, 1},
                       {1, 0});
    }

    void ContentBrowserPanel::DrawStatusBadge(const Entry& entry, ImVec2 corner)
    {
        if (entry.Directory)
            return;
        const auto* status = StatusOf(entry.Path);
        if (!status)
            return;
        ImDrawList*  draw = ImGui::GetWindowDrawList();
        const ImVec2 center(corner.x - 6.0f, corner.y + 6.0f);
        if (status->Products)
        {
            draw->AddCircleFilled(center, 5.0f, ImportedColor);
            draw->AddCircle(center, 5.0f, IM_COL32(20, 20, 20, 255), 0, 1.0f);
        }
        else if (CanImport(entry.Path))
            draw->AddCircle(center, 4.5f, NotImportedColor, 0, 1.5f);
    }

    void ContentBrowserPanel::DrawRenameField(const Entry& entry, float width)
    {
        if (m_RenameFocus)
        {
            ImGui::SetKeyboardFocusHere();
            m_RenameFocus = false;
        }
        ImGui::SetNextItemWidth(width);
        // Activation selects everything; the callback then narrows the selection
        // to the name without its extension, once.
        const auto select = [](ImGuiInputTextCallbackData* data) {
            auto* selection = static_cast<int*>(data->UserData);
            if (*selection >= 0)
            {
                data->SelectionStart = 0;
                data->SelectionEnd = data->CursorPos = std::min(*selection, data->BufTextLen);
                *selection                            = -1;
            }
            return 0;
        };
        const bool submitted = ImGui::InputText("##rename",
                                                m_RenameBuffer.data(),
                                                m_RenameBuffer.size(),
                                                ImGuiInputTextFlags_EnterReturnsTrue | ImGuiInputTextFlags_CallbackAlways,
                                                select,
                                                &m_RenameSelection);
        if (ImGui::IsItemActive() && ImGui::IsKeyPressed(ImGuiKey_Escape))
        {
            m_Renaming.clear();
            return;
        }
        if (submitted || ImGui::IsItemDeactivated())
            CommitRename();
        (void)entry;
    }

    void ContentBrowserPanel::DrawItemTooltip(const Entry& entry)
    {
        ImGui::BeginTooltip();
        ImGui::TextUnformatted(entry.Name.c_str());
        ImGui::TextDisabled("%s", Browser::KindName(entry.Kind));
        if (!entry.Directory)
        {
            ImGui::TextDisabled("%s, modified %s", FormatSize(entry.Size).c_str(), FormatTime(entry.Modified).c_str());
            if (const auto* status = StatusOf(entry.Path))
            {
                if (status->Products)
                    ImGui::TextColored(ImGui::ColorConvertU32ToFloat4(ImportedColor),
                                       "Imported (%zu products)",
                                       status->Products);
                else if (CanImport(entry.Path))
                    ImGui::TextDisabled("Not imported");
            }
        }
        ImGui::EndTooltip();
    }

    void ContentBrowserPanel::HandleItem(const Entry& entry)
    {
        const ImGuiIO& io = ImGui::GetIO();
        if (ImGui::IsItemClicked(ImGuiMouseButton_Left))
        {
            if (m_Renaming != entry.Path)
                CommitRename();
            if (ImGui::IsMouseDoubleClicked(ImGuiMouseButton_Left))
            {
                m_Selection.Set(entry.Path);
                Open(entry);
                return;
            }
            m_Selection.Click(entry.Path, io.KeyCtrl, io.KeyShift, VisiblePaths());
        }
        if (ImGui::IsItemClicked(ImGuiMouseButton_Right))
        {
            if (!m_Selection.Contains(entry.Path))
                m_Selection.Set(entry.Path);
            m_MenuTargets  = m_Selection.Ordered(VisiblePaths());
            m_MenuFolder   = m_Current;
            m_OpenItemMenu = true;
        }
        if (ImGui::BeginDragDropSource())
        {
            m_DragItems                = Targets(entry.Path);
            const std::string payload = entry.Path.generic_u8string();
            ImGui::SetDragDropPayload("asset_payload", payload.data(), payload.size());
            if (m_DragItems.size() > 1)
                ImGui::Text("%s  %zu items", ICON_FA_CLONE, m_DragItems.size());
            else
                ImGui::Text("%s  %s", entry.Directory ? ICON_FA_FOLDER : ICON_FA_FILE, entry.Name.c_str());
            ImGui::EndDragDropSource();
        }
        if (entry.Directory)
            AcceptDrop(entry.Path);
        if (ImGui::IsItemHovered() && GImGui->HoveredIdTimer > 0.6f && !ImGui::IsMouseDragging(ImGuiMouseButton_Left) &&
            m_Renaming != entry.Path)
            DrawItemTooltip(entry);
    }

    void ContentBrowserPanel::AcceptDrop(const fs::path& directory)
    {
        if (!ImGui::BeginDragDropTarget())
            return;
        if (const auto* payload = ImGui::AcceptDragDropPayload("asset_payload"))
        {
            const fs::path dropped =
                fs::u8path(std::string(static_cast<const char*>(payload->Data), size_t(payload->DataSize)));
            const bool fromBrowser =
                std::find(m_DragItems.begin(), m_DragItems.end(), dropped) != m_DragItems.end();
            MoveInto(fromBrowser ? m_DragItems : std::vector<fs::path> {dropped}, directory);
            m_DragItems.clear();
        }
        ImGui::EndDragDropTarget();
    }

    void ContentBrowserPanel::HandleKeyboard(int columns)
    {
        const ImGuiIO& io = ImGui::GetIO();
        if (!ImGui::IsWindowFocused(ImGuiFocusedFlags_RootAndChildWindows) || io.WantTextInput || !m_Renaming.empty())
            return;
        const auto paths   = VisiblePaths();
        const auto targets = m_Selection.Ordered(paths);
        const auto pressed = [](ImGuiKey key) { return ImGui::IsKeyPressed(key, false); };
        if (io.KeyCtrl)
        {
            if (pressed(ImGuiKey_A))
                m_Selection.SetAll(paths);
            if (pressed(ImGuiKey_C) && !targets.empty())
                CopyToClipboard(targets, false);
            if (pressed(ImGuiKey_X) && !targets.empty())
                CopyToClipboard(targets, true);
            if (pressed(ImGuiKey_V))
                Paste(m_Current);
            if (pressed(ImGuiKey_D) && !targets.empty())
                Duplicate(targets);
            return;
        }
        if (io.KeyAlt)
        {
            if (pressed(ImGuiKey_LeftArrow) && !m_Back.empty())
            {
                m_Forward.push_back(m_Current);
                const auto target = m_Back.back();
                m_Back.pop_back();
                Navigate(target, false);
            }
            if (pressed(ImGuiKey_RightArrow) && !m_Forward.empty())
            {
                m_Back.push_back(m_Current);
                const auto target = m_Forward.back();
                m_Forward.pop_back();
                Navigate(target, false);
            }
            if (pressed(ImGuiKey_UpArrow) && m_Current != m_Root)
                Navigate(m_Current.parent_path());
            return;
        }
        if (pressed(ImGuiKey_Backspace) && m_Current != m_Root)
            Navigate(m_Current.parent_path());
        if (pressed(ImGuiKey_F2) && targets.size() == 1)
            StartRename(targets.front());
        if (pressed(ImGuiKey_Delete) && !targets.empty())
            RequestDelete(targets);
        if (pressed(ImGuiKey_Enter) && targets.size() == 1)
            for (const auto& entry : m_Visible)
                if (entry.Path == targets.front())
                {
                    Open(entry);
                    break;
                }
        // Arrow keys move the selection.
        if (paths.empty())
            return;
        int step = 0;
        if (pressed(ImGuiKey_RightArrow))
            step = 1;
        if (pressed(ImGuiKey_LeftArrow))
            step = -1;
        if (pressed(ImGuiKey_DownArrow))
            step = columns;
        if (pressed(ImGuiKey_UpArrow))
            step = -columns;
        if (!step)
            return;
        const auto anchor = std::find(paths.begin(), paths.end(), m_Selection.Anchor());
        const int  index  = anchor == paths.end() ? 0 :
                                                    std::clamp(int(anchor - paths.begin()) + step, 0, int(paths.size()) - 1);
        m_Selection.Set(paths[index]);
    }

    // ---------------------------------------------------------------------- preview

    void ContentBrowserPanel::DrawPreview()
    {
        const auto selected = m_Selection.Ordered(VisiblePaths());
        if (selected.size() != 1)
        {
            if (selected.empty())
            {
                ImGui::TextUnformatted((std::string(ICON_FA_FOLDER_OPEN "  ") + m_Current.filename().u8string()).c_str());
                ImGui::TextDisabled("%zu items", m_Visible.size());
            }
            else
            {
                uint64_t total = 0;
                for (const auto& entry : m_Visible)
                    if (m_Selection.Contains(entry.Path))
                        total += entry.Size;
                ImGui::Text("%zu items selected", selected.size());
                ImGui::TextDisabled("%s", FormatSize(total).c_str());
            }
            return;
        }
        const auto  path  = selected.front();
        const auto  found = std::find_if(m_Visible.begin(), m_Visible.end(), [&](const Entry& e) { return e.Path == path; });
        if (found == m_Visible.end())
            return;
        const Entry& entry = *found;
        if (path != m_PreviewPath)
        {
            m_PreviewPath = path;
            m_PreviewText.clear();
            m_Dependents.reset();
            if (!entry.Directory && IsTextFile(entry.Extension))
            {
                std::ifstream file(path, std::ios::binary);
                m_PreviewText.resize(16 * 1024);
                file.read(m_PreviewText.data(), std::streamsize(m_PreviewText.size()));
                m_PreviewText.resize(size_t(file.gcount()));
            }
        }

        ImGui::PushTextWrapPos(0.0f);
        ImGui::TextUnformatted(entry.Name.c_str());
        ImGui::PopTextWrapPos();
        ImGui::TextDisabled("%s", Browser::KindName(entry.Kind));
        ImGui::Spacing();

        // Visual preview.
        const float width = ImGui::GetContentRegionAvail().x;
        Ref<Texture2D> visual;
        if (!entry.Directory && Browser::ThumbnailCache::Supports(path))
            visual = m_Thumbnails.Get(path, entry.Modified, 512);
        else if (!entry.Directory && m_MaterialThumbnail && path.extension() == ".material")
            visual = m_MaterialThumbnail(path, 256);
        if (visual)
        {
            if (auto texture = visual)
            {
                const float w = float(texture->GetWidth()), h = float(texture->GetHeight());
                // Leave room for the details below in short panels.
                const float limit = std::clamp(ImGui::GetContentRegionAvail().y * 0.4f, 64.0f, 260.0f);
                const float scale = std::min(width / w, limit / h);
                ImGui::Image(static_cast<ImTextureID>(uintptr_t(texture->GetImGuiTextureID())),
                             {w * scale, h * scale},
                             {0, 1},
                             {1, 0});
            }
            else
                ImGui::TextDisabled("Loading preview...");
        }
        else if (!m_PreviewText.empty())
            ImGui::InputTextMultiline("##text",
                                      m_PreviewText.data(),
                                      m_PreviewText.size() + 1,
                                      {width, 160.0f},
                                      ImGuiInputTextFlags_ReadOnly);
        else
        {
            const ImVec2 at = ImGui::GetCursorScreenPos();
            const float  side = std::min(width, 96.0f);
            DrawIcon(entry, at, {at.x + side, at.y + side});
            ImGui::Dummy({side, side});
        }

        ImGui::Spacing();
        if (ImGui::CollapsingHeader("Details", ImGuiTreeNodeFlags_DefaultOpen) && PropertyGrid::Begin("##Details", 72.0f))
        {
            if (!entry.Directory)
                PropertyGrid::Text("Size", FormatSize(entry.Size));
            PropertyGrid::Text("Modified", FormatTime(entry.Modified));
            const auto folder = path.parent_path().lexically_relative(m_Root).generic_u8string();
            PropertyGrid::Text("Folder", folder == "." ? m_Root.filename().u8string() : folder);
            PropertyGrid::End();
        }

        const auto* status = entry.Directory ? nullptr : StatusOf(path);
        if (!status)
            return;
        if (!ImGui::CollapsingHeader("Asset", ImGuiTreeNodeFlags_DefaultOpen))
            return;
        const auto& source = status->Source;
        if (PropertyGrid::Begin("##Asset", 72.0f))
        {
            PropertyGrid::Row("Source ID");
            ImGui::AlignTextToFramePadding();
            ImGui::TextUnformatted(source.sourceAssetID.ToString().c_str());
            ImGui::SameLine();
            if (ImGui::SmallButton(ICON_FA_COPY "##id"))
                ImGui::SetClipboardText(source.sourceAssetID.ToString().c_str());
            if (ImGui::IsItemHovered())
                ImGui::SetTooltip("Copy the source ID");
            if (CanImport(path))
            {
                const auto importer = Importers().Resolve(path, source.Import.Importer);
                PropertyGrid::Text("Importer",
                                   std::holds_alternative<RegisteredImporter>(importer) ?
                                       std::get<RegisteredImporter>(importer).Name :
                                       std::string("(invalid)"));
                std::string settings;
                for (const auto& [key, value] : source.Import.Settings)
                    settings += (settings.empty() ? "" : "\n") + key + " = " + value;
                PropertyGrid::Text("Settings", settings.empty() ? "Defaults" : settings, settings.empty());
                PropertyGrid::Row("Status");
                ImGui::AlignTextToFramePadding();
                if (status->Products)
                    ImGui::TextColored(ImGui::ColorConvertU32ToFloat4(ImportedColor),
                                       ICON_FA_CHECK " Imported (%zu)",
                                       status->Products);
                else
                    ImGui::TextDisabled("Not imported");
            }
            PropertyGrid::End();
        }
        if (status->Products && m_Registry && ImGui::TreeNode("Products"))
        {
            for (const auto& record : m_Registry->GetAllProducts())
                if (record.Target == EditorAssetTarget &&
                    record.Metadata.assetID.GetSourceID() == source.sourceAssetID)
                    ImGui::BulletText("%s  #%llu",
                                      ProductTypeName(record.Metadata.Type),
                                      static_cast<unsigned long long>(record.Metadata.assetID.GetLocalID()));
            ImGui::TreePop();
        }
        if (CanImport(path))
        {
            const float half = (ImGui::GetContentRegionAvail().x - ImGui::GetStyle().ItemSpacing.x) * 0.5f;
            if (ImGui::Button(status->Products ? ICON_FA_ROTATE_RIGHT " Reimport" : ICON_FA_DOWNLOAD " Import",
                              {half, 0.0f}) &&
                m_Import)
                m_Import(path);
            ImGui::SameLine();
            if (ImGui::Button(ICON_FA_GEAR " Settings", {half, 0.0f}) && m_ImportSettings)
                m_ImportSettings(path);
        }
        ImGui::Spacing();
        if (!m_Dependents)
        {
            if (ImGui::Button(ICON_FA_MAGNIFYING_GLASS " Find references", {-FLT_MIN, 0.0f}) && m_Service)
            {
                auto plan = m_Service->PlanSourceAssetDeletion(source.sourceAssetID);
                if (auto* value = std::get_if<SourceAssetDeletionPlan>(&plan))
                    m_Dependents = value->Dependents;
                else
                    Report(std::get<AssetError>(plan));
            }
        }
        else if (m_Dependents->empty())
            ImGui::TextDisabled("No other asset references it.");
        else
        {
            ImGui::TextDisabled("Referenced by:");
            for (const auto& dependent : *m_Dependents)
                ImGui::BulletText("%s", dependent.Path.ToString().c_str());
        }
    }

    // ---------------------------------------------------------------------- menus and dialogs

    void ContentBrowserPanel::DrawContextMenus()
    {
        if (m_OpenItemMenu)
            ImGui::OpenPopup("##ItemMenu");
        if (m_OpenFolderMenu)
            ImGui::OpenPopup("##FolderMenu");
        m_OpenItemMenu = m_OpenFolderMenu = false;
        const bool hasClipboard           = !m_Clipboard.empty();

        if (ImGui::BeginPopup("##ItemMenu"))
        {
            const auto&     targets = m_MenuTargets;
            const bool      single  = targets.size() == 1;
            std::error_code error;
            const bool      folder = single && fs::is_directory(targets.front(), error);
            if (single)
            {
                if (ImGui::MenuItem(Label(folder ? ICON_FA_FOLDER_OPEN : ICON_FA_FILE, "Open").c_str(), "Enter"))
                    for (const auto& entry : m_Visible)
                        if (entry.Path == targets.front())
                            Open(entry);
                if (!folder && CanImport(targets.front()))
                {
                    const auto* status = StatusOf(targets.front());
                    if (ImGui::MenuItem(Label(status && status->Products ? ICON_FA_ROTATE_RIGHT : ICON_FA_DOWNLOAD,
                                              status && status->Products ? "Reimport" : "Import")
                                            .c_str()) &&
                        m_Import)
                        m_Import(targets.front());
                    if (ImGui::MenuItem(Label(ICON_FA_GEAR, "Import Settings...").c_str()) && m_ImportSettings)
                        m_ImportSettings(targets.front());
                    if (targets.front().extension() == ".material" &&
                        ImGui::MenuItem(Label(ICON_FA_CLONE, "Create Material Instance").c_str(),
                                        nullptr,
                                        false,
                                        status && status->Products) &&
                        m_CreateInstance)
                        m_CreateInstance(targets.front());
                    if (Browser::KindOf(targets.front(), false) == Browser::AssetKind::Model &&
                        ImGui::MenuItem(Label(ICON_FA_PALETTE, "Extract Materials").c_str(),
                                        nullptr,
                                        false,
                                        status && status->Products) &&
                        m_ExtractMaterials)
                        m_ExtractMaterials(targets.front());
                    // A ragdoll generated from the model's skeleton (opens the ragdoll editor).
                    if (Browser::KindOf(targets.front(), false) == Browser::AssetKind::Model &&
                        ImGui::MenuItem(Label(ICON_FA_PERSON_FALLING, "Create Ragdoll").c_str(),
                                        nullptr,
                                        false,
                                        status && status->Products && bool(m_CreateRagdoll)))
                        m_CreateRagdoll(targets.front());
                }
                if (folder && ImGui::MenuItem(Label(ICON_FA_STAR, IsFavorite(targets.front()) ?
                                                                      "Remove from Favorites" :
                                                                      "Add to Favorites")
                                                  .c_str()))
                    ToggleFavorite(targets.front());
                ImGui::Separator();
            }
            if (ImGui::MenuItem(Label(ICON_FA_PEN, "Rename").c_str(), "F2", false, single))
                StartRename(targets.front());
            if (ImGui::MenuItem(Label(ICON_FA_CLONE, "Duplicate").c_str(), "Ctrl+D"))
                Duplicate(targets);
            if (ImGui::MenuItem(Label(ICON_FA_SCISSORS, "Cut").c_str(), "Ctrl+X"))
                CopyToClipboard(targets, true);
            if (ImGui::MenuItem(Label(ICON_FA_COPY, "Copy").c_str(), "Ctrl+C"))
                CopyToClipboard(targets, false);
            if (folder && ImGui::MenuItem(Label(ICON_FA_PASTE, "Paste Into").c_str(), nullptr, false, hasClipboard))
                Paste(targets.front());
            ImGui::PushStyleColor(ImGuiCol_Text, ImVec4(0.95f, 0.45f, 0.40f, 1.0f));
            if (ImGui::MenuItem(Label(ICON_FA_TRASH, "Delete").c_str(), "Del"))
                RequestDelete(targets);
            ImGui::PopStyleColor();
            if (single)
            {
                ImGui::Separator();
                if (ImGui::MenuItem(Label(ICON_FA_COPY, "Copy Path").c_str()))
                    ImGui::SetClipboardText(targets.front().u8string().c_str());
                if (const auto* status = folder ? nullptr : StatusOf(targets.front());
                    status && ImGui::MenuItem(Label(ICON_FA_COPY, "Copy Source ID").c_str()))
                    ImGui::SetClipboardText(status->Source.sourceAssetID.ToString().c_str());
                if (ImGui::MenuItem(Label(ICON_FA_UP_RIGHT_FROM_SQUARE, "Show in File Manager").c_str()))
                    Reveal(targets.front());
            }
            ImGui::EndPopup();
        }

        if (ImGui::BeginPopup("##FolderMenu"))
        {
            const auto folder = m_MenuFolder;
            if (ImGui::BeginMenu(Label(ICON_FA_PLUS, "Create").c_str()))
            {
                if (ImGui::MenuItem(Label(ICON_FA_FOLDER_PLUS, "Folder").c_str()))
                    NewFolder(folder);
                if (ImGui::MenuItem(Label(ICON_FA_PALETTE, "Material").c_str()) && m_CreateMaterial)
                {
                    m_CreateMaterial(folder);
                    Refresh();
                }
                if (ImGui::MenuItem(Label(ICON_FA_DESKTOP, "UI").c_str()) && m_CreateUI)
                    m_CreateUI(folder), Refresh();
                if (ImGui::MenuItem(Label(ICON_FA_FILM, "Timeline").c_str()) && m_CreateTimeline)
                    m_CreateTimeline(folder), Refresh();
                if (ImGui::MenuItem(Label(ICON_FA_ATOM, "Physics Material").c_str()) && m_CreatePhysicsMaterial)
                    m_CreatePhysicsMaterial(folder), Refresh();
                ImGui::EndMenu();
            }
            if (ImGui::MenuItem(Label(ICON_FA_PASTE, "Paste").c_str(), "Ctrl+V", false, hasClipboard))
                Paste(folder);
            if (!m_MenuTargets.empty())
            {
                ImGui::Separator();
                if (ImGui::MenuItem(Label(ICON_FA_STAR, IsFavorite(folder) ? "Remove from Favorites" : "Add to Favorites")
                                        .c_str()))
                    ToggleFavorite(folder);
                if (ImGui::MenuItem(Label(ICON_FA_PEN, "Rename").c_str()))
                {
                    Navigate(folder.parent_path());
                    StartRename(folder);
                }
                if (ImGui::MenuItem(Label(ICON_FA_SCISSORS, "Cut").c_str()))
                    CopyToClipboard(m_MenuTargets, true);
                if (ImGui::MenuItem(Label(ICON_FA_COPY, "Copy").c_str()))
                    CopyToClipboard(m_MenuTargets, false);
                ImGui::PushStyleColor(ImGuiCol_Text, ImVec4(0.95f, 0.45f, 0.40f, 1.0f));
                if (ImGui::MenuItem(Label(ICON_FA_TRASH, "Delete").c_str()))
                    RequestDelete(m_MenuTargets);
                ImGui::PopStyleColor();
            }
            else if (folder != m_Root && ImGui::MenuItem(Label(ICON_FA_STAR, IsFavorite(folder) ?
                                                                                "Remove from Favorites" :
                                                                                "Add to Favorites")
                                                              .c_str()))
                ToggleFavorite(folder);
            ImGui::Separator();
            if (ImGui::BeginMenu(Label(ICON_FA_TABLE_CELLS_LARGE, "View").c_str()))
            {
                if (ImGui::MenuItem("Grid", nullptr, m_View == View::Grid))
                    m_View = View::Grid;
                if (ImGui::MenuItem("List", nullptr, m_View == View::List))
                    m_View = View::List;
                ImGui::MenuItem("Preview pane", nullptr, &m_ShowPreview);
                ImGui::EndMenu();
            }
            if (ImGui::MenuItem(Label(ICON_FA_ROTATE_RIGHT, "Refresh").c_str()))
            {
                Refresh();
                if (m_Service)
                    (void)m_Service->RefreshSources();
            }
            if (ImGui::MenuItem(Label(ICON_FA_COPY, "Copy Path").c_str()))
                ImGui::SetClipboardText(folder.u8string().c_str());
            if (ImGui::MenuItem(Label(ICON_FA_UP_RIGHT_FROM_SQUARE, "Show in File Manager").c_str()))
                Reveal(folder);
            ImGui::EndPopup();
        }
    }

    void ContentBrowserPanel::DrawDeleteDialog()
    {
        if (m_OpenDeleteDialog)
        {
            ImGui::OpenPopup("Delete Assets##Browser");
            m_OpenDeleteDialog = false;
        }
        if (!ImGui::BeginPopupModal("Delete Assets##Browser", nullptr, ImGuiWindowFlags_AlwaysAutoResize))
            return;
        ImGui::Text("Delete %zu item%s?", m_DeleteTargets.size(), m_DeleteTargets.size() == 1 ? "" : "s");
        for (size_t i = 0; i < std::min<size_t>(m_DeleteTargets.size(), 8); ++i)
            ImGui::BulletText("%s", m_DeleteTargets[i].lexically_relative(m_Root).generic_u8string().c_str());
        if (m_DeleteTargets.size() > 8)
            ImGui::TextDisabled("...and %zu more", m_DeleteTargets.size() - 8);
        if (m_DeletePreview)
        {
            if (m_DeletePreview->Folders)
                ImGui::TextDisabled("%zu files in total, including the contents of %zu folder%s.",
                                    m_DeletePreview->Files,
                                    m_DeletePreview->Folders,
                                    m_DeletePreview->Folders == 1 ? "" : "s");
            if (!m_DeletePreview->Dependents.empty())
            {
                ImGui::Spacing();
                ImGui::TextColored(WarningColor,
                                   ICON_FA_TRIANGLE_EXCLAMATION " %zu other asset%s reference these and lose the "
                                   "reference until reimported:",
                                   m_DeletePreview->Dependents.size(),
                                   m_DeletePreview->Dependents.size() == 1 ? "" : "s");
                for (size_t i = 0; i < std::min<size_t>(m_DeletePreview->Dependents.size(), 8); ++i)
                    ImGui::BulletText("%s", m_DeletePreview->Dependents[i].Path.ToString().c_str());
            }
        }
        ImGui::Spacing();
        ImGui::TextDisabled("Files are deleted from disk. This cannot be undone.");
        ImGui::Spacing();
        ImGui::PushStyleColor(ImGuiCol_Button, DangerColor);
        if (ImGui::Button(ICON_FA_TRASH " Delete", {120.0f, 0.0f}))
        {
            auto removed = m_Operations.Delete(m_DeleteTargets);
            if (const auto* error = std::get_if<AssetError>(&removed))
                Report(*error);
            m_Selection.Clear();
            m_DeleteTargets.clear();
            Refresh();
            ImGui::CloseCurrentPopup();
        }
        ImGui::PopStyleColor();
        ImGui::SameLine();
        if (ImGui::Button("Cancel", {120.0f, 0.0f}) || ImGui::IsKeyPressed(ImGuiKey_Escape))
        {
            m_DeleteTargets.clear();
            ImGui::CloseCurrentPopup();
        }
        ImGui::EndPopup();
    }

    // ---------------------------------------------------------------------- actions

    void ContentBrowserPanel::Open(const Entry& entry)
    {
        if (entry.Directory)
            Navigate(entry.Path);
        else if (m_OpenFile)
            m_OpenFile(entry.Path);
    }

    void ContentBrowserPanel::StartRename(const fs::path& item)
    {
        m_Renaming = item;
        m_RenameBuffer.fill(0);
        const auto name = item.filename().u8string();
        std::copy_n(name.begin(), std::min(name.size(), m_RenameBuffer.size() - 1), m_RenameBuffer.begin());
        m_RenameFocus = true;
        // "clothes_Light.tga" selects "clothes_Light"; folders and ".hidden" names select all.
        std::error_code error;
        const auto      dot = name.rfind('.');
        m_RenameSelection   = !fs::is_directory(item, error) && dot != std::string::npos && dot > 0 ?
                                  int(dot) :
                                  int(name.size());
        m_Selection.Set(item);
    }

    void ContentBrowserPanel::CommitRename()
    {
        if (m_Renaming.empty())
            return;
        const auto item = m_Renaming;
        m_Renaming.clear();
        const std::string name = m_RenameBuffer.data();
        if (name.empty() || name == item.filename().u8string())
            return;
        auto renamed = m_Operations.Rename(item, name);
        if (const auto* error = std::get_if<AssetError>(&renamed))
            Report(*error);
        else
            SelectAfterChange({std::get<fs::path>(renamed)});
    }

    void ContentBrowserPanel::RequestDelete(std::vector<fs::path> items)
    {
        if (items.empty())
            return;
        auto preview = m_Operations.PlanDelete(items);
        if (const auto* error = std::get_if<AssetError>(&preview))
        {
            Report(*error);
            m_DeletePreview.reset();
        }
        else
            m_DeletePreview = std::get<Browser::DeletionPreview>(preview);
        m_DeleteTargets    = std::move(items);
        m_OpenDeleteDialog = true;
    }

    void ContentBrowserPanel::Duplicate(const std::vector<fs::path>& items)
    {
        std::vector<fs::path> copies;
        // Copies go next to their originals.
        for (const auto& item : items)
        {
            auto copied = m_Operations.Copy({item}, item.parent_path());
            if (const auto* error = std::get_if<AssetError>(&copied))
            {
                Report(*error);
                break;
            }
            for (const auto& copy : std::get<std::vector<fs::path>>(copied))
                copies.push_back(copy);
        }
        SelectAfterChange(copies);
    }

    void ContentBrowserPanel::CopyToClipboard(const std::vector<fs::path>& items, bool cut)
    {
        m_Clipboard    = items;
        m_ClipboardCut = cut;
        // Also as text for other applications.
        std::string text;
        for (const auto& item : items)
            text += item.u8string() + "\n";
        ImGui::SetClipboardText(text.c_str());
    }

    void ContentBrowserPanel::Paste(const fs::path& directory)
    {
        if (m_Clipboard.empty())
            return;
        if (m_ClipboardCut)
        {
            MoveInto(m_Clipboard, directory);
            m_Clipboard.clear();
            return;
        }
        auto copied = m_Operations.Copy(m_Clipboard, directory);
        if (const auto* error = std::get_if<AssetError>(&copied))
            Report(*error);
        else if (Normal(directory) == m_Current)
            SelectAfterChange(std::get<std::vector<fs::path>>(copied));
        Refresh();
    }

    void ContentBrowserPanel::MoveInto(const std::vector<fs::path>& items, const fs::path& directory)
    {
        std::vector<fs::path> moving;
        for (const auto& item : items)
            if (Normal(item.parent_path()) != Normal(directory) && Normal(item) != Normal(directory))
                moving.push_back(item);
        if (moving.empty())
            return;
        auto moved = m_Operations.Move(moving, directory);
        if (const auto* error = std::get_if<AssetError>(&moved))
            Report(*error);
        m_Selection.Clear();
        Refresh();
    }

    void ContentBrowserPanel::NewFolder(const fs::path& parent)
    {
        auto created = m_Operations.CreateFolder(parent);
        if (const auto* error = std::get_if<AssetError>(&created))
        {
            Report(*error);
            return;
        }
        Navigate(parent);
        Refresh();
        StartRename(std::get<fs::path>(created));
    }

    bool ContentBrowserPanel::IsFavorite(const fs::path& directory) const
    {
        const auto relative = Normal(directory).lexically_relative(m_Root).generic_u8string();
        return std::find(m_Favorites.begin(), m_Favorites.end(), relative) != m_Favorites.end();
    }

    void ContentBrowserPanel::ToggleFavorite(const fs::path& directory)
    {
        const auto relative = Normal(directory).lexically_relative(m_Root).generic_u8string();
        const auto found    = std::find(m_Favorites.begin(), m_Favorites.end(), relative);
        if (found != m_Favorites.end())
            m_Favorites.erase(found);
        else if (relative != ".")
            m_Favorites.push_back(relative);
    }

    void ContentBrowserPanel::Report(const AssetError& error)
    {
        if (m_ReportError)
            m_ReportError(error.Message);
    }

    int ContentBrowserPanel::ScrollTarget() const
    {
        if (m_ScrollTo.empty())
            return -1;
        for (size_t i = 0; i < m_Visible.size(); ++i)
            if (m_Visible[i].Path == m_ScrollTo)
                return int(i);
        return -1;
    }

    void ContentBrowserPanel::ScrolledTo()
    {
        ImGui::SetScrollHereY(0.5f);
        m_ScrollTo.clear();
    }

    void ContentBrowserPanel::Reveal(const fs::path& file)
    {
        const auto target = Normal(file);
        if (!Browser::IsWithin(target, m_Root))
            return;
        Navigate(target.parent_path());
        m_Search.fill(0); // A search could hide the file.
        Refresh();
        m_Selection.Set(target);
        m_ScrollTo       = target;
        m_FocusRequested = true;
    }

    void ContentBrowserPanel::SelectAfterChange(const std::vector<fs::path>& paths)
    {
        Refresh();
        m_Selection.SetAll(paths);
    }
} // namespace Kans
