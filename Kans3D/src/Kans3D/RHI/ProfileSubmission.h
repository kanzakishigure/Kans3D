#pragma once
#ifndef KS_DIST
#include "RHI.h"

namespace Kans::RenderHardware
{
    void SubmitProfiled(Device& device, const Handle<CommandList>& commands, const std::string& name);
}
#endif
