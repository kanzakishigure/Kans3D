#include "kspch.h"
#include "Input.h"

#include <GLFW/glfw3.h>
#include "Kans3D/Core/Application.h"
namespace Kans
{

    void Input::Update() {}
    bool Input::IsKeyPressed(KeyCode keycode)
    {
        auto window = static_cast<GLFWwindow*>(Application::Get().GetWindow().GetNativeWindow());
        auto state  = glfwGetKey(window, (int)keycode);
        return state == GLFW_PRESS || state == GLFW_REPEAT;
    }

    bool Input::IsMouseButtonPressed(MouseButton button)
    {
        auto window = static_cast<GLFWwindow*>(Application::Get().GetWindow().GetNativeWindow());
        auto state  = glfwGetMouseButton(window, (int)button);
        return state == GLFW_PRESS || state == GLFW_REPEAT;
    }

    float Input::GetMouseX()
    {
        auto [X, Y] = GetMousePosition();
        return (float)X;
    }
    float Input::GetMouseY()
    {
        auto [X, Y] = GetMousePosition();
        return (float)Y;
    }

    void Input::SetCursorMode(CursorMode mode)
    {
        auto window = static_cast<GLFWwindow*>(Application::Get().GetWindow().GetNativeWindow());
        glfwSetInputMode(window, GLFW_CURSOR, GLFW_CURSOR_NORMAL + (uint16_t)mode);
    }

    // c++17新特性std::pair 返回一对数值，使用{}
    std::pair<float, float> Input::GetMousePosition()
    {
        auto   window = static_cast<GLFWwindow*>(Application::Get().GetWindow().GetNativeWindow());
        double Xpos, Ypos;
        glfwGetCursorPos(window, &Xpos, &Ypos);
        return {(float)Xpos, (float)Ypos};
    }

} // namespace Kans