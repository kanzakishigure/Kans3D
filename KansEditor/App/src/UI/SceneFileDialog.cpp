#include "SceneFileDialog.h"
#include <algorithm>
#include <chrono>
#include <cstring>
#include <imgui.h>
#include "EditorIcons.h"
#include "Kans3D/Core/FileSystem/FileSystem.h"

namespace Kans
{
    namespace
    {
        constexpr const char* SceneExtension = ".kans";

        std::string Lower(std::string text)
        {
            std::transform(text.begin(), text.end(), text.begin(), [](unsigned char c) { return char(std::tolower(c)); });
            return text;
        }

        bool Inside(const std::filesystem::path& path, const std::filesystem::path& root)
        {
            const auto relative = path.lexically_relative(root);
            return !relative.empty() && *relative.begin() != "..";
        }

        std::time_t ToTime(std::filesystem::file_time_type time)
        {
            // C++17 has no clock_cast; shift by the offset between the clocks.
            const auto system = std::chrono::time_point_cast<std::chrono::system_clock::duration>(
                time - std::filesystem::file_time_type::clock::now() + std::chrono::system_clock::now());
            return std::chrono::system_clock::to_time_t(system);
        }

        std::string FormatTime(std::time_t time)
        {
            if (!time)
                return {};
            char text[32];
            std::strftime(text, sizeof(text), "%Y-%m-%d %H:%M", std::localtime(&time));
            return text;
        }

        std::string FormatSize(std::uintmax_t size)
        {
            char text[32];
            if (size < 1024)
                std::snprintf(text, sizeof(text), "%ju B", size);
            else if (size < 1024 * 1024)
                std::snprintf(text, sizeof(text), "%.1f KB", double(size) / 1024.0);
            else
                std::snprintf(text, sizeof(text), "%.1f MB", double(size) / (1024.0 * 1024.0));
            return text;
        }

        template<size_t N>
        void Copy(std::array<char, N>& buffer, const std::string& text)
        {
            buffer.fill(0);
            std::memcpy(buffer.data(), text.data(), std::min(text.size(), N - 1));
        }
    } // namespace

    void SceneFileDialog::Open(Mode mode, const std::filesystem::path& root, const std::filesystem::path& current)
    {
        std::error_code error;
        m_Mode          = mode;
        m_Root          = std::filesystem::weakly_canonical(root, error).lexically_normal();
        m_OpenRequested = true;
        m_Error.clear();
        m_Replace.clear();
        m_NewFolder = false;
        m_Close     = false;
        m_Search.fill(0);

        auto start = current.empty() ? m_Root : current;
        if (start.is_relative())
            start = m_Root / start;
        start = start.lexically_normal();
        std::string name;
        if (start.has_extension())
        {
            name  = start.filename().u8string();
            start = start.parent_path();
        }
        // The nearest existing folder, e.g. when the suggested folder is not created yet.
        while (Inside(start, m_Root) && !std::filesystem::is_directory(start, error))
            start = start.parent_path();
        Navigate(start);
        if (name.empty() && mode == Mode::Save)
            name = std::string("UntitledScene") + SceneExtension;
        SetName(name);
        m_NameSelection = int(std::filesystem::u8path(name).stem().u8string().size());
    }

    void SceneFileDialog::Navigate(const std::filesystem::path& directory)
    {
        const auto normal = directory.lexically_normal();
        m_Directory       = normal == m_Root || Inside(normal, m_Root) ? normal : m_Root;
        m_Search.fill(0);
        m_NewFolder = false;
        Refresh();
    }

