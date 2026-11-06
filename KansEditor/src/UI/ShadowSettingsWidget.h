#pragma once
#include "Kans3D/Renderer/Lighting/ShadowSettings.h"

namespace Kans
{
    // Shadow rows for an open PropertyGrid. `directional` shows the cascade and sun
    // settings; otherwise the point light's source radius. Returns true when any
    // setting changed (the caller stores the validated result).
    bool DrawShadowSettings(ShadowSettings& settings, bool directional);
} // namespace Kans
