#include "ProjectBrowser.h"
#include "Kans3D/ImGui/EditorTheme.h"
#include <algorithm>
#include <cctype>
#include <ctime>
#include <imgui.h>
#include <misc/cpp/imgui_stdlib.h>
#include "Kans3D/Project/ProjectFile.h"
#include "UI/EditorIcons.h"

namespace Kans
{
    namespace fs = std::filesystem;
    namespace
    {
        const ImVec4& Accent    = EditorTheme::Colors().Accent;
        const ImVec4& ErrorText = EditorTheme::Colors().Danger;

        ImFont* BoldFont()
        {
            auto& fonts = ImGui::GetIO().Fonts->Fonts;
            return fonts.Size > 1 ? fonts[1] : ImGui::GetFont();
        }

        void Heading(const char* text, float size = 20.0f)
        {
            ImGui::PushFont(BoldFont(), size);
            ImGui::TextUnformatted(text);
            ImGui::PopFont();
        }

        // Accent-filled main action button.
        bool PrimaryButton(const char* label, bool enabled = true)
        {
            ImGui::BeginDisabled(!enabled);
            ImGui::PushStyleColor(ImGuiCol_Button, Accent);
            ImGui::PushStyleColor(ImGuiCol_ButtonHovered, EditorTheme::Colors().AccentHover);
            ImGui::PushStyleColor(ImGuiCol_ButtonActive, EditorTheme::Colors().AccentPressed);
            const bool pressed = ImGui::Button(label);
            ImGui::PopStyleColor(3);
            ImGui::EndDisabled();
            return pressed;
        }

        float ButtonWidth(const char* label)
        {
            return ImGui::CalcTextSize(label, nullptr, true).x + ImGui::GetStyle().FramePadding.x * 2.0f;
        }

        bool ContainsIgnoreCase(const std::string& text, const std::string& needle)
        {
            return std::search(text.begin(), text.end(), needle.begin(), needle.end(), [](unsigned char a, unsigned char b) {
                       return std::tolower(a) == std::tolower(b);
                   }) != text.end();
        }

        const char* TemplateDescription(const std::string& name)
        {
            if (name == "Blank")
                return "An empty scene with a camera and lights.";
            if (name == "Showcase")
                return "Sample scene with PBR and toon materials and HDR skies.";
            return "";
        }
    } // namespace

    ProjectBrowser::ProjectBrowser(fs::path templatesDir)
        : m_TemplatesDir(std::move(templatesDir)), m_Recent(RecentProjects::DefaultStorageFile())
    {
        Refresh();
    }

    void ProjectBrowser::Refresh()
    {
        if (auto error = m_Recent.Load())
            m_Error = error->Message;
        m_Templates      = Launcher::ListTemplates(m_TemplatesDir);
        m_SelectedRecent = m_Recent.Entries().empty() ? -1 : 0;
        m_Result.reset();
        ResetNewProject();
        // Start where there is something to do.
        m_Page = m_Recent.Entries().empty() ? Page::New : Page::Recent;
    }

    void ProjectBrowser::SetPage(Page page)
    {
        m_Page = page;
        m_Error.clear();
    }

    void ProjectBrowser::ResetNewProject()
    {
        std::vector<fs::path> recent;
        for (const auto& entry : m_Recent.Entries())
            recent.push_back(entry.ProjectFile);
        m_Location         = Launcher::DefaultLocation(recent, Launcher::HomeDirectory()).u8string();
        m_Name             = Launcher::UniqueProjectName(fs::u8path(m_Location), "NewProject");
        m_SelectedTemplate = 0;
    }

    void ProjectBrowser::BrowseForProject()
    {
        const auto& entries = m_Recent.Entries();
        m_Browser.Open(FolderBrowser::Mode::Project,
                       entries.empty() ? Launcher::HomeDirectory() : entries.front().ProjectFile.parent_path().parent_path(),
                       "Open Project");
        m_BrowseTarget = BrowseTarget::Project;
    }