    void SceneFileDialog::Refresh()
    {
        m_Entries.clear();
        std::error_code error;
        for (std::filesystem::directory_iterator it(m_Directory, error), end; !error && it != end; it.increment(error))
        {
            Entry entry;
            entry.Path      = it->path();
            entry.Name      = entry.Path.filename().u8string();
            entry.Directory = it->is_directory(error);
            if (entry.Name.empty() || entry.Name[0] == '.')
                continue;
            if (!entry.Directory && Lower(entry.Path.extension().u8string()) != SceneExtension)
                continue;
            if (!entry.Directory)
                entry.Size = it->file_size(error);
            if (const auto time = it->last_write_time(error); !error)
                entry.Modified = ToTime(time);
            error.clear();
            m_Entries.push_back(std::move(entry));
        }
        if (error)
            m_Error = "Cannot read " + m_Directory.u8string() + ": " + error.message();
        m_Unsorted = true;
    }

    void SceneFileDialog::SetName(const std::string& name) { Copy(m_Name, name); }

    // ---------------------------------------------------------------------- drawing

    void SceneFileDialog::Draw(const Submit& submit)
    {
        const char* title = m_Mode == Mode::Open ? ICON_FA_FOLDER_OPEN "  Open Scene###SceneFileDialog" :
                                                   ICON_FA_FLOPPY_DISK "  Save Scene###SceneFileDialog";
        if (m_OpenRequested)
        {
            ImGui::OpenPopup("###SceneFileDialog");
            m_OpenRequested = false;
        }
        const ImGuiViewport* viewport = ImGui::GetMainViewport();
        ImGui::SetNextWindowPos(viewport->GetCenter(), ImGuiCond_Appearing, {0.5f, 0.5f});
        ImGui::SetNextWindowSize({820.0f, 500.0f}, ImGuiCond_FirstUseEver);
        ImGui::SetNextWindowSizeConstraints({520.0f, 320.0f}, {FLT_MAX, FLT_MAX});
        ImGui::PushStyleVar(ImGuiStyleVar_WindowPadding, {10.0f, 10.0f});
        const bool open = ImGui::BeginPopupModal(title, nullptr, ImGuiWindowFlags_NoScrollbar);
        ImGui::PopStyleVar();
        m_Showing = open;
        if (!open)
            return;

        DrawPathBar();
        const float footer = ImGui::GetFrameHeightWithSpacing() * 2.0f + ImGui::GetStyle().ItemSpacing.y +
                             (m_Error.empty() ? 0.0f : ImGui::GetTextLineHeightWithSpacing());
        ImGui::BeginChild("##Places", {160.0f, -footer}, ImGuiChildFlags_Borders);
        DrawPlaces();
        ImGui::EndChild();
        ImGui::SameLine();
        ImGui::BeginChild("##Entries", {0.0f, -footer}, ImGuiChildFlags_Borders);
        DrawEntries(submit);
        ImGui::EndChild();
        DrawFooter(submit);

        // Save over an existing scene only after asking.
        if (!m_Replace.empty())
            ImGui::OpenPopup("Replace scene?");
        ImGui::PushStyleVar(ImGuiStyleVar_WindowPadding, {12.0f, 12.0f});
        const bool replacing = ImGui::BeginPopupModal("Replace scene?", nullptr, ImGuiWindowFlags_AlwaysAutoResize);
        ImGui::PopStyleVar();
        if (replacing)
        {
            ImGui::Text("%s already exists.", m_Replace.filename().u8string().c_str());
            ImGui::TextDisabled("Replace it with the current scene?");
            ImGui::Spacing();
            if (ImGui::Button("Replace", {90.0f, 0.0f}))
            {
                const auto file = m_Replace;
                m_Replace.clear();
                ImGui::CloseCurrentPopup();
                Finish(submit, file);
            }
            ImGui::SameLine();
            if (ImGui::Button("Cancel", {90.0f, 0.0f}) || ImGui::IsKeyPressed(ImGuiKey_Escape))
            {
                m_Replace.clear();
                ImGui::CloseCurrentPopup();
            }
            ImGui::EndPopup();
        }
        else if (ImGui::IsKeyPressed(ImGuiKey_Escape) && !ImGui::IsAnyItemActive())
            m_Close = true;
        if (m_Close)
        {
            m_Close = false;
            ImGui::CloseCurrentPopup();
        }
        ImGui::EndPopup();
    }

