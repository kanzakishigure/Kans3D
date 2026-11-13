#pragma once
#include <memory>
#include <Kans3D.h>
#include "KansEditorCore/Launcher/WindowPlacementFlow.h"
#include "Launcher/ProjectBrowser.h"
#include "UI/Titlebar.h"

namespace Kans
{
    // The project Hub, the whole process when the editor starts without --project: picks or
    // creates a project, starts an editor process for it (`launch`) and closes; a failed
    // launch stays in the Hub with the error. Under a debugger (EditorProcess::OpensInProcess)
    // `openInProcess` replaces the Hub with the editor in this process instead, after the frame;
    // it returns an error message when the project cannot be opened.
    using InProcessOpener = std::function<std::optional<std::string>(const std::filesystem::path& project)>;
    class LauncherLayer : public Layer
    {
    public:
        LauncherLayer(std::filesystem::path                          templatesDir,
                      std::shared_ptr<Launcher::WindowPlacementFlow> placements,
                      Launcher::EditorLauncher                       launch,
                      InProcessOpener                                openInProcess = {});
        void OnAttach() override;
        void OnDetach() override;
        void OnImGuiRender() override;
        void Request(const std::filesystem::path& project) { m_Browser.Request(project); }

    private:
        ProjectBrowser                                  m_Browser;
        std::shared_ptr<Launcher::WindowPlacementFlow> m_Placements;
        Launcher::EditorLauncher                        m_Launch;
        InProcessOpener                                 m_OpenInProcess;
        Titlebar                                        m_Titlebar;
        bool                                            m_Launched = false;
    };
} // namespace Kans
