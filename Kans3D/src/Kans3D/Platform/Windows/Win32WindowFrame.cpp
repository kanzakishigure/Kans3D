#include "Win32WindowFrame.h"
#ifdef _WIN32
#include <dwmapi.h>
#include <windowsx.h>
#define GLFW_EXPOSE_NATIVE_WIN32
#include <algorithm>
#include <GLFW/glfw3.h>
#include <GLFW/glfw3native.h>

namespace Kans
{
    namespace
    {
        constexpr wchar_t Property[] = L"Kans3D.CustomWindowFrame";
        WindowSize        FrameThickness(HWND window)
        {
            const UINT dpi     = GetDpiForWindow(window);
            const int  padding = GetSystemMetricsForDpi(SM_CXPADDEDBORDER, dpi);
            return {GetSystemMetricsForDpi(SM_CXSIZEFRAME, dpi) + padding,
                    GetSystemMetricsForDpi(SM_CYSIZEFRAME, dpi) + padding};
        }
        POINT MessagePoint(LPARAM value) { return {GET_X_LPARAM(value), GET_Y_LPARAM(value)}; }
    } // namespace
    Win32WindowFrame::Win32WindowFrame(GLFWwindow* window, const std::function<TitlebarHit(int, int)>& hitTest) :
        m_Window(glfwGetWin32Window(window)), m_HitTest(hitTest)
    {
        if (!SetPropW(m_Window, Property, this))
            return;
        SetLastError(0);
        m_Previous =
            reinterpret_cast<WNDPROC>(SetWindowLongPtrW(m_Window, GWLP_WNDPROC, reinterpret_cast<LONG_PTR>(Procedure)));
        if (!m_Previous)
        {
            RemovePropW(m_Window, Property);
            return; // Keep the native titlebar if subclassing fails.
        }
        ExtendFrame();
        SetWindowPos(
            m_Window, nullptr, 0, 0, 0, 0, SWP_NOMOVE | SWP_NOSIZE | SWP_NOZORDER | SWP_NOACTIVATE | SWP_FRAMECHANGED);
    }
    Win32WindowFrame::~Win32WindowFrame()
    {
        if (!m_Previous)
            return;
        CancelPress();
        SetWindowLongPtrW(m_Window, GWLP_WNDPROC, reinterpret_cast<LONG_PTR>(m_Previous));
        RemovePropW(m_Window, Property);
    }
    void Win32WindowFrame::SetSize(int width, int height)
    {
        // GLFW 3.4 adds a native caption to client sizes. Our normal client and outer
        // rectangles coincide, so bypass that conversion while retaining WM_SIZE.
        SetWindowPos(m_Window, nullptr, 0, 0, width, height, SWP_NOMOVE | SWP_NOZORDER | SWP_NOACTIVATE);
    }
    void Win32WindowFrame::SetPosition(int x, int y)
    {
        SetWindowPos(m_Window, nullptr, x, y, 0, 0, SWP_NOSIZE | SWP_NOZORDER | SWP_NOACTIVATE);
    }
    void Win32WindowFrame::ExtendFrame() const
    {
        // One transparent DWM pixel preserves the shadow. WM_NCCALCSIZE gives the
        // renderer the whole normal rectangle, so no unpainted caption strip remains.
        const MARGINS margins {0, 0, 1, 0};
        DwmExtendFrameIntoClientArea(m_Window, &margins);
    }
    int Win32WindowFrame::HitTest(POINT screen) const
    {
        RECT rect;
        GetWindowRect(m_Window, &rect);
        POINT client = screen;
        ScreenToClient(m_Window, &client);
        const auto border = FrameThickness(m_Window);
        // Borders must win even when application UI covers the top resize edge.
        const int edge = TitlebarHitTestCode(TitlebarHit::Client,
                                             screen.x - rect.left,
                                             screen.y - rect.top,
                                             rect.right - 1 - screen.x,
                                             rect.bottom - 1 - screen.y,
                                             border.Width,
                                             border.Height,
                                             IsZoomed(m_Window) != FALSE);
        if (edge != HTCLIENT)
            return edge;
        if (!PtInRect(&rect, screen))
            return HTNOWHERE;
        return TitlebarHitTestCode(m_HitTest ? m_HitTest(client.x, client.y) : TitlebarHit::Client,
                                   screen.x - rect.left,
                                   screen.y - rect.top,
                                   rect.right - 1 - screen.x,
                                   rect.bottom - 1 - screen.y,
                                   0,
                                   0,
                                   true);
    }
    void Win32WindowFrame::CancelPress()
    {
        m_Pressed = false;
        m_Buttons = {};
        if (GetCapture() == m_Window)
            ReleaseCapture();
    }
    LRESULT CALLBACK Win32WindowFrame::Procedure(HWND window, UINT message, WPARAM wParam, LPARAM lParam)
    {
        auto* frame = static_cast<Win32WindowFrame*>(GetPropW(window, Property));
        return frame ? frame->Handle(message, wParam, lParam) : DefWindowProcW(window, message, wParam, lParam);
    }
    LRESULT Win32WindowFrame::Handle(UINT message, WPARAM wParam, LPARAM lParam)
    {
        switch (message)
        {
            case WM_NCCALCSIZE: {
                // Both RECT and NCCALCSIZE_PARAMS start with the proposed outer RECT.
                // Maximized HWNDs extend beyond the monitor by the resize frame.
                auto* rect = reinterpret_cast<RECT*>(lParam);
                if (IsZoomed(m_Window))
                {
                    const auto frame = FrameThickness(m_Window);
                    InflateRect(rect, -frame.Width, -frame.Height);
                }
                return 0;
            }
            case WM_GETMINMAXINFO: {
                const auto result = CallWindowProcW(m_Previous, m_Window, message, wParam, lParam);
                auto*      info   = reinterpret_cast<MINMAXINFO*>(lParam);
                // GLFW's minimum adds a native caption too; use our actual frame.
                if (m_Minimum.Width > 0)
                    info->ptMinTrackSize.x = m_Minimum.Width;
                if (m_Minimum.Height > 0)
                    info->ptMinTrackSize.y = m_Minimum.Height;
                return result;
            }
            case WM_NCHITTEST:
                return HitTest(MessagePoint(lParam));
            case WM_NCACTIVATE:
                // -1 updates activation without painting a system caption.
                if (!wParam)
                    CancelPress();
                return CallWindowProcW(m_Previous, m_Window, message, wParam, -1);
            case WM_NCPAINT:
                return 0; // All visible pixels belong to the renderer/DWM.
            case WM_ACTIVATE:
            case WM_DWMCOMPOSITIONCHANGED:
                ExtendFrame();
                break;
            case WM_GETDPISCALEDSIZE:
                // Our normal client and outer sizes coincide, so Windows' default linear
                // scaling is exact. FALSE requests it; GLFW's handler would add a native
                // caption, and TRUE would trust a SIZE we never wrote.
                return FALSE;
            case WM_DPICHANGED: {
                // Let GLFW apply the suggested DPI rectangle and deliver resize events,
                // then recalculate our frame using the HWND's new monitor DPI.
                const auto result = CallWindowProcW(m_Previous, m_Window, message, wParam, lParam);
                ExtendFrame();
                SetWindowPos(m_Window,
                             nullptr,
                             0,
                             0,
                             0,
                             0,
                             SWP_NOMOVE | SWP_NOSIZE | SWP_NOZORDER | SWP_NOACTIVATE | SWP_FRAMECHANGED);
                return result;
            }
            case WM_NCMOUSEMOVE: {
                const bool over           = HitTest(MessagePoint(lParam)) == HTMAXBUTTON;
                m_Buttons.MaximizeHovered = over;
                m_Buttons.MaximizePressed = m_Pressed && over;
                TRACKMOUSEEVENT tracking {sizeof(tracking), TME_LEAVE | TME_NONCLIENT, m_Window, 0};
                TrackMouseEvent(&tracking);
                if (over)
                {
                    // HTMAXBUTTON advertises the Snap Layout target. Consume mouse
                    // messages so native caption-button painting cannot cover the UI.
                    return 0;
                }
                break;
            }
            case WM_NCLBUTTONDBLCLK:
            case WM_NCLBUTTONDOWN:
                if (wParam == HTMAXBUTTON)
                {
                    m_Pressed = true;
                    m_Buttons = {true, true};
                    SetCapture(m_Window); // Release outside the button must cancel the click.
                    return 0;
                }
                break;
            case WM_MOUSEMOVE:
                if (m_Pressed)
                {
                    POINT point = MessagePoint(lParam);
                    ClientToScreen(m_Window, &point);
                    const bool over = HitTest(point) == HTMAXBUTTON;
                    m_Buttons       = {over, over};
                    return 0;
                }
                break;
            case WM_NCLBUTTONUP:
            case WM_LBUTTONUP:
                if (m_Pressed || (message == WM_NCLBUTTONUP && wParam == HTMAXBUTTON))
                {
                    POINT point = MessagePoint(lParam);
                    if (message == WM_LBUTTONUP)
                        ClientToScreen(m_Window, &point);
                    const bool click = m_Pressed && HitTest(point) == HTMAXBUTTON;
                    CancelPress();
                    if (click)
                        ShowWindow(m_Window, IsZoomed(m_Window) ? SW_RESTORE : SW_MAXIMIZE);
                    return 0;
                }
                break;
            case WM_NCMOUSELEAVE: {
                // Capture routes subsequent moves through the client path. A leave
                // notification during a press need not mean the pointer left the button.
                POINT      cursor;
                const bool over = m_Pressed && GetCursorPos(&cursor) && HitTest(cursor) == HTMAXBUTTON;
                m_Buttons       = {over, over};
                return 0;
            }
            case WM_CANCELMODE:
                CancelPress();
                break;
            case WM_CAPTURECHANGED:
                if (reinterpret_cast<HWND>(lParam) != m_Window)
                {
                    m_Pressed = false;
                    m_Buttons = {};
                }
                break;
        }
        return CallWindowProcW(m_Previous, m_Window, message, wParam, lParam);
    }