    void SceneFileDialog::DrawPathBar()
    {
        const bool atRoot = m_Directory == m_Root;
        ImGui::BeginDisabled(atRoot);
        if (ImGui::Button(ICON_FA_ARROW_UP))
            Navigate(m_Directory.parent_path());
        ImGui::EndDisabled();
        if (ImGui::IsItemHovered(ImGuiHoveredFlags_AllowWhenDisabled))
            ImGui::SetTooltip("Parent folder (Backspace)");
        if (!atRoot && ImGui::IsKeyPressed(ImGuiKey_Backspace) && !ImGui::IsAnyItemActive())
            Navigate(m_Directory.parent_path());

        // Breadcrumbs from the project folder.
        ImGui::PushStyleColor(ImGuiCol_Button, ImVec4(0, 0, 0, 0));
        ImGui::PushStyleVar(ImGuiStyleVar_ItemSpacing, {2.0f, ImGui::GetStyle().ItemSpacing.y});
        std::filesystem::path crumb = m_Root;
        ImGui::SameLine();
        if (ImGui::Button((ICON_FA_HOUSE "  " + m_Root.filename().u8string()).c_str()))
            Navigate(m_Root);
        std::filesystem::path target;
        int                   index = 0;
        for (const auto& part : m_Directory.lexically_relative(m_Root))
        {
            if (part == ".")
                continue;
            crumb /= part;
            ImGui::SameLine();
            ImGui::TextDisabled("/");
            ImGui::SameLine();
            ImGui::PushID(index++);
            if (ImGui::Button(part.u8string().c_str()))
                target = crumb;
            ImGui::PopID();
        }
        ImGui::PopStyleVar();
        ImGui::PopStyleColor();
        if (!target.empty())
            Navigate(target);

        // Filter on the right.
        const float width = 220.0f;
        ImGui::SameLine(
            std::max(ImGui::GetCursorPosX(), (ImGui::GetCursorPosX() + ImGui::GetContentRegionAvail().x) - width));
        ImGui::SetNextItemWidth(width);
        ImGui::InputTextWithHint("##Search", ICON_FA_MAGNIFYING_GLASS "  Filter", m_Search.data(), m_Search.size());
    }

    void SceneFileDialog::DrawPlaces()
    {
        ImGui::TextDisabled("Places");
        std::error_code error;
        const auto place = [&](const char* icon, const std::string& label, const std::filesystem::path& path) {
            if (!std::filesystem::is_directory(path, error))
                return;
            if (ImGui::Selectable((std::string(icon) + "  " + label).c_str(), m_Directory == path))
                Navigate(path);
        };
        place(ICON_FA_HOUSE, "Project", m_Root);
        const auto& assets = KansFileSystem::GetAssetFolder();
        if (!assets.empty() && Inside(assets, m_Root))
        {
            place(ICON_FA_FOLDER, "Assets", assets);
            place(ICON_FA_FOLDER, "Scenes", assets / "scenes");
        }
    }

