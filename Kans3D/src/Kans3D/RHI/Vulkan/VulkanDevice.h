#pragma once
#include <functional>
#include <string>
#include "Kans3D/RHI/RHI.h"

namespace Kans::RenderHardware
{
    // Backend-private construction policy. CPU-only selection is the safe default
    // for headless callers. Callers can opt into hardware and request validation.
    struct VulkanDeviceOptions
    {
        void*                                   NativeWindow              = nullptr;
        bool                                    HeadlessSurface           = false;
        bool                                    AsynchronousWindowFrames  = false;
        bool                                    RequireCpuDevice          = true;
        bool                                    Validation                = false;
        bool                                    SynchronizationValidation = false;
        std::function<void(const std::string&)> ValidationError;
    };
    std::unique_ptr<Device> CreateVulkanDevice(const DeviceDesc& desc = {});
    std::unique_ptr<Device> CreateVulkanDevice(const DeviceDesc& desc, const VulkanDeviceOptions& options);
} // namespace Kans::RenderHardware
