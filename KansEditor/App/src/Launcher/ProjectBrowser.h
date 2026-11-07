#pragma once
#include <filesystem>
#include <optional>
#include <string>
#include <vector>
#include "Kans3D/Project/RecentProjects.h"
#include "KansEditorCore/Launcher/LauncherModel.h"
#include "UI/FolderBrowser.h"

namespace Kans
{
    // Recent projects, New Project (from a template) and Open Project. Drawn
    // full-window by the launcher and inside a modal by the editor.
    class ProjectBrowser
    {
    public:
        enum class Page
        {
            Recent,
            New
        };

        explicit ProjectBrowser(std::filesystem::path templatesDir);
        // Reloads the recent list and templates (call before showing it again).
        void Refresh();
        void SetPage(Page page);
        // Starts the Open Project browser.
        void BrowseForProject();
        // Opens `path` (a .kproj or a folder): returned by the next Draw, or explained there.
        void Request(const std::filesystem::path& path);
        // Fills the current window's remaining area. Returns the .kproj to open, once.
        std::optional<std::filesystem::path> Draw();

    private:
        void DrawSidebar(float width);
        void DrawRecent();
        void DrawNew();
        void DrawDialogs();
        void ResetNewProject();

        std::filesystem::path                 m_TemplatesDir;
        RecentProjects                        m_Recent;
        std::vector<Launcher::ProjectTemplate> m_Templates;
        Page                                  m_Page = Page::Recent;
        int                                   m_SelectedRecent = -1, m_SelectedTemplate = 0;
        std::string                           m_Search, m_Name, m_Location, m_Error;
        std::optional<std::filesystem::path>  m_Result;
        // A folder chosen in Open Project that has no .kproj yet.
        std::filesystem::path                 m_MissingProjectFolder;
        bool                                  m_AskCreateProjectFile = false;
        FolderBrowser                         m_Browser;
        enum class BrowseTarget
        {
            None,
            Project,
            Location
        } m_BrowseTarget = BrowseTarget::None;
    };
} // namespace Kans
