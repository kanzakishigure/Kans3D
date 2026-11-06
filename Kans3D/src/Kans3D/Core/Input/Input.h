#pragma once
#include "Kans3D/Core/Base/Base.h"
#include "KeyCodes.h"
namespace Kans
{

    class Input
    {
    public:
        static void Update();
        static bool IsKeyPressed(KeyCode KeyCode);

        static bool  IsMouseButtonPressed(MouseButton button);
        static float GetMouseX();
        static float GetMouseY();

        static void SetCursorMode(CursorMode mode);

        static std::pair<float, float> GetMousePosition();
    };

} // namespace Kans