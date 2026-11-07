#include "FolderBrowser.h"
#include "Kans3D/ImGui/EditorTheme.h"
#include <algorithm>
#include <cctype>
#include <imgui.h>
#include <misc/cpp/imgui_stdlib.h>
#include "EditorIcons.h"
#include "Kans3D/Project/ProjectFile.h"
#include "KansEditorCore/Launcher/LauncherModel.h"
#if defined(_WIN32)
#include <windows.h>
#endif

namespace Kans
{
    namespace fs = std::filesystem;
    namespace
    {
        bool LessIgnoreCase(const std::string& a, const std::string& b)
        {
            return std::lexicographical_compare(a.begin(), a.end(), b.begin(), b.end(), [](unsigned char x, unsigned char y) {
                return std::tolower(x) < std::tolower(y);
            });
        }
    } // namespace

    void FolderBrowser::Open(Mode mode, const fs::path& start, const char* title)
    {
        m_Mode          = mode;
        m_Title         = title;
        m_OpenRequested = true;
        // Start at the nearest folder that still exists.
        auto            directory = start.empty() ? Launcher::HomeDirectory() : fs::absolute(start);
        std::error_code ec;
        while (!fs::is_directory(directory, ec) && directory.has_parent_path() && directory != directory.parent_path())
            directory = directory.parent_path();
        Navigate(directory);
    }

    void FolderBrowser::Navigate(const fs::path& directory)
    {
        m_Entries.clear();
        m_Selected.clear();
        m_Error.clear();
        std::error_code ec;
        for (fs::directory_iterator it(directory, fs::directory_options::skip_permission_denied, ec), end;
             !ec && it != end;
             it.increment(ec))
        {
            std::error_code entryError;
            auto            name = it->path().filename().u8string();
            // Hidden folders are noise when looking for projects.
            if (name.empty() || name.front() == '.')
                continue;
            if (it->is_directory(entryError))
            {
                // A folder holding a project opens as one; it is not just somewhere to look inside.
                const bool project = m_Mode == Mode::Project && std::holds_alternative<fs::path>(FindProjectFile(it->path()));
                m_Entries.push_back({name, it->path(), project});
            }
            else if (m_Mode == Mode::Project && it->path().extension() == ".kproj")
                m_Entries.push_back({name, it->path(), true});
        }
        if (ec)
        {
            m_Error = "Cannot read this folder: " + ec.message();
            if (!m_Directory.empty())
                return; // Stay where we were.
        }
        std::sort(m_Entries.begin(), m_Entries.end(), [](const Entry& a, const Entry& b) {
            if (a.Project != b.Project)
                return a.Project;
            return LessIgnoreCase(a.Name, b.Name);
        });
        m_Directory = directory.lexically_normal();
        m_PathText  = m_Directory.u8string();
    }

