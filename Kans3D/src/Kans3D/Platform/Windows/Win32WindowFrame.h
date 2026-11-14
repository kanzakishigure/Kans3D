#pragma once
#ifdef _WIN32
#include <windows.h>
#include <functional>
#include "Kans3D/Core/WindowGeometry.h"
#include "Win32WindowEdgeColor.h"

struct GLFWwindow;
namespace Kans
{
    // Only the engine's main HWND is subclassed. ImGui viewport HWNDs are untouched.
    class Win32WindowFrame
    {
    public:
        Win32WindowFrame(GLFWwindow* window, const std::function<TitlebarHit(int, int)>& hitTest);
        ~Win32WindowFrame();
        void                SetSize(int width, int height);
        void                SetPosition(int x, int y);
        void                SetMinimumSize(WindowSize minimum) { m_Minimum = minimum; }
        bool                IsInstalled() const { return m_Previous != nullptr; }
        bool                HasNativeWindowEdge() const { return m_HasNativeWindowEdge; }
        void                SetWindowEdgeColor(float r, float g, float b);
        TitlebarButtonState GetButtonState() const { return m_Buttons; }

    private:
        static LRESULT CALLBACK                     Procedure(HWND window, UINT message, WPARAM wParam, LPARAM lParam);
        LRESULT                                     Handle(UINT message, WPARAM wParam, LPARAM lParam);
        int                                         HitTest(POINT screen) const;
        void                                        ExtendFrame();
        void                                        ApplyNativeEdge();
        void                                        CancelPress();
        HWND                                        m_Window   = nullptr;
        WNDPROC                                     m_Previous = nullptr;
        const std::function<TitlebarHit(int, int)>& m_HitTest;
        TitlebarButtonState                         m_Buttons;
        bool                                        m_Pressed = false;
        bool                                        m_HasNativeWindowEdge = false;
        Win32WindowEdgeColor                        m_EdgeColor;
        WindowSize                                  m_Minimum {0, 0};
    };
    WindowPlacement GetWin32WindowPlacement(GLFWwindow* window, bool customFrame);
} // namespace Kans
#endif
