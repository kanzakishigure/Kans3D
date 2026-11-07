#pragma once
#include "Kans3D/Core/Events/Event.h"
#include "Kans3D/Scene/Scene.h"
namespace Kans
{
    class EditorPanel
    {
    public:
        virtual ~EditorPanel() = default;

        // Draws the panel contents inside the window the editor opened for it.
        virtual void onImGuiRender() = 0;

        virtual void onEvent(Event& e) {}
        virtual void onProjectChanged() {}
        virtual void setSceneContext(const Ref<Scene>& context) {}
    };
} // namespace Kans