#include "kspch.h"
#include "Kans3D/Platform/Windows/WindowsWindow.h"

#include "Kans3D/Core/FileSystem/FileSystem.h"
#include "Kans3D/Core/Input/Input.h"
#include "Kans3D/RHI/Backend.h"

#include <imgui.h>
#include <stb_image.h>

namespace Kans
{

    static bool s_GLiFWIntialized                 = false;
    int         WindowsWindow::s_GLFWWindowsCount = 0;

    static void GLFWErrorCallback(int error, const char* description)
    {
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
        if (!s_GLiFWIntialized)
        {
            // TODO glfwterminate on system shutdown
            PROFILE_SCOPE("glfwInit");

            int success = glfwInit();
            CORE_ASSERT(success, "unable init GLFW!");
            glfwSetErrorCallback(GLFWErrorCallback);
            s_GLiFWIntialized = true;
        }
        if (m_Specification.HideTitlebar)
        {
            glfwWindowHint(GLFW_DECORATED, false);
        }

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

        // Update window size to actual size.if we use the fullscreenOption
        if (false)
        {
            int width, height;
            glfwGetWindowSize(m_Window, &width, &height);
            m_Specification.Width  = width;
            m_Specification.Height = height;
        }
        // set the windows Icon
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
        glfwDestroyWindow(m_Window);
    }
    void WindowsWindow::ProcessEvents()
    {
        PROFILE_FUCTION();
        glfwPollEvents();
        // detect the handle or other device
        Input::Update();
    }

    void WindowsWindow::SwapBuffers()
    {
        RenderHardware::PresentWindow(*this);
    }

    void WindowsWindow::SetVSync(bool enable)
    {
        PROFILE_FUCTION();

        RenderHardware::SetWindowVSync(*this, enable);

        m_Specification.VSync = enable;
    }

    bool WindowsWindow::IsVSync() const { return m_Specification.VSync; }

} // namespace Kans
