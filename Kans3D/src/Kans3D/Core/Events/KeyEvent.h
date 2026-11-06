#pragma once

#include "Kans3D/Core/Events/Event.h"
#include "Kans3D/Core/Input/KeyCodes.h"
namespace Kans
{

    // KeyEvent的基类，用于后续KeyEvent的模板
    class KeyEvent : public Event
    {
    public:
        KeyCode GetKeyCode() const { return m_KeyCode; }

        EVENT_CLASS_CATEGORY(EventCategoryKeyboard | EventCategoryInput)

    protected:
        KeyEvent(const KeyCode keycode) : m_KeyCode(keycode) {}
        KeyCode m_KeyCode;
    };
    class KeyPressedEvent : public KeyEvent
    {
    public:
        // 子类通过调用父类构造函数，对从父类继承的值进行赋值
        KeyPressedEvent(const KeyCode keycode, const uint16_t repeatCount) :
            KeyEvent(keycode), m_RepeatCount(repeatCount)
        {}

        uint16_t    GetRepeatCount() const { return m_RepeatCount; }
        std::string ToString() const override
        {
            std::stringstream ss;
            ss << "KeyPressedEvent: " << m_KeyCode << " (" << m_RepeatCount << " repeats)";
            return ss.str();
        }
        EVENT_CLASS_TYPE(KeyPressed)
    private:
        uint16_t m_RepeatCount;
    };
    class KeyReleasedEvent : public KeyEvent
    {
    public:
        KeyReleasedEvent(KeyCode keycode) : KeyEvent(keycode) {}

        std::string ToString() const override
        {
            std::stringstream ss;
            ss << "KeyReleasedEvent: " << m_KeyCode;
            return ss.str();
        }
        EVENT_CLASS_TYPE(KeyReleased)
    };
    class KeyTypedEvent : public KeyEvent
    {
    public:
        KeyTypedEvent(char32_t codepoint) : KeyEvent(static_cast<KeyCode>(codepoint)), m_Codepoint(codepoint) {}
        KeyTypedEvent(KeyCode codepoint) : KeyTypedEvent(static_cast<char32_t>(codepoint)) {}
        char32_t GetCodepoint() const { return m_Codepoint; }

        std::string ToString() const override
        {
            std::stringstream ss;
            ss << "KeyTypedEvent: " << static_cast<uint32_t>(m_Codepoint);
            return ss.str();
        }
        EVENT_CLASS_TYPE(KeyTyped)
    private:
        char32_t m_Codepoint;
    };

} // namespace Kans