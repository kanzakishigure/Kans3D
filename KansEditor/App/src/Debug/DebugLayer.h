#pragma once
#include <Kans3D.h>
namespace Kans
{
    class DebugLayer : public Layer
    {
    public:
        DebugLayer() {}
        virtual void OnAttach() override;
        virtual void OnDetach() override;
        virtual void OnUpdate(TimeStep ts) override;
        virtual void OnImGuiRender() override;
        virtual void OnEvent(Event& event) override;

    private:
    };
} // namespace Kans