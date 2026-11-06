#pragma once
#include <array>
#include <optional>
#include <string>
#include <glm/glm.hpp>
#include "ScriptInterop.h"

namespace Kans
{
    // The mouse as scripts see it this frame. Positions are game-view pixels (Y down, origin at the
    // view's top-left); Delta is the change since the previous frame; Scroll is this frame's wheel
    // (Y: vertical, X: horizontal); Buttons follow MouseButton (0 left, 1 right, 2 middle).
    struct ScriptMouseFrame
    {
        glm::vec2           Position {0}, Delta {0}, Scroll {0};
        std::array<bool, 8> Buttons {};
    };

    // Script input and requests that wait for the end of the frame. The play session (or a host)
    // feeds one mouse frame per update before scripts run.
    namespace ScriptInput
    {
        void                    SetMouse(const ScriptMouseFrame&);
        const ScriptMouseFrame& Mouse();
        // Clears the mouse frame and a pending scene load (play starts and stops).
        void Reset();
        // SceneManager.Load: the scene to switch to once this frame ends. A later request in the same
        // frame replaces an earlier one (the last call wins).
        void                       RequestSceneLoad(std::string path);
        std::optional<std::string> TakeSceneLoad();
        void                       Install(Scripting::NativeApi&);
    } // namespace ScriptInput
} // namespace Kans
