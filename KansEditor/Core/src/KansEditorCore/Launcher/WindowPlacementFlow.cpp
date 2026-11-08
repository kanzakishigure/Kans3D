#include "KansEditorCore/Launcher/WindowPlacementFlow.h"
#include <cmath>
#include "Kans3D/Core/Window.h"

namespace Kans::Launcher
{
    WindowPlacementFlow::WindowPlacementFlow(std::filesystem::path file, float scale) :
        m_File(std::move(file)), m_Scale(scale)
    {}

    WindowSetup WindowPlacementFlow::Start(bool project)
    {
        m_Mode = project ? Mode::Editor : Mode::Launcher;
        return Setup(m_Mode);
    }

    bool WindowPlacementFlow::Remember(const WindowPlacement& current)
    {
        // Re-read first: another editor process may have saved the other placement since.
        WindowPlacementStore store(m_File);
        store.Load();
        store.Set(m_Mode == Mode::Editor ? EditorName : LauncherName, current);
        return store.Save();
    }

    WindowSetup WindowPlacementFlow::Setup(Mode mode) const
    {
        const auto px = [&](float pixels) { return int(std::lround(pixels * m_Scale)); };
        WindowPlacementStore store(m_File);
        store.Load();
        WindowSetup setup;
        if (mode == Mode::Editor)
        {
            setup.Restore     = store.Get(EditorName);
            setup.DefaultSize = {px(1600), px(900)}; // The size it restores to from maximized.
            setup.Maximize    = true;
            setup.MinimumSize = {px(960), px(600)};
        }
        else
        {
            setup.Restore     = store.Get(LauncherName);
            setup.DefaultSize = {px(1100), px(760)};
            setup.MinimumSize = {px(800), px(560)};
        }
        return setup;
    }

    WindowSize InitialWindowSize(const WindowSetup& setup)
    {
        if (setup.Restore && setup.Restore->Width > 0 && setup.Restore->Height > 0)
            return {setup.Restore->Width, setup.Restore->Height};
        return setup.DefaultSize;
    }

    HubLaunch LaunchFromHub(WindowPlacementFlow&         hub,
                            const std::filesystem::path& project,
                            const WindowPlacement&       current,
                            const EditorLauncher&        launch)
    {
        if (!launch)
            return {false, "Cannot start the editor"};
        std::error_code ec;
        auto            absolute = std::filesystem::absolute(project, ec);
        if (ec)
            absolute = project;
        if (auto error = launch(absolute.lexically_normal()))
            return {false, error->empty() ? std::string("Cannot start the editor") : *error};
        hub.Remember(current); // Under the flow's own name: an editor never writes the Hub's.
        return {true, {}};
    }

    void ApplyWindowSetup(Window& window, const WindowSetup& setup)
    {
        window.SetMinimumSize(setup.MinimumSize.Width, setup.MinimumSize.Height);
        if (setup.Restore)
        {
            window.SetPlacement(*setup.Restore);
            return;
        }
        if (window.IsMaximized() || window.IsMinimized())
            window.Restore();
        if (setup.DefaultSize.Width > 0 && setup.DefaultSize.Height > 0)
            window.SetSize(setup.DefaultSize.Width, setup.DefaultSize.Height);
        window.CenterOnMonitor();
        if (setup.Maximize)
            window.Maximize();
    }
} // namespace Kans::Launcher
