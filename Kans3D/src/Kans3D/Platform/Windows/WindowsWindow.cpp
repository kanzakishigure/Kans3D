#include "kspch.h"
#include "Kans3D/Platform/Windows/WindowsWindow.h"

#include "Kans3D/Core/FileSystem/FileSystem.h"
#include "Kans3D/Core/Input/Input.h"
#include "Kans3D/RHI/Backend.h"

#ifdef _WIN32
#include "Win32WindowFrame.h"
#endif

#include <imgui.h>
#include <stb_image.h>

namespace Kans
{

    int         WindowsWindow::s_GLFWWindowsCount = 0;

    void GLFWErrorCallback(int error, const char* description)
    {
        // Non-text clipboard data is an expected result of glfwGetClipboardString.
        if (error == GLFW_FORMAT_UNAVAILABLE)
            return;
        CORE_ERROR("GLFW Error ({0}):{1}", error, description);
    }
    Window* Window::Create(const WindowSpecification& props) { return new WindowsWindow(props); }
    WindowsWindow::WindowsWindow(const WindowSpecification& props) : m_Specification(props), m_Window(nullptr) {}

    WindowsWindow::~WindowsWindow() { WindowsWindow::Shutdown(); }

    void WindowsWindow::Init()
    {
        PROFILE_FUCTION();

        CORE_INFO(
            "create window [{0}] : ({1},{2})", m_Specification.Title, m_Specification.Width, m_Specification.Height);
        // glfwInit is idempotent, including when another GLFW consumer initialized it.
        const int success = glfwInit();
        CORE_ASSERT(success, "unable init GLFW!");
        glfwSetErrorCallback(GLFWErrorCallback);
        // Custom captions retain the native resize/snap style on Windows. Other
        // platforms deliberately retain native decorations too. Reset sticky GLFW hints.
        glfwWindowHint(GLFW_DECORATED, GLFW_TRUE);

        m_Window = static_cast<GLFWwindow*>(
            RenderHardware::CreateWindowSurface(RenderHardware::GetBackend(), m_Specification));
        if (!m_Specification.Fullscreen)
            ++s_GLFWWindowsCount;

        // Set GLFW Event Callback data
        glfwSetWindowUserPointer(m_Window, this);
        // Set GLFW Event Callback
        glfwSetWindowSizeCallback(m_Window, windowSizeCallback);
        glfwSetWindowCloseCallback(m_Window, windowCloseCallback);
        glfwSetKeyCallback(m_Window, keyCallback);
        glfwSetMouseButtonCallback(m_Window, mouseButtonCallback);
        glfwSetScrollCallback(m_Window, scrollCallback);
        glfwSetCursorPosCallback(m_Window, cursorPosCallback);
        glfwSetCharCallback(m_Window, charCallback);

        glfwSetWindowPosCallback(m_Window, [](GLFWwindow* window, int, int) {
            static_cast<WindowsWindow*>(glfwGetWindowUserPointer(window))->UpdateNormalRectangle();
        });
        glfwSetWindowMaximizeCallback(m_Window, [](GLFWwindow* window, int maximized) {
            auto& data               = *static_cast<WindowsWindow*>(glfwGetWindowUserPointer(window));
            data.m_WasMaximized      = maximized != 0;
            data.m_WaitingForRestore = maximized != 0;
        });
        glfwSetWindowIconifyCallback(m_Window, [](GLFWwindow* window, int iconified) {
            auto& data               = *static_cast<WindowsWindow*>(glfwGetWindowUserPointer(window));
            data.m_WaitingForRestore = iconified != 0 || data.IsMaximized();
        });
#ifdef _WIN32
        if (!m_Specification.Fullscreen && m_Specification.Titlebar == TitlebarMode::Custom)
        {
            const auto position = GetPosition();
            const auto size     = GetSize();
            m_CustomFrame       = std::make_unique<Win32WindowFrame>(m_Window, m_TitlebarHitTest);
            if (HasCustomTitlebar())
            {
                m_CustomFrame->SetPosition(position.X, position.Y);
                m_CustomFrame->SetSize(size.Width, size.Height);
            }
        }
#endif
        const auto size        = GetSize();
        m_Specification.Width  = size.Width;
        m_Specification.Height = size.Height;
        UpdateNormalRectangle();
        // set the windows Icon
#ifdef _WIN32
        // An executable with a GLFW_ICON resource already gives the window class its multi-size
        // icon, which Windows scales far better than one PNG.
        const bool exeIcon = FindResourceW(nullptr, L"GLFW_ICON", MAKEINTRESOURCEW(14) /* RT_GROUP_ICON */) != nullptr;
#else
        const bool exeIcon = false;
#endif
        if (!exeIcon)
        {
            GLFWimage             icon;
            int                   channels;
            std::filesystem::path iconpath = KansFileSystem::GetResoucesFolder() / "Icon/Kanslogo.png";
            icon.pixels = stbi_load(iconpath.string().c_str(), &icon.width, &icon.height, &channels, 4);
            if (icon.pixels)
            {
                glfwSetWindowIcon(m_Window, 1, &icon);
                stbi_image_free(icon.pixels);
            }
            else
                CORE_ERROR("Cannot load window icon '{}'; keeping the default window icon.", iconpath.string());
        }
    }
    void WindowsWindow::Shutdown()
    {
        PROFILE_FUCTION();
        // GLFWapi
#ifdef _WIN32
        m_CustomFrame.reset(); // Unhook before GLFW destroys its HWND.
#endif
        if (m_Window)
            glfwDestroyWindow(m_Window);
        m_Window = nullptr;
    }
    void WindowsWindow::ProcessEvents()
    {
        PROFILE_FUCTION();
        UpdateNormalRectangle();
        glfwPollEvents();
        if (m_PendingPlacement)
        {
            if (IsMaximized() || IsMinimized())
                Restore(); // A minimized window may first restore into its maximized state.
            if (!IsMaximized() && !IsMinimized())
            {
                const auto placement = *m_PendingPlacement;
                m_PendingPlacement.reset();
                ApplyPlacement(placement);
            }
        }
        UpdateNormalRectangle();
        // detect the handle or other device
        Input::Update();
    }

