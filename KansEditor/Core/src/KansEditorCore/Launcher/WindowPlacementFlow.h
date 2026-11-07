#pragma once
#include <filesystem>
#include <optional>
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

    // The launcher and the editor share one OS window but keep separate placements,
    // "Launcher" and "Editor". The launcher opens small and centred, the editor where it
    // was last left (maximized the first time); neither ever writes the other's placement.
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

        // Startup: with --project the editor's placement, otherwise the launcher's.
        WindowSetup Start(bool project);
        // The launcher hands its window to the editor: keeps the launcher's placement,
        // then answers the editor's.
        WindowSetup OpenEditor(const WindowPlacement& launcher);
        // Keeps the current mode's placement (the editor exiting or switching projects,
        // the launcher closing). False when it could not be written.
        bool Remember(const WindowPlacement& current);
        Mode GetMode() const { return m_Mode; }

    private:
        WindowSetup Setup(Mode mode) const;

        std::filesystem::path m_File;
        float                 m_Scale;
        Mode                  m_Mode = Mode::Launcher;
    };

    // Applies a setup to the main window: the minimum size first (so the previous mode's
    // minimum never bends this placement), then the saved placement or the centred default.
    void ApplyWindowSetup(Window& window, const WindowSetup& setup);
} // namespace Kans::Launcher
