#pragma once
#include <vulkan/vulkan.h>
#include <functional>
#include "Kans3D/RHI/RHI.h"
namespace Kans::RenderHardware::VulkanTestProbes
{
    struct Diagnostics
    {
        VkPhysicalDeviceType Type;
        uint32_t             ApiVersion;
        bool                 Validation, SynchronizationValidation;
        uint64_t             PipelineCreations, DescriptorAllocations, DescriptorPools, GroupCreationNanoseconds;
        uint64_t             WaitIdleCalls;
        // Monotonic native buffer allocation count, including buffers already retired.
        uint64_t BufferCreations;
        uint64_t PipelineAttempts;
        uint64_t GroupCreations;
        uint64_t CommandBufferAllocations;
    };
    using BarrierObserver =
        std::function<void(const std::vector<VkBufferMemoryBarrier2>&, const std::vector<VkImageMemoryBarrier2>&)>;
    void SetBarrierObserver(const Device& device, BarrierObserver observer);
    // Observes host Buffer::Write and recorded CommandList::UpdateBuffer payloads.
    using BufferWriteObserver = std::function<void(const BufferDesc&, size_t, const void*, size_t)>;
    void        SetBufferWriteObserver(const Device& device, BufferWriteObserver observer);
    Diagnostics Inspect(const Device& device);
#ifdef TRACY_ENABLE
    struct TracyDiagnostics
    {
        std::optional<int32_t> ContextId;
        uint64_t               PassZones   = 0;
        uint64_t               Collections = 0;
    };
    // Exercise native timestamps and query resets under validation without a Tracy server.
    void             ForceTracyRecording(const Device& device);
    TracyDiagnostics InspectTracy(const Device& device);
#endif
    VkPhysicalDeviceDriverProperties DriverProperties(const Device& device);
    void                             ReportDeviceOnce(const Device& device);
    // Test-only injection at the native pipeline creation boundary; empty disables it.
    void        SetPipelineFailure(const Device& device, std::string nameSubstring);
    void        EmitValidationError(const Device& device, const char* message);
} // namespace Kans::RenderHardware::VulkanTestProbes
