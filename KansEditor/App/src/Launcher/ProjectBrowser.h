#pragma once
#include <filesystem>
#include <memory>
#include <optional>
#include <string>
#include <vector>
#include <imgui.h>
#include "Kans3D/Project/RecentProjects.h"
#include "KansEditorCore/Launcher/LauncherModel.h"
#include "KansEditorCore/Panels/ContentBrowser/ThumbnailCache.h"
#include "UI/FolderBrowser.h"

namespace Kans
{
    // Recent projects (a grid of thumbnail cards), New Project (from a template) and
    // Open Project. Drawn full-window by the launcher and inside a modal by the editor.
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
        // Index into the recent list of the selected card, or -1.
        int GetSelectedRecent() const { return m_SelectedRecent; }
        // The message shown under the list (a failed open or save), empty when none.
        const std::string& GetError() const { return m_Error; }

    private:
        // What a recent entry shows besides its name: the project's thumbnail, if any.
        struct Card
        {
            std::filesystem::path           Thumbnail;
            std::filesystem::file_time_type Modified {};
            bool                            Exists = false;
        };
        void RefreshCards();
        // Draws one card at `min`; sets `remove` when "Remove from List" is chosen.
        void DrawCard(int index, ImVec2 min, ImVec2 size, float thumbnailHeight, int& remove);
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
        std::vector<Card>                     m_Cards; // Parallel to m_Recent.Entries().
        // Thumbnail textures load when first drawn and go with the browser.
        std::unique_ptr<Browser::ThumbnailCache> m_Thumbnails;
        bool                                  m_ScrollToSelection = false;
        enum class BrowseTarget
        {
            None,
            Project,
            Location
        } m_BrowseTarget = BrowseTarget::None;
    };
} // namespace Kans
