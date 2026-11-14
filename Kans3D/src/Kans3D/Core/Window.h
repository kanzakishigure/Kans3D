#pragma once
#include <cstdint>
#include <functional>
#include <string>
#include "Kans3D/Core/Base/Base.h"
#include "Kans3D/Core/Events/Event.h"
#include "Kans3D/Core/WindowGeometry.h"

namespace Kans
{

    struct WindowSpecification
    {
        std::string Title        = "Kans Engine";
        uint32_t    Width        = 1920;
        uint32_t    Height       = 1080;
        bool        Fullscreen   = false;
        // Former HideTitlebar=true maps to Custom; false maps to Native.
        // Custom is effective only on Windows, and only for the main window.
        TitlebarMode Titlebar     = TitlebarMode::Custom;
        bool        VSync        = true;
    };
    class Window
    {
    public:
        using EventCallbackFn = std::function<void(Event&)>;
        virtual ~Window() {}

        virtual void Init()          = 0;
        virtual void ProcessEvents() = 0;
        virtual void SwapBuffers()   = 0;

        // Client sizes and positions use GLFW screen coordinates, not framebuffer pixels.
        // Under DPI scaling these may differ from framebuffer pixels (notably macOS).
        // Values are not rescaled when saved/restored across monitors; native frame metrics
        // and hit-test coordinates follow the current monitor DPI. Call on the window thread.
        // Positioning/centering are subject to compositor support (e.g. Wayland restrictions).
        virtual WindowSize     GetSize() const                = 0;
        virtual void           SetSize(int width, int height) = 0;
        virtual WindowPosition GetPosition() const            = 0;
        virtual void           SetPosition(int x, int y)      = 0;
        virtual void           CenterOnMonitor()              = 0;
        virtual void           Maximize()                     = 0;
        virtual void           Minimize()                     = 0;
        virtual void           Restore()                      = 0;
        virtual bool           IsMaximized() const            = 0;
        virtual bool           IsMinimized() const            = 0;
        // Zero removes that axis's minimum. A monitor's work area takes priority when
        // restoring a placement that cannot fit, even if smaller than this minimum.
        virtual void SetMinimumSize(int width, int height) = 0;
        // Always the normal client rectangle, including while minimized/maximized.
        // SetPlacement clamps against current monitor work areas; on X11 a restore
        // from maximized/minimized is applied asynchronously by ProcessEvents.
        virtual WindowPlacement GetPlacement() const                           = 0;
        virtual void            SetPlacement(const WindowPlacement& placement) = 0;
        // Client-area screen coordinates, matching ImGui/GLFW cursor coordinates.
        // Keep this callback cheap: Windows calls it during native message dispatch.
        virtual void                SetTitlebarHitTest(std::function<TitlebarHit(int x, int y)> callback) = 0;
        virtual TitlebarButtonState GetTitlebarButtonState() const                                        = 0;
        virtual bool                HasCustomTitlebar() const                                             = 0;
        virtual bool                HasNativeWindowEdge() const { return false; }
        virtual void                SetWindowEdgeColor(float r, float g, float b) {}

        virtual unsigned int               GetWidth() const                                        = 0;
        virtual unsigned int               GetHeight() const                                       = 0;
        virtual const WindowSpecification& GetWindowSpecification() const                          = 0;
        virtual const void                 SetWindowSpecification(const WindowSpecification& spec) = 0;

        virtual void         SetEventCallback(const EventCallbackFn& callback) = 0;
        virtual void         SetVSync(bool enabled)                            = 0;
        virtual bool         IsVSync() const                                   = 0;
        virtual inline void* GetNativeWindow() const                           = 0;
        virtual void         SetTitle(const std::string& title) {}

        // return a windows pointer of the platform we support
        static Window* Create(const WindowSpecification& props = WindowSpecification());
    };
} // namespace Kans
