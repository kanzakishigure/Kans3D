#pragma once
#include "Kans3D/Core/Events/Event.h"
#include "Kans3D/Core/Input/KeyCodes.h"

namespace Kans
{
    class MouseMoveEvent : public Event
    {
    public:
        MouseMoveEvent(const float x, const float y) : m_MouseX(x), m_MouseY(y) {}
        float GetX() { return m_MouseX; }
        float GetY() { return m_MouseY; }

        std::string ToString() const override
        {
            std::stringstream ss;
            ss << "MouseMoveEvent:" << "X:" << m_MouseX << "," << "Y:" << m_MouseY;
            return ss.str();
        }
        EVENT_CLASS_TYPE(MouseMoved)
        EVENT_CLASS_CATEGORY(EventCategoryInput | EventCategoryMouse)
    private:
        float m_MouseX;
        float m_MouseY;
    };
    class MouseScrolledEvent : public Event
    {
    public:
        MouseScrolledEvent(const float xoffset, const float yoffset) : m_Xoffset(xoffset), m_Yoffset(yoffset) {}

        float       GetXOffset() const { return m_Xoffset; }
        float       GetYOffset() const { return m_Yoffset; }
        std::string ToString() const override
        {
            std::stringstream ss;
            ss << "MouseScrolledEvent: " << GetXOffset() << ", " << GetYOffset();
            return ss.str();
        }

        EVENT_CLASS_TYPE(MouseScrolled)
        EVENT_CLASS_CATEGORY(EventCategoryInput | EventCategoryMouse)
    private:
        float m_Xoffset, m_Yoffset;
    };

    class MouseButtonEvent : public Event
    {
    public:
        MouseButton GetMouseButton() const { return m_Button; }
        EVENT_CLASS_CATEGORY(EventCategoryInput | EventCategoryMouse | EventCategoryMouseButton)
    protected:
        MouseButtonEvent(const MouseButton button) : m_Button(button) {}
        MouseButton m_Button;
    };

    class MouseButtonPressedEvent : public MouseButtonEvent
    {
    public:
        MouseButtonPressedEvent(const MouseButton button) : MouseButtonEvent(button) {}

        std::string ToString() const override
        {
            std::stringstream ss;
            ss << "MouseButtonPressedEvent: " << m_Button;
            return ss.str();
        }
        EVENT_CLASS_TYPE(MouseButtonPressed)
    };
    class MouseButtonReleasedEvent : public MouseButtonEvent
    {
    public:
        MouseButtonReleasedEvent(const MouseButton button) : MouseButtonEvent(button) {}

        std::string ToString() const override
        {
            std::stringstream ss;
            ss << "MouseButtonReleasedEvent: " << m_Button;
            return ss.str();
        }

        EVENT_CLASS_TYPE(MouseButtonReleased)
    };

} // namespace Kans