    void WindowsWindow::SwapBuffers()
    {
        RenderHardware::PresentWindow(*this);
    }

    WindowSize WindowsWindow::GetSize() const
    {
        WindowSize size;
        glfwGetWindowSize(m_Window, &size.Width, &size.Height);
        return size;
    }
    void WindowsWindow::SetSize(int width, int height)
    {
        width  = std::max({1, width, m_MinimumSize.Width});
        height = std::max({1, height, m_MinimumSize.Height});
#ifdef _WIN32
        if (HasCustomTitlebar())
            m_CustomFrame->SetSize(width, height);
        else
#endif
            glfwSetWindowSize(m_Window, width, height);
        UpdateNormalRectangle();
        if (!IsMaximized() && !IsMinimized() && !m_WaitingForRestore)
        {
            // Preserve the requested normal size if an X11 configure is still queued
            // and the caller maximizes before the next event pump.
            m_NormalPlacement.Width  = width;
            m_NormalPlacement.Height = height;
        }
    }
    WindowPosition WindowsWindow::GetPosition() const
    {
        WindowPosition position;
        glfwGetWindowPos(m_Window, &position.X, &position.Y);
        return position;
    }
    void WindowsWindow::SetPosition(int x, int y)
    {
#ifdef _WIN32
        if (HasCustomTitlebar())
            m_CustomFrame->SetPosition(x, y);
        else
#endif
            glfwSetWindowPos(m_Window, x, y);
        UpdateNormalRectangle();
        if (!IsMaximized() && !IsMinimized() && !m_WaitingForRestore)
        {
            m_NormalPlacement.X = x;
            m_NormalPlacement.Y = y;
        }
    }
    void WindowsWindow::CenterOnMonitor()
    {
        int        count       = 0;
        auto**     monitors    = glfwGetMonitors(&count);
        auto*      selected    = glfwGetPrimaryMonitor();
        const auto position    = GetPosition();
        const auto size        = GetSize();
        int64_t    bestOverlap = 0;
        for (int i = 0; i < count; ++i)
        {
            int x, y;
            glfwGetMonitorPos(monitors[i], &x, &y);
            const auto* mode = glfwGetVideoMode(monitors[i]);
            if (!mode)
                continue;
            const int64_t overlap =
                std::max<int64_t>(0,
                                  std::min<int64_t>(int64_t(position.X) + size.Width, int64_t(x) + mode->width) -
                                      std::max(position.X, x)) *
                std::max<int64_t>(0,
                                  std::min<int64_t>(int64_t(position.Y) + size.Height, int64_t(y) + mode->height) -
                                      std::max(position.Y, y));
            if (overlap > bestOverlap)
            {
                selected    = monitors[i];
                bestOverlap = overlap;
            }
        }
        if (selected)
        {
            int x, y, width, height;
            glfwGetMonitorWorkarea(selected, &x, &y, &width, &height);
            int left = 0, top = 0, right = 0, bottom = 0;
            if (!HasCustomTitlebar())
                glfwGetWindowFrameSize(m_Window, &left, &top, &right, &bottom);
            SetPosition(x + (width - size.Width - left - right) / 2 + left,
                        y + (height - size.Height - top - bottom) / 2 + top);
        }
    }
    void WindowsWindow::UpdateNormalRectangle()
    {
        if (!m_Window || m_PlacementChanging || m_WaitingForRestore || IsMaximized() || IsMinimized())
            return;
        const auto p = GetPosition();
        const auto s = GetSize();
        if (s.Width > 0 && s.Height > 0)
            m_NormalPlacement = {p.X, p.Y, s.Width, s.Height, false};
    }
    void WindowsWindow::Maximize()
    {
        m_WaitingForRestore = true; // X11 state requests are asynchronous.
        m_WasMaximized      = true;
        glfwMaximizeWindow(m_Window);
    }
    void WindowsWindow::Minimize()
    {
        m_WaitingForRestore = true;
        glfwIconifyWindow(m_Window);
    }
    void WindowsWindow::Restore()
    {
        glfwRestoreWindow(m_Window);
        m_WasMaximized = false;
        if (!IsMaximized() && !IsMinimized())
            m_WaitingForRestore = false;
    }
    bool WindowsWindow::IsMaximized() const { return glfwGetWindowAttrib(m_Window, GLFW_MAXIMIZED) != 0; }
    bool WindowsWindow::IsMinimized() const { return glfwGetWindowAttrib(m_Window, GLFW_ICONIFIED) != 0; }
    void WindowsWindow::SetMinimumSize(int width, int height)
    {
        m_MinimumSize = {std::max(0, width), std::max(0, height)};
#ifdef _WIN32
        if (HasCustomTitlebar())
            m_CustomFrame->SetMinimumSize(m_MinimumSize);
#endif
        const bool limited = m_MinimumSize.Width > 0 || m_MinimumSize.Height > 0;
        // GLFW's Win32 implementation applies minima only when both axes are set.
        glfwSetWindowSizeLimits(m_Window,
                                limited ? std::max(1, m_MinimumSize.Width) : GLFW_DONT_CARE,
                                limited ? std::max(1, m_MinimumSize.Height) : GLFW_DONT_CARE,
                                GLFW_DONT_CARE,
                                GLFW_DONT_CARE);
    }
    WindowPlacement WindowsWindow::GetPlacement() const
    {
        if (m_PendingPlacement)
            return *m_PendingPlacement;
#ifdef _WIN32
        return GetWin32WindowPlacement(m_Window, HasCustomTitlebar());
#else
        auto placement = m_NormalPlacement;
        if (!IsMaximized() && !IsMinimized() && !m_WaitingForRestore)
        {
            const auto p = GetPosition();
            const auto s = GetSize();
            placement    = {p.X, p.Y, s.Width, s.Height, false};
        }
        placement.Maximized = IsMaximized() || (IsMinimized() && m_WasMaximized);
        return placement;
#endif
    }
    void WindowsWindow::SetPlacement(const WindowPlacement& requested)
    {
        int                          count    = 0;
        auto**                       monitors = glfwGetMonitors(&count);
        std::vector<MonitorWorkArea> areas;
        int                          left = 0, top = 0, right = 0, bottom = 0;
        if (!HasCustomTitlebar())
            glfwGetWindowFrameSize(m_Window, &left, &top, &right, &bottom);
        for (int i = 0; i < count; ++i)
        {
            MonitorWorkArea area;
            glfwGetMonitorWorkarea(monitors[i], &area.X, &area.Y, &area.Width, &area.Height);
            // Clamp client origins/sizes against work areas inset by native decorations,
            // otherwise a native caption could remain above the reachable desktop.
            area.X += left;
            area.Y += top;
            area.Width  = std::max(1, area.Width - left - right);
            area.Height = std::max(1, area.Height - top - bottom);
            areas.push_back(area);
        }
        auto placement     = requested;
        placement.Width    = std::max(placement.Width, m_MinimumSize.Width);
        placement.Height   = std::max(placement.Height, m_MinimumSize.Height);
        placement          = ClampWindowPlacement(placement, areas);
        m_PendingPlacement = placement;
        Restore();
        if (IsMaximized() && !IsMinimized())
            Restore();
        if (!IsMaximized() && !IsMinimized())
        {
            m_PendingPlacement.reset();
            ApplyPlacement(placement);
        }
        // On X11 restore is asynchronous: ProcessEvents applies it once normal.
    }
    void WindowsWindow::ApplyPlacement(const WindowPlacement& placement)
    {
        m_PlacementChanging = true;
        // A replacement monitor may be smaller than the configured UI minimum.
        const WindowSize minimum {std::min(m_MinimumSize.Width, placement.Width),
                                  std::min(m_MinimumSize.Height, placement.Height)};
#ifdef _WIN32
        if (HasCustomTitlebar())
            m_CustomFrame->SetMinimumSize(minimum);
#endif
        const bool limited = minimum.Width > 0 || minimum.Height > 0;
        glfwSetWindowSizeLimits(m_Window,
                                limited ? std::max(1, minimum.Width) : GLFW_DONT_CARE,
                                limited ? std::max(1, minimum.Height) : GLFW_DONT_CARE,
                                GLFW_DONT_CARE,
                                GLFW_DONT_CARE);
#ifdef _WIN32
        if (HasCustomTitlebar())
        {
            m_CustomFrame->SetPosition(placement.X, placement.Y);
            m_CustomFrame->SetSize(placement.Width, placement.Height);
        }
        else
#endif
        {
            glfwSetWindowPos(m_Window, placement.X, placement.Y);
            glfwSetWindowSize(m_Window, placement.Width, placement.Height);
        }
        m_NormalPlacement           = placement;
        m_NormalPlacement.Maximized = false;
        m_PlacementChanging         = false;
        if (placement.Maximized)
            Maximize();
    }
    bool WindowsWindow::HasCustomTitlebar() const
    {
#ifdef _WIN32
        return m_CustomFrame && m_CustomFrame->IsInstalled();
#else
        return false;
#endif
    }
    TitlebarButtonState WindowsWindow::GetTitlebarButtonState() const
    {
#ifdef _WIN32
        if (m_CustomFrame)
            return m_CustomFrame->GetButtonState();
#endif
        return {};
    }

    void WindowsWindow::SetVSync(bool enable)
    {
        PROFILE_FUCTION();

        RenderHardware::SetWindowVSync(*this, enable);

        m_Specification.VSync = enable;
    }

    bool WindowsWindow::IsVSync() const { return m_Specification.VSync; }

} // namespace Kans
