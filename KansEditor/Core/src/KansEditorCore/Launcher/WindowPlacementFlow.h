#pragma once
#include <filesystem>
#include <functional>
#include <optional>
#include <string>
#include "Kans3D/Core/WindowGeometry.h"
#include "KansEditorCore/Launcher/WindowPlacementStore.h"

namespace Kans
{
    class Window;
}
namespace Kans::Launcher
{
    // What to do with the main window when a mode starts.
    struct WindowSetup
    {
        // The saved placement to restore (it carries its own maximized state).
        std::optional<WindowPlacement> Restore;
        // Without one: this client size, centred on the monitor, then maximized if asked.
        WindowSize DefaultSize {0, 0};
        bool       Maximize = false;
        WindowSize MinimumSize {0, 0};
    };

    // The project Hub and the editor run as separate processes with separate placements,
    // "Launcher" for the Hub and "Editor" for the editor. The Hub opens small and centred,
    // the editor where it was last left (maximized the first time); each process creates
    // its window at its own placement and never writes the other's.
    class WindowPlacementFlow
    {
    public:
        enum class Mode
        {
            Launcher,
            Editor
        };
        static constexpr const char* LauncherName = "Launcher";
        static constexpr const char* EditorName   = "Editor";

        // `scale` multiplies the default and minimum sizes (EditorTheme::Px).
        explicit WindowPlacementFlow(std::filesystem::path file  = WindowPlacementStore::DefaultStorageFile(),
                                     float                 scale = 1.0f);

        // Startup: with --project this process is the editor, otherwise the Hub.
        WindowSetup Start(bool project);
        // Keeps this process's placement (the Hub launching a project or closing, the editor
        // exiting or switching projects). False when it could not be written.
        bool Remember(const WindowPlacement& current);
        Mode GetMode() const { return m_Mode; }

    private:
        WindowSetup Setup(Mode mode) const;

        std::filesystem::path m_File;
        float                 m_Scale;
        Mode                  m_Mode = Mode::Launcher;
    };

    // The client size to create the main window at, so a setup never resizes it at start:
    // the saved placement's (restored) size, otherwise the default.
    WindowSize InitialWindowSize(const WindowSetup& setup);

    // Starts an editor process for a project; returns an error message on failure.
    using EditorLauncher = std::function<std::optional<std::string>(const std::filesystem::path& project)>;
    struct HubLaunch
    {
        bool        Close = false; // The editor started: the Hub process closes.
        std::string Error;         // Otherwise why not, shown in the Hub.
    };
    // The Hub opening `project` (made absolute: the new process starts in another folder):
    // after a successful launch it keeps the Hub's placement and closes; a failed launch
    // keeps it open with the error and saves nothing.
    HubLaunch LaunchFromHub(WindowPlacementFlow&         hub,
                            const std::filesystem::path& project,
                            const WindowPlacement&       current,
                            const EditorLauncher&        launch);

    // Applies a setup to the main window: the minimum size first (so the previous mode's
    // minimum never bends this placement), then the saved placement or the centred default.
    void ApplyWindowSetup(Window& window, const WindowSetup& setup);
} // namespace Kans::Launcher