    void ProjectBrowser::Request(const fs::path& path)
    {
        m_Error.clear();
        auto selection = Launcher::ResolveSelection(path);
        if (auto* file = std::get_if<fs::path>(&selection))
            m_Result = *file;
        else if (auto* missing = std::get_if<Launcher::MissingProjectFile>(&selection))
        {
            m_MissingProjectFolder = missing->Folder;
            m_AskCreateProjectFile = true;
        }
        else
            m_Error = std::get<AssetError>(selection).Message;
    }

    std::optional<fs::path> ProjectBrowser::Draw()
    {
        const float sidebar = 210.0f;
        DrawSidebar(sidebar);
        ImGui::SameLine(0, 0);
        ImGui::PushStyleVar(ImGuiStyleVar_WindowPadding, {24.0f, 20.0f});
        if (ImGui::BeginChild("##ProjectBrowserMain", {0, 0}, ImGuiChildFlags_AlwaysUseWindowPadding))
        {
            if (m_Page == Page::Recent)
                DrawRecent();
            else
                DrawNew();
        }
        ImGui::EndChild();
        ImGui::PopStyleVar();
        DrawDialogs();
        auto result = m_Result;
        m_Result.reset();
        return result;
    }

    void ProjectBrowser::DrawSidebar(float width)
    {
        ImGui::PushStyleColor(ImGuiCol_ChildBg, EditorTheme::Colors().App);
        ImGui::PushStyleVar(ImGuiStyleVar_WindowPadding, {12.0f, 20.0f});
        ImGui::BeginChild("##ProjectBrowserSidebar", {width, 0}, ImGuiChildFlags_AlwaysUseWindowPadding);
        ImGui::Indent(4.0f);
        Heading("Kans3D", 24.0f);
        ImGui::TextDisabled("Projects");
        ImGui::Unindent(4.0f);
        ImGui::Dummy({0, 12.0f});

        const auto item = [&](const char* label, Page page) {
            const bool selected = m_Page == page;
            ImGui::PushStyleVar(ImGuiStyleVar_SelectableTextAlign, {0.0f, 0.5f});
            if (ImGui::Selectable(label, selected, 0, {0, ImGui::GetFrameHeight() + 6.0f}))
                SetPage(page);
            ImGui::PopStyleVar();
            if (selected) // Accent bar marks the active page.
            {
                auto* draw = ImGui::GetWindowDrawList();
                draw->AddRectFilled(ImGui::GetItemRectMin(),
                                    {ImGui::GetItemRectMin().x + 3.0f, ImGui::GetItemRectMax().y},
                                    ImGui::GetColorU32(Accent));
            }
        };
        item(ICON_FA_CLOCK_ROTATE_LEFT "   Recent Projects", Page::Recent);
        item(ICON_FA_PLUS "   New Project", Page::New);

        // Open sits at the bottom, apart from the pages.
        ImGui::SetCursorPosY(std::max(ImGui::GetCursorPosY(),
                                      ImGui::GetWindowHeight() - ImGui::GetFrameHeight() - ImGui::GetStyle().WindowPadding.y));
        if (ImGui::Button(ICON_FA_FOLDER_OPEN "  Open Project...", {-FLT_MIN, 0}))
            BrowseForProject();
        ImGui::EndChild();
        ImGui::PopStyleVar();
        ImGui::PopStyleColor();
    }

