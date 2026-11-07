#pragma once
#include "Kans3D/Renderer/RenderView.h"

namespace Kans
{
    class Renderer;
    // Display-space overlay. Owns no geometry and never writes scene alpha/depth.
    class EditorGridPass
    {
    public:
        static void Record(Renderer&                            renderer,
                           RenderHardware::CommandList&         commands,
                           const RenderView&                    view,
                           const ViewGrid&                      grid,
                           const RenderHardware::RenderingDesc& target);
    };
} // namespace Kans