    void SceneFileDialog::DrawEntries(const Submit& submit)
    {
        const ImGuiTableFlags flags = ImGuiTableFlags_Sortable | ImGuiTableFlags_RowBg | ImGuiTableFlags_ScrollY |
                                      ImGuiTableFlags_Resizable | ImGuiTableFlags_BordersInnerV;
        if (!ImGui::BeginTable("##Files", 3, flags))
            return;
        ImGui::TableSetupScrollFreeze(0, 1);
        ImGui::TableSetupColumn("Name", ImGuiTableColumnFlags_WidthStretch | ImGuiTableColumnFlags_DefaultSort);
        ImGui::TableSetupColumn("Modified", ImGuiTableColumnFlags_WidthFixed, 130.0f);
        ImGui::TableSetupColumn("Size", ImGuiTableColumnFlags_WidthFixed, 80.0f);
        ImGui::TableHeadersRow();

        if (ImGuiTableSortSpecs* specs = ImGui::TableGetSortSpecs(); specs && (specs->SpecsDirty || m_Unsorted))
        {
            // Folders stay first.
            const auto column    = specs->SpecsCount ? specs->Specs[0].ColumnIndex : 0;
            const bool ascending = !specs->SpecsCount || specs->Specs[0].SortDirection == ImGuiSortDirection_Ascending;
            std::stable_sort(m_Entries.begin(), m_Entries.end(), [&](const Entry& a, const Entry& b) {
                if (a.Directory != b.Directory)
                    return a.Directory;
                const int order = column == 1 ? (a.Modified < b.Modified ? -1 : a.Modified > b.Modified) :
                                  column == 2 ? (a.Size < b.Size ? -1 : a.Size > b.Size) :
                                                Lower(a.Name).compare(Lower(b.Name));
                return ascending ? order < 0 : order > 0;
            });
            specs->SpecsDirty = false;
            m_Unsorted        = false;
        }

        const std::string   filter = Lower(m_Search.data());
        const std::string   chosen = m_Name.data();
        std::optional<Entry> enter, open;
        bool                 any = false;
        for (const Entry& entry : m_Entries)
        {
            if (!filter.empty() && Lower(entry.Name).find(filter) == std::string::npos)
                continue;
            any = true;
            ImGui::TableNextRow();
            ImGui::TableNextColumn();
            const std::string label = std::string(entry.Directory ? ICON_FA_FOLDER : ICON_FA_FILE) + "  " + entry.Name;
            ImGui::PushStyleColor(ImGuiCol_Text,
                                  entry.Directory ? ImVec4(0.95f, 0.78f, 0.42f, 1.0f) : ImGui::GetStyleColorVec4(ImGuiCol_Text));
            if (ImGui::Selectable(label.c_str(),
                                  !entry.Directory && entry.Name == chosen,
                                  ImGuiSelectableFlags_SpanAllColumns | ImGuiSelectableFlags_AllowDoubleClick))
            {
                if (entry.Directory)
                {
                    if (ImGui::IsMouseDoubleClicked(ImGuiMouseButton_Left))
                        enter = entry;
                }
                else
                {
                    SetName(entry.Name);
                    if (ImGui::IsMouseDoubleClicked(ImGuiMouseButton_Left))
                        open = entry;
                }
            }
            ImGui::PopStyleColor();
            ImGui::TableNextColumn();
            ImGui::TextDisabled("%s", FormatTime(entry.Modified).c_str());
            ImGui::TableNextColumn();
            if (!entry.Directory)
                ImGui::TextDisabled("%s", FormatSize(entry.Size).c_str());
        }
        ImGui::EndTable();
        if (!any)
            ImGui::TextDisabled(filter.empty() ? "No scenes or folders here." : "Nothing matches the filter.");
        // Acted on after the table, which reads m_Entries.
        if (enter)
            Navigate(enter->Path);
        else if (open)
            Confirm(submit);
    }