    void ProjectBrowser::DrawRecent()
    {
        Heading("Recent Projects");
        const auto& entries = m_Recent.Entries();
        if (!entries.empty())
        {
            ImGui::SameLine();
            const float searchWidth = std::min(260.0f, ImGui::GetContentRegionAvail().x * 0.5f);
            ImGui::SetCursorPosX(ImGui::GetCursorPosX() + ImGui::GetContentRegionAvail().x - searchWidth);
            ImGui::SetNextItemWidth(searchWidth);
            ImGui::InputTextWithHint("##Search", ICON_FA_MAGNIFYING_GLASS "  Search", &m_Search);
        }
        ImGui::Dummy({0, 6.0f});

        if (entries.empty())
        {
            ImGui::Dummy({0, 40.0f});
            ImGui::TextDisabled("No recent projects yet.");
            ImGui::Dummy({0, 8.0f});
            if (PrimaryButton(ICON_FA_PLUS "  New Project"))
                SetPage(Page::New);
            ImGui::SameLine();
            if (ImGui::Button(ICON_FA_FOLDER_OPEN "  Open Project..."))
                BrowseForProject();
            return;
        }

        const float footer   = ImGui::GetFrameHeightWithSpacing() * 2.0f + 8.0f;
        const auto  now      = static_cast<int64_t>(std::time(nullptr));
        int         remove   = -1;
        const float line     = ImGui::GetTextLineHeight();
        const float rowHeight = line * 2.0f + 18.0f;
        ImGui::PushStyleColor(ImGuiCol_ChildBg, EditorTheme::Colors().Raised);
        if (ImGui::BeginChild("##RecentList", {0, -footer}, ImGuiChildFlags_None))
        {
            int shown = 0;
            for (int i = 0; i < int(entries.size()); ++i)
            {
                const auto& entry = entries[i];
                const auto  path  = entry.ProjectFile.parent_path().u8string();
                if (!m_Search.empty() && !ContainsIgnoreCase(entry.Name, m_Search) && !ContainsIgnoreCase(path, m_Search))
                    continue;
                ++shown;
                const bool exists = RecentProjects::Exists(entry);
                ImGui::PushID(i);
                const auto top = ImGui::GetCursorScreenPos();
                if (ImGui::Selectable("##Row",
                                      m_SelectedRecent == i,
                                      ImGuiSelectableFlags_AllowDoubleClick | ImGuiSelectableFlags_AllowOverlap,
                                      {0, rowHeight}))
                {
                    m_SelectedRecent = i;
                    if (ImGui::IsMouseDoubleClicked(ImGuiMouseButton_Left))
                        Request(entry.ProjectFile);
                }
                const float right = ImGui::GetItemRectMax().x;
                auto*       draw  = ImGui::GetWindowDrawList();
                const auto  text  = ImGui::GetColorU32(exists ? ImGuiCol_Text : ImGuiCol_TextDisabled);
                const auto  dim   = ImGui::GetColorU32(ImGuiCol_TextDisabled);
                const float x     = top.x + 14.0f;
                const float y     = top.y + 9.0f;
                ImGui::PushFont(nullptr, 22.0f);
                draw->AddText({x, top.y + (rowHeight - ImGui::GetFontSize()) * 0.5f},
                              exists ? ImGui::GetColorU32(Accent) : dim,
                              ICON_FA_CUBE);
                ImGui::PopFont();
                const float textX = x + 36.0f;
                ImGui::PushFont(BoldFont(), 0.0f);
                draw->AddText({textX, y}, text, entry.Name.c_str());
                ImGui::PopFont();
                draw->AddText({textX, y + line + 2.0f}, dim, path.c_str());

                // Right column: when it was opened (or that it is gone) and a row menu.
                const float menuWidth = ImGui::GetFrameHeight();
                const auto  when      = exists ? Launcher::FormatLastOpened(now, entry.LastOpened) : std::string("Missing");
                const float whenWidth = ImGui::CalcTextSize(when.c_str()).x;
                draw->AddText({right - menuWidth - 16.0f - whenWidth, top.y + (rowHeight - line) * 0.5f},
                              exists ? dim : ImGui::GetColorU32(ErrorText),
                              when.c_str());
                ImGui::SetCursorScreenPos({right - menuWidth - 6.0f, top.y + (rowHeight - menuWidth) * 0.5f});
                ImGui::PushStyleColor(ImGuiCol_Button, {0, 0, 0, 0});
                if (ImGui::Button(ICON_FA_ELLIPSIS_VERTICAL, {menuWidth, menuWidth}))
                    ImGui::OpenPopup("##RowMenu");
                ImGui::PopStyleColor();
                if (ImGui::BeginPopup("##RowMenu"))
                {
                    if (ImGui::MenuItem(ICON_FA_FOLDER_OPEN "  Open", nullptr, false, exists))
                        Request(entry.ProjectFile);
                    if (ImGui::MenuItem(ICON_FA_XMARK "  Remove from List"))
                        remove = i;
                    ImGui::EndPopup();
                }
                // Continue below the row; the dummy is the item that grows the list.
                ImGui::SetCursorScreenPos({top.x, top.y + rowHeight});
                ImGui::Dummy({0, 2.0f});
                ImGui::PopID();
            }
            if (shown == 0)
                ImGui::TextDisabled("No projects match \"%s\".", m_Search.c_str());
        }
        ImGui::EndChild();
        ImGui::PopStyleColor();

        if (remove >= 0)
        {
            m_Recent.Remove(entries[remove].ProjectFile);
            if (auto error = m_Recent.Save())
                m_Error = error->Message;
            m_SelectedRecent = std::min(m_SelectedRecent, int(m_Recent.Entries().size()) - 1);
        }

        ImGui::Dummy({0, 4.0f});
        if (!m_Error.empty())
            ImGui::TextColored(ErrorText, ICON_FA_TRIANGLE_EXCLAMATION "  %s", m_Error.c_str());
        else
            ImGui::NewLine();
        const auto& current  = m_Recent.Entries();
        const bool  canOpen  = m_SelectedRecent >= 0 && m_SelectedRecent < int(current.size()) &&
                             RecentProjects::Exists(current[m_SelectedRecent]);
        const char* openLabel = ICON_FA_FOLDER_OPEN "  Open";
        ImGui::SetCursorPosX(ImGui::GetCursorPosX() + ImGui::GetContentRegionAvail().x - ButtonWidth(openLabel));
        if (PrimaryButton(openLabel, canOpen) ||
            (canOpen && !ImGui::IsAnyItemActive() && !ImGui::IsPopupOpen(nullptr, ImGuiPopupFlags_AnyPopupId) &&
             ImGui::IsKeyPressed(ImGuiKey_Enter, false)))
            Request(current[m_SelectedRecent].ProjectFile);
    }

