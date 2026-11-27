#pragma once
#ifdef TRACY_ENABLE
#include <string_view>
#include "RHI.h"

namespace Kans::RenderHardware
{
    void SubmitProfiled(Device& device, const Handle<CommandList>& commands, std::string_view name);
}
#endif
