#pragma once
#include "Kans3D/Core/Window.h"

#include "Kans3D/Core/Events/ApplicationEvent.h"
#include "Kans3D/Core/Events/KeyEvent.h"
#include "Kans3D/Core/Events/MouseEvent.h"

#include <memory>
#include <optional>
#include <GLFW/glfw3.h>

namespace Kans
{
    void GLFWErrorCallback(int error, const char* description);

    class Win32WindowFrame;
    class WindowsWindow : public Window
    {
    public:
        WindowsWindow(const WindowSpecification& props);
        virtual ~WindowsWindow();

        virtual void Init() override;
        virtual void ProcessEvents() override;
        virtual void SwapBuffers() override;

        WindowSize      GetSize() const override;
        void            SetSize(int width, int height) override;
        WindowPosition  GetPosition() const override;
        void            SetPosition(int x, int y) override;
        void            CenterOnMonitor() override;
        void            Maximize() override;
        void            Minimize() override;
        void            Restore() override;
        bool            IsMaximized() const override;
        bool            IsMinimized() const override;
        void            SetMinimumSize(int width, int height) override;
        WindowPlacement GetPlacement() const override;
        void            SetPlacement(const WindowPlacement& placement) override;
        void            SetTitlebarHitTest(std::function<TitlebarHit(int, int)> callback) override
        {
            m_TitlebarHitTest = std::move(callback);
        }
        TitlebarButtonState GetTitlebarButtonState() const override;
        bool                HasCustomTitlebar() const override;
        bool                HasNativeWindowEdge() const override;
        void                SetWindowEdgeColor(float r, float g, float b) override;

        inline unsigned int GetWidth() const override { return m_Specification.Width; }
        inline unsigned int GetHeight() const override { return m_Specification.Height; }

        virtual const WindowSpecification& GetWindowSpecification() const override { return m_Specification; }
        virtual const void SetWindowSpecification(const WindowSpecification& spec) override { m_Specification = spec; }
        // 结构
        inline void          SetEventCallback(const EventCallbackFn& callback) override { m_EventCallback = callback; }
        void                 SetVSync(bool enable) override;
        bool                 IsVSync() const override;
        virtual inline void* GetNativeWindow() const override { return m_Window; }
        void                 SetTitle(const std::string& title) override
        {
            m_Specification.Title = title;
            glfwSetWindowTitle(m_Window, title.c_str());
        }

    protected:
        static void windowSizeCallback(GLFWwindow* window, int width, int height)
        {
            WindowsWindow& data         = *(WindowsWindow*)glfwGetWindowUserPointer(window);
            data.m_Specification.Height = height;
            data.m_Specification.Width  = width;

            data.UpdateNormalRectangle();
            WindowResizeEvent event(width, height);
            if (data.m_EventCallback)
                data.m_EventCallback(event);
        }
        static void windowCloseCallback(GLFWwindow* window)
        {
            WindowsWindow&   data = *(WindowsWindow*)glfwGetWindowUserPointer(window);
            WindowCloseEvent event;
            data.m_EventCallback(event);
        }
        static void keyCallback(GLFWwindow* window, int key, int scancode, int action, int mods)
        {
            WindowsWindow& data        = *(WindowsWindow*)glfwGetWindowUserPointer(window);
            uint16_t       repeatCount = 0;
            switch (action)
            {
                case GLFW_PRESS: {
                    repeatCount = 0;
                    KeyPressedEvent event((KeyCode)key, repeatCount);
                    data.m_EventCallback(event);
                }
                break;
                case GLFW_REPEAT: {
                    repeatCount = 1;
                    KeyPressedEvent event((KeyCode)key, repeatCount);
                    data.m_EventCallback(event);
                }
                break;
                case GLFW_RELEASE: {
                    KeyReleasedEvent event((KeyCode)key);
                    data.m_EventCallback(event);
                }
                break;
            }
        }
        static void mouseButtonCallback(GLFWwindow* window, int button, int action, int mods)
        {
            WindowsWindow& data = *(WindowsWindow*)glfwGetWindowUserPointer(window);
            switch (action)
            {
                case GLFW_PRESS: {

                    MouseButtonPressedEvent event((MouseButton)button);
                    data.m_EventCallback(event);
                }
                break;
                case GLFW_RELEASE: {

                    MouseButtonReleasedEvent event((MouseButton)button);
                    data.m_EventCallback(event);
                }
                break;
            }
        }
        static void scrollCallback(GLFWwindow* window, double xoffset, double yoffset)
        {
            WindowsWindow&     data = *(WindowsWindow*)glfwGetWindowUserPointer(window);
            MouseScrolledEvent event((float)xoffset, (float)yoffset);
            data.m_EventCallback(event);
        }
        static void cursorPosCallback(GLFWwindow* window, double xpos, double ypos)
        {
            WindowsWindow& data = *(WindowsWindow*)glfwGetWindowUserPointer(window);
            MouseMoveEvent event((float)xpos, (float)ypos);
            data.m_EventCallback(event);
        }
        static void charCallback(GLFWwindow* window, unsigned int codepoint)
        {
            WindowsWindow& data = *(WindowsWindow*)glfwGetWindowUserPointer(window);
            KeyTypedEvent  event(static_cast<char32_t>(codepoint));
            data.m_EventCallback(event);
        };

    private:
        virtual void Shutdown();
        void         UpdateNormalRectangle();
        void         ApplyPlacement(const WindowPlacement& placement);

    private:
        GLFWwindow*         m_Window;
        WindowSpecification m_Specification;
        EventCallbackFn     m_EventCallback = [](Event&) {};

        WindowPlacement                      m_NormalPlacement;
        WindowSize                           m_MinimumSize {0, 0};
        bool                                 m_PlacementChanging = false;
        bool                                 m_WasMaximized      = false;
        bool                                 m_WaitingForRestore = false;
        std::optional<WindowPlacement>       m_PendingPlacement;
        std::function<TitlebarHit(int, int)> m_TitlebarHitTest;
#ifdef _WIN32
        std::unique_ptr<Win32WindowFrame> m_CustomFrame;
#endif
        static int s_GLFWWindowsCount;
    };
} // namespace Kans