    std::optional<fs::path> FolderBrowser::Draw()
    {
        if (m_OpenRequested)
        {
            ImGui::OpenPopup(m_Title.c_str());
            m_OpenRequested = false;
        }
        const auto* viewport = ImGui::GetMainViewport();
        ImGui::SetNextWindowSize({std::min(720.0f, viewport->WorkSize.x - 40.0f), std::min(480.0f, viewport->WorkSize.y - 40.0f)},
                                 ImGuiCond_Appearing);
        ImGui::SetNextWindowPos(viewport->GetCenter(), ImGuiCond_Appearing, {0.5f, 0.5f});
        if (!ImGui::BeginPopupModal(m_Title.c_str(), nullptr, ImGuiWindowFlags_NoSavedSettings))
            return std::nullopt;

        std::optional<fs::path> chosen;
        std::optional<fs::path> navigate;
        // Path bar: up, home, drives, then an editable path.
        if (ImGui::Button(ICON_FA_ARROW_UP) && m_Directory.has_parent_path() && m_Directory != m_Directory.parent_path())
            navigate = m_Directory.parent_path();
        if (ImGui::IsItemHovered())
            ImGui::SetTooltip("Parent folder");
        ImGui::SameLine();
        if (ImGui::Button(ICON_FA_HOUSE))
            navigate = Launcher::HomeDirectory();
        if (ImGui::IsItemHovered())
            ImGui::SetTooltip("Home");
#if defined(_WIN32)
        ImGui::SameLine();
        ImGui::SetNextItemWidth(ImGui::GetFrameHeight() * 2.2f);
        const auto drive = m_Directory.root_name().u8string();
        if (ImGui::BeginCombo("##Drive", drive.c_str()))
        {
            const DWORD drives = GetLogicalDrives();
            for (int i = 0; i < 26; ++i)
                if (drives & (1u << i))
                {
                    const std::string letter = std::string(1, char('A' + i)) + ":";
                    if (ImGui::Selectable((ICON_FA_HARD_DRIVE " " + letter).c_str(), letter == drive))
                        navigate = fs::path(letter + "\\");
                }
            ImGui::EndCombo();
        }
#endif
        ImGui::SameLine();
        ImGui::SetNextItemWidth(-FLT_MIN);
        bool pathSubmitted = false;
        if (ImGui::InputText("##Path", &m_PathText, ImGuiInputTextFlags_EnterReturnsTrue))
        {
            pathSubmitted = true; // This Enter belongs to the path field, not the main button.
            std::error_code ec;
            const auto      typed = fs::u8path(m_PathText);
            if (fs::is_directory(typed, ec))
                navigate = typed;
            else if (m_Mode == Mode::Project && typed.extension() == ".kproj" && fs::is_regular_file(typed, ec))
                chosen = typed;
            else
                m_Error = "Folder not found.";
        }

        const float footer = ImGui::GetFrameHeightWithSpacing() * 2.0f;
        if (ImGui::BeginChild("##Folders", {0, -footer}, ImGuiChildFlags_Borders))
        {
            if (m_Entries.empty())
                ImGui::TextDisabled(m_Mode == Mode::Project ? "No folders or projects here." : "No folders here.");
            for (const auto& entry : m_Entries)
            {
                ImGui::PushID(entry.Name.c_str());
                const auto label = std::string(entry.Project ? ICON_FA_CUBE "  " : ICON_FA_FOLDER "  ") + entry.Name;
                if (entry.Project)
                    ImGui::PushStyleColor(ImGuiCol_Text, EditorTheme::Colors().AccentHover);
                if (ImGui::Selectable(label.c_str(), m_Selected == entry.Path, ImGuiSelectableFlags_AllowDoubleClick))
                {
                    m_Selected = entry.Path;
                    if (ImGui::IsMouseDoubleClicked(ImGuiMouseButton_Left))
                    {
                        if (entry.Project)
                            chosen = entry.Path; // A .kproj or the folder holding one.
                        else
                            navigate = entry.Path;
                    }
                }
                if (entry.Project)
                    ImGui::PopStyleColor();
                ImGui::PopID();
            }
        }
        ImGui::EndChild();

        if (!m_Error.empty())
            ImGui::TextColored(EditorTheme::Colors().Danger, ICON_FA_TRIANGLE_EXCLAMATION " %s", m_Error.c_str());
        else
            ImGui::TextDisabled("%s", (m_Selected.empty() ? m_Directory : m_Selected).u8string().c_str());

        // Right-aligned actions; the main one acts on the selection, else the current folder.
        const char* accept      = m_Mode == Mode::Project ? "Open" : "Select Folder";
        const float acceptWidth = ImGui::CalcTextSize(accept).x + ImGui::GetStyle().FramePadding.x * 2.0f;
        const float cancelWidth = ImGui::CalcTextSize("Cancel").x + ImGui::GetStyle().FramePadding.x * 2.0f;
        ImGui::SetCursorPosX(ImGui::GetCursorPosX() + ImGui::GetContentRegionAvail().x - acceptWidth - cancelWidth -
                             ImGui::GetStyle().ItemSpacing.x);
        if (ImGui::Button(accept) || (!pathSubmitted && !ImGui::IsAnyItemActive() && ImGui::IsKeyPressed(ImGuiKey_Enter, false)))
            chosen = m_Selected.empty() ? m_Directory : m_Selected;
        ImGui::SameLine();
        if (ImGui::Button("Cancel") || ImGui::IsKeyPressed(ImGuiKey_Escape, false))
            ImGui::CloseCurrentPopup();

        if (navigate)
            Navigate(*navigate);
        if (chosen)
            ImGui::CloseCurrentPopup();
        ImGui::EndPopup();
        return chosen;
    }
} // namespace Kans