    void SceneFileDialog::DrawFooter(const Submit& submit)
    {
        if (!m_Error.empty())
            ImGui::TextColored({0.95f, 0.38f, 0.35f, 1.0f}, "%s", m_Error.c_str());

        // New folder (saving only).
        if (m_Mode == Mode::Save)
        {
            if (m_NewFolder)
            {
                ImGui::SetNextItemWidth(220.0f);
                if (m_FocusFolder)
                {
                    ImGui::SetKeyboardFocusHere();
                    m_FocusFolder = false;
                }
                const bool create = ImGui::InputTextWithHint("##Folder",
                                                             "New folder name",
                                                             m_FolderName.data(),
                                                             m_FolderName.size(),
                                                             ImGuiInputTextFlags_EnterReturnsTrue);
                ImGui::SameLine();
                if (ImGui::Button(ICON_FA_CHECK) || create)
                {
                    const std::string name = m_FolderName.data();
                    std::error_code   error;
                    if (name.empty() || name.find_first_of("/\\") != std::string::npos || name == "." || name == "..")
                        m_Error = "Enter a folder name without slashes";
                    else if (std::filesystem::exists(m_Directory / std::filesystem::u8path(name), error))
                        m_Error = name + " already exists";
                    else if (!std::filesystem::create_directory(m_Directory / std::filesystem::u8path(name), error))
                        m_Error = "Cannot create the folder: " + error.message();
                    else
                    {
                        m_Error.clear();
                        Navigate(m_Directory / std::filesystem::u8path(name));
                    }
                }
                ImGui::SameLine();
                if (ImGui::Button(ICON_FA_XMARK))
                    m_NewFolder = false;
            }
            else if (ImGui::Button(ICON_FA_FOLDER_PLUS "  New folder"))
            {
                m_NewFolder   = true;
                m_FocusFolder = true;
                m_FolderName.fill(0);
            }
        }
        else
            ImGui::TextDisabled("Showing folders and %s scenes", SceneExtension);

        // File name, then the buttons on the right.
        const float buttons = 2.0f * 90.0f + ImGui::GetStyle().ItemSpacing.x;
        ImGui::AlignTextToFramePadding();
        ImGui::TextUnformatted("File name");
        ImGui::SameLine();
        ImGui::SetNextItemWidth(ImGui::GetContentRegionAvail().x - buttons - ImGui::GetStyle().ItemSpacing.x);
        if (ImGui::IsWindowAppearing())
            ImGui::SetKeyboardFocusHere();
        // Activation selects everything; the callback narrows it to the name
        // without its extension, once.
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
        const bool entered = ImGui::InputText("##Name",
                                              m_Name.data(),
                                              m_Name.size(),
                                              ImGuiInputTextFlags_EnterReturnsTrue | ImGuiInputTextFlags_CallbackAlways,
                                              select,
                                              &m_NameSelection);
        ImGui::SameLine();
        ImGui::BeginDisabled(m_Name[0] == 0);
        const bool confirmed = ImGui::Button(m_Mode == Mode::Open ? "Open" : "Save", {90.0f, 0.0f});
        ImGui::EndDisabled();
        ImGui::SameLine();
        if (ImGui::Button("Cancel", {90.0f, 0.0f}))
            ImGui::CloseCurrentPopup();
        if (entered || confirmed)
            Confirm(submit);
    }

    void SceneFileDialog::Confirm(const Submit& submit)
    {
        std::string name = m_Name.data();
        if (name.empty())
            return;
        auto path = std::filesystem::u8path(name);
        path      = (path.is_absolute() ? path : m_Directory / path).lexically_normal();
        std::error_code error;
        if (std::filesystem::is_directory(path, error))
        {
            // A typed folder name opens the folder.
            if (path == m_Root || Inside(path, m_Root))
            {
                Navigate(path);
                SetName({});
            }
            else
                m_Error = "Choose a folder inside the project";
            return;
        }
        if (!(path.parent_path() == m_Root || Inside(path.parent_path(), m_Root)))
        {
            m_Error = "Choose a file inside the project";
            return;
        }
        if (m_Mode == Mode::Save)
        {
            if (Lower(path.extension().u8string()) != SceneExtension)
                path += SceneExtension;
            if (!std::filesystem::is_directory(path.parent_path(), error))
            {
                m_Error = "The folder " + path.parent_path().u8string() + " does not exist";
                return;
            }
            if (std::filesystem::exists(path, error))
            {
                m_Replace = path;
                return;
            }
        }
        else if (!std::filesystem::is_regular_file(path, error))
        {
            m_Error = path.filename().u8string() + " does not exist";
            return;
        }
        Finish(submit, path);
    }

    void SceneFileDialog::Finish(const Submit& submit, const std::filesystem::path& file)
    {
        if (auto failure = submit(file))
        {
            m_Error = *failure;
            Refresh();
            return;
        }
        m_Error.clear();
        m_Close = true;
    }
} // namespace Kans
