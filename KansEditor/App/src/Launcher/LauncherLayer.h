#pragma once
#include <memory>
#include <Kans3D.h>
#include "KansEditorCore/Launcher/WindowPlacementFlow.h"
#include "Launcher/ProjectBrowser.h"
#include "UI/Titlebar.h"

namespace Kans
{
    // Shown when the editor starts without --project: picks or creates a
    // project, then replaces itself with the editor layer in the same window,
    // which moves from the launcher's placement to the editor's.
    class LauncherLayer : public Layer
    {
    public:
        LauncherLayer(std::filesystem::path templatesDir, std::shared_ptr<Launcher::WindowPlacementFlow> placements);
        void OnAttach() override;
        void OnDetach() override;
        void OnImGuiRender() override;
        void Request(const std::filesystem::path& project) { m_Browser.Request(project); }

    private:
        ProjectBrowser                                  m_Browser;
        std::shared_ptr<Launcher::WindowPlacementFlow> m_Placements;
        Titlebar                                        m_Titlebar;
        bool                                            m_Opening = false;
    };
} // namespace Kans