    WindowPlacement GetWin32WindowPlacement(GLFWwindow* window, bool customFrame)
    {
        const auto      hwnd = glfwGetWin32Window(window);
        WINDOWPLACEMENT native {sizeof(native)};
        if (!GetWindowPlacement(hwnd, &native))
        {
            WindowPlacement fallback;
            glfwGetWindowPos(window, &fallback.X, &fallback.Y);
            glfwGetWindowSize(window, &fallback.Width, &fallback.Height);
            fallback.Maximized = IsZoomed(hwnd) != FALSE;
            return fallback;
        }
        RECT rect = native.rcNormalPosition;
        // WINDOWPLACEMENT is in workspace coordinates for ordinary top-level windows.
        // GLFW positions are client origins in screen coordinates, including when the
        // taskbar is above/left of the monitor's desktop origin.
        const auto exStyle = static_cast<DWORD>(GetWindowLongPtrW(hwnd, GWL_EXSTYLE));
        if (!(exStyle & WS_EX_TOOLWINDOW))
        {
            MONITORINFO monitor {sizeof(monitor)};
            if (GetMonitorInfoW(MonitorFromWindow(hwnd, MONITOR_DEFAULTTONEAREST), &monitor))
                OffsetRect(
                    &rect, monitor.rcWork.left - monitor.rcMonitor.left, monitor.rcWork.top - monitor.rcMonitor.top);
        }
        RECT frame {};
        if (!customFrame)
            AdjustWindowRectExForDpi(
                &frame, static_cast<DWORD>(GetWindowLongPtrW(hwnd, GWL_STYLE)), FALSE, exStyle, GetDpiForWindow(hwnd));
        const bool maximized =
            native.showCmd == SW_SHOWMAXIMIZED || (IsIconic(hwnd) && (native.flags & WPF_RESTORETOMAXIMIZED));
        return {static_cast<int>(rect.left - frame.left),
                static_cast<int>(rect.top - frame.top),
                static_cast<int>((rect.right - rect.left) - (frame.right - frame.left)),
                static_cast<int>((rect.bottom - rect.top) - (frame.bottom - frame.top)),
                maximized};
    }
} // namespace Kans
#endif
