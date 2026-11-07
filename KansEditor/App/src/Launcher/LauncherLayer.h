#pragma once
#include <Kans3D.h>
#include "Launcher/ProjectBrowser.h"

namespace Kans
{
    // Shown when the editor starts without --project: picks or creates a
    // project, then replaces itself with the editor layer.
    class LauncherLayer : public Layer
    {
    public:
        explicit LauncherLayer(std::filesystem::path templatesDir);
        void OnAttach() override;
        void OnImGuiRender() override;
        void Request(const std::filesystem::path& project) { m_Browser.Request(project); }

    private:
        ProjectBrowser m_Browser;
        bool           m_Opening = false;
    };
} // namespace Kans
