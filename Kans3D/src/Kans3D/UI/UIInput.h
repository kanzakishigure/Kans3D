#pragma once
#include <array>
#include <set>
#include <string>
#include <vector>
#include <glm/glm.hpp>
#include "Kans3D/Core/Input/KeyCodes.h"
namespace Kans
{
    class Event;
    // Application coordinates are Y-down. Scale converts viewport units to rendered pixels (e.g. DPI).
    struct UIViewport
    {
        glm::vec2 Origin {0}, Size {1}, Scale {1};
        glm::vec2 Pixels() const { return Size * Scale; }
        glm::vec2 Map(glm::vec2 point) const { return (point - Origin) * Scale; }
        bool      Contains(glm::vec2 point) const
        {
            auto p = Map(point);
            return p.x >= 0 && p.y >= 0 && p.x < Pixels().x && p.y < Pixels().y;
        }
    };
    struct UIInputState
    {
        glm::vec2 Pointer {0}, Scroll {0}; // Pointer is already in game-view pixels unless Process gets a viewport.
        bool      PointerInside = true;
        std::array<bool, 8> ButtonDown {}, ButtonUp {}, Buttons {};
        std::set<KeyCode>   KeyDown, KeyUp, Keys;
        std::u32string      Typed;
        bool                Shift = false, Control = false, Alt = false, Super = false;
        // Applications can fill these from a gamepad. Keyboard maps to the same intents in UIContext.
        bool Up = false, Down = false, Left = false, Right = false, Submit = false, Cancel = false;
        void Feed(Event&);
        // Keep held state and modifiers while clearing per-frame edges/deltas/intents.
        void NextFrame();
    };
} // namespace Kans