    void ProjectBrowser::DrawNew()
    {
        Heading("New Project");
        ImGui::TextDisabled("Start from a template.");
        ImGui::Dummy({0, 8.0f});

        if (m_Templates.empty())
            ImGui::TextColored(ErrorText, ICON_FA_TRIANGLE_EXCLAMATION "  No templates found in %s",
                               m_TemplatesDir.u8string().c_str());
        // Template cards.
        const float cardWidth  = 220.0f;
        const float cardHeight = 128.0f;
        for (int i = 0; i < int(m_Templates.size()); ++i)
        {
            ImGui::PushID(i);
            if (i > 0)
                ImGui::SameLine(0, 12.0f);
            const bool selected = m_SelectedTemplate == i;
            const auto top      = ImGui::GetCursorScreenPos();
            if (ImGui::InvisibleButton("##Template", {cardWidth, cardHeight}))
                m_SelectedTemplate = i;
            auto*       draw    = ImGui::GetWindowDrawList();
            const bool  hovered = ImGui::IsItemHovered();
            const ImVec2 bottom {top.x + cardWidth, top.y + cardHeight};
            draw->AddRectFilled(top, bottom, ImGui::GetColorU32(hovered ? EditorTheme::Colors().Hover : EditorTheme::Colors().Raised), 6.0f);
            if (selected)
                draw->AddRect(top, bottom, ImGui::GetColorU32(Accent), 6.0f, 0, 2.0f);
            ImGui::PushFont(nullptr, 28.0f);
            draw->AddText({top.x + 14.0f, top.y + 14.0f},
                          ImGui::GetColorU32(selected ? Accent : ImGui::GetStyleColorVec4(ImGuiCol_TextDisabled)),
                          m_Templates[i].Name == "Blank" ? ICON_FA_FILE : ICON_FA_SHAPES);
            ImGui::PopFont();
            ImGui::PushFont(BoldFont(), 0.0f);
            draw->AddText({top.x + 14.0f, top.y + 56.0f}, ImGui::GetColorU32(ImGuiCol_Text), m_Templates[i].Name.c_str());
            ImGui::PopFont();
            draw->AddText(nullptr,
                          0.0f,
                          {top.x + 14.0f, top.y + 56.0f + ImGui::GetTextLineHeight() + 4.0f},
                          ImGui::GetColorU32(ImGuiCol_TextDisabled),
                          TemplateDescription(m_Templates[i].Name),
                          nullptr,
                          cardWidth - 28.0f);
            ImGui::PopID();
        }
        ImGui::Dummy({0, 16.0f});

        // Name and location, laid out as a two-column form.
        const float labelWidth = 110.0f;
        const float fieldWidth = std::min(560.0f, ImGui::GetContentRegionAvail().x - labelWidth);
        const auto  label      = [&](const char* text) {
            ImGui::AlignTextToFramePadding();
            ImGui::TextUnformatted(text);
            ImGui::SameLine(labelWidth);
        };
        label("Project name");
        ImGui::SetNextItemWidth(fieldWidth);
        ImGui::InputText("##Name", &m_Name);
        label("Location");
        const char* browse = ICON_FA_FOLDER "  Browse...";
        ImGui::SetNextItemWidth(fieldWidth - ButtonWidth(browse) - ImGui::GetStyle().ItemSpacing.x);
        ImGui::InputText("##Location", &m_Location);
        ImGui::SameLine();
        if (ImGui::Button(browse))
        {
            m_Browser.Open(FolderBrowser::Mode::Folder, fs::u8path(m_Location), "Choose Location");
            m_BrowseTarget = BrowseTarget::Location;
        }

        const auto location = fs::u8path(m_Location);
        const auto problem  = Launcher::NewProjectProblem(location, m_Name);
        ImGui::Dummy({0, 4.0f});
        ImGui::SetCursorPosX(labelWidth); // Window-relative, like SameLine(labelWidth) above.
        if (problem)
            ImGui::TextColored(ErrorText, ICON_FA_TRIANGLE_EXCLAMATION "  %s", problem->c_str());
        else
            ImGui::TextDisabled("Creates %s", (location / fs::u8path(m_Name) / fs::u8path(m_Name + ".kproj")).u8string().c_str());
        if (!m_Error.empty())
        {
            ImGui::SetCursorPosX(labelWidth);
            ImGui::TextColored(ErrorText, ICON_FA_TRIANGLE_EXCLAMATION "  %s", m_Error.c_str());
        }

        ImGui::Dummy({0, 12.0f});
        ImGui::SetCursorPosX(labelWidth);
        const bool canCreate = !problem && m_SelectedTemplate < int(m_Templates.size());
        if (PrimaryButton(ICON_FA_PLUS "  Create Project", canCreate))
        {
            m_Error.clear();
            std::error_code ec;
            fs::create_directories(location, ec);
            auto created = CreateProjectFromTemplate(m_Templates[m_SelectedTemplate].Directory, location, m_Name);
            if (auto* file = std::get_if<fs::path>(&created))
                m_Result = *file;
            else
                m_Error = std::get<AssetError>(created).Message;
        }
    }

    void ProjectBrowser::DrawDialogs()
    {
        if (auto chosen = m_Browser.Draw())
        {
            if (m_BrowseTarget == BrowseTarget::Project)
                Request(*chosen);
            else if (m_BrowseTarget == BrowseTarget::Location)
                m_Location = chosen->u8string();
            m_BrowseTarget = BrowseTarget::None;
        }

        if (m_AskCreateProjectFile)
        {
            ImGui::OpenPopup("Create Project File?");
            m_AskCreateProjectFile = false;
        }
        ImGui::SetNextWindowPos(ImGui::GetMainViewport()->GetCenter(), ImGuiCond_Appearing, {0.5f, 0.5f});
        if (ImGui::BeginPopupModal("Create Project File?", nullptr, ImGuiWindowFlags_AlwaysAutoResize | ImGuiWindowFlags_NoSavedSettings))
        {
            const auto name = m_MissingProjectFolder.filename().u8string();
            ImGui::Text("%s has no project file.", m_MissingProjectFolder.u8string().c_str());
            ImGui::Text("Create %s.kproj there and open the folder as a project?", name.c_str());
            ImGui::Dummy({0, 6.0f});
            if (PrimaryButton("Create and Open"))
            {
                auto created = Launcher::CreateProjectFile(m_MissingProjectFolder);
                if (auto* file = std::get_if<fs::path>(&created))
                    m_Result = *file;
                else
                    m_Error = std::get<AssetError>(created).Message;
                ImGui::CloseCurrentPopup();
            }
            ImGui::SameLine();
            if (ImGui::Button("Cancel") || ImGui::IsKeyPressed(ImGuiKey_Escape, false))
                ImGui::CloseCurrentPopup();
            ImGui::EndPopup();
        }
    }
} // namespace Kans
