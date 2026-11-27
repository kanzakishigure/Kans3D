#include "kspch.h"
#include "VulkanPresentation.h"
#include <GLFW/glfw3.h>
#include <backends/imgui_impl_glfw.h>
#include "Kans3D/Core/Window.h"
#include "Kans3D/RHI/FrameCapture.h"
// Compile the pinned backend once here. Its viewport render-buffer structure is
// used with our WSI callbacks; its old example WSI code is not used.
#include <backends/imgui_impl_vulkan.cpp>

namespace Kans::RenderHardware
{
    namespace
    {
        using namespace VulkanDetail;
#ifndef KS_DIST
        void CountImGuiDraws(const Context& context, const ImDrawData& data)
        {
            if (!context.ProfileFrames)
                return;
            const int width  = int(data.DisplaySize.x * data.FramebufferScale.x);
            const int height = int(data.DisplaySize.y * data.FramebufferScale.y);
            if (width <= 0 || height <= 0)
                return;
            // Mirror the backend's clip decision. Application callbacks can encode independent work.
            for (const auto* list : data.CmdLists)
                for (const auto& command : list->CmdBuffer)
                {
                    if (command.UserCallback)
                        continue;
                    const auto clipLeft   = (command.ClipRect.x - data.DisplayPos.x) * data.FramebufferScale.x;
                    const auto clipTop    = (command.ClipRect.y - data.DisplayPos.y) * data.FramebufferScale.y;
                    const auto clipRight  = (command.ClipRect.z - data.DisplayPos.x) * data.FramebufferScale.x;
                    const auto clipBottom = (command.ClipRect.w - data.DisplayPos.y) * data.FramebufferScale.y;
                    if (std::min(float(width), clipRight) <= std::max(0.f, clipLeft) ||
                        std::min(float(height), clipBottom) <= std::max(0.f, clipTop))
                        continue;
                    ++context.FrameCounters.ImGuiDrawCalls;
                    context.FrameCounters.ImGuiTriangles += command.ElemCount / 3;
                }
        }
#endif
        struct Swapchain
        {
            const Context&     C;
            GLFWwindow*        Window;
            Kans::Window*      Source = nullptr;
            VkSurfaceKHR       Surface;
            bool               OwnSurface, VSync, Dirty = true, Ready = false;
            VkSwapchainKHR     Chain = VK_NULL_HANDLE;
            VkSurfaceFormatKHR Format {};
            VkExtent2D         Extent {};
            VkExtent2D         FallbackExtent {96, 72};
            VkRenderPass       Pass           = VK_NULL_HANDLE;
            VkPipeline         WindowPipeline = VK_NULL_HANDLE;
            struct Image
            {
                VkImage       Native;
                VkImageView   View        = VK_NULL_HANDLE;
                VkFramebuffer Framebuffer = VK_NULL_HANDLE;
                VkSemaphore   Complete    = VK_NULL_HANDLE;
            };
            struct Slot
            {
                VkCommandPool   Pool     = VK_NULL_HANDLE;
                VkCommandBuffer Command  = VK_NULL_HANDLE;
                VkFence         Fence    = VK_NULL_HANDLE;
                VkSemaphore     Acquired = VK_NULL_HANDLE;
            };
            std::vector<Image> Images;
            std::vector<Slot>  Slots;
            uint32_t           SlotIndex = 0, ImageIndex = 0;
            std::string        Capture;
            VkSemaphore        CaptureComplete   = VK_NULL_HANDLE;
            VkBuffer           CaptureBuffer     = VK_NULL_HANDLE;
            VmaAllocation      CaptureAllocation = VK_NULL_HANDLE;
            void*              CaptureMapped     = nullptr;
            Swapchain(const Context& context,
                      GLFWwindow*    window,
                      VkSurfaceKHR   surface,
                      bool           own,
                      bool           vsync,
                      uint32_t       slots,
                      Kans::Window*  source = nullptr) :
                C(context), Window(window), Source(source), Surface(surface), OwnSurface(own), VSync(vsync),
                Slots(slots)
            {
                try
                {
                    VkBool32 supported;
                    Check(vkGetPhysicalDeviceSurfaceSupportKHR(C.Physical, C.QueueFamily, Surface, &supported));
                    Require(supported, "Graphics queue cannot present this platform window");
                    uint32_t count;
                    Check(vkGetPhysicalDeviceSurfaceFormatsKHR(C.Physical, Surface, &count, nullptr));
                    std::vector<VkSurfaceFormatKHR> formats(count);
                    Check(vkGetPhysicalDeviceSurfaceFormatsKHR(C.Physical, Surface, &count, formats.data()));
                    for (auto native : {VK_FORMAT_B8G8R8A8_UNORM, VK_FORMAT_R8G8B8A8_UNORM})
                        for (auto format : formats)
                            if ((format.format == native || format.format == VK_FORMAT_UNDEFINED) &&
                                format.colorSpace == VK_COLOR_SPACE_SRGB_NONLINEAR_KHR && !Format.format)
                                Format = {native, format.colorSpace};
                    Require(Format.format != VK_FORMAT_UNDEFINED,
                            "Surface needs RGBA8/BGRA8 UNORM with sRGB nonlinear presentation to preserve editor UI "
                            "colours");
                    VkAttachmentDescription attachment {};
                    attachment.format         = Format.format;
                    attachment.samples        = VK_SAMPLE_COUNT_1_BIT;
                    attachment.loadOp         = VK_ATTACHMENT_LOAD_OP_CLEAR;
                    attachment.storeOp        = VK_ATTACHMENT_STORE_OP_STORE;
                    attachment.stencilLoadOp  = VK_ATTACHMENT_LOAD_OP_DONT_CARE;
                    attachment.stencilStoreOp = VK_ATTACHMENT_STORE_OP_DONT_CARE;
                    attachment.initialLayout  = VK_IMAGE_LAYOUT_UNDEFINED;
                    attachment.finalLayout    = VK_IMAGE_LAYOUT_COLOR_ATTACHMENT_OPTIMAL;
                    VkAttachmentReference reference {0, VK_IMAGE_LAYOUT_COLOR_ATTACHMENT_OPTIMAL};
                    VkSubpassDescription  subpass {};
                    subpass.pipelineBindPoint    = VK_PIPELINE_BIND_POINT_GRAPHICS;
                    subpass.colorAttachmentCount = 1;
                    subpass.pColorAttachments    = &reference;
                    VkSubpassDependency dependency {};
                    dependency.srcSubpass   = VK_SUBPASS_EXTERNAL;
                    dependency.dstSubpass   = 0;
                    dependency.srcStageMask = dependency.dstStageMask = VK_PIPELINE_STAGE_COLOR_ATTACHMENT_OUTPUT_BIT;
                    dependency.dstAccessMask                          = VK_ACCESS_COLOR_ATTACHMENT_WRITE_BIT;
                    VkRenderPassCreateInfo pass {VK_STRUCTURE_TYPE_RENDER_PASS_CREATE_INFO};
                    pass.attachmentCount = 1;
                    pass.pAttachments    = &attachment;
                    pass.subpassCount    = 1;
                    pass.pSubpasses      = &subpass;
                    pass.dependencyCount = 1;
                    pass.pDependencies   = &dependency;
                    Check(vkCreateRenderPass(C.Logical, &pass, nullptr, &Pass));
                    for (auto& slot : Slots)
                    {
                        VkCommandPoolCreateInfo pool {VK_STRUCTURE_TYPE_COMMAND_POOL_CREATE_INFO};
                        pool.queueFamilyIndex = C.QueueFamily;
                        Check(vkCreateCommandPool(C.Logical, &pool, nullptr, &slot.Pool));
                        VkCommandBufferAllocateInfo command {VK_STRUCTURE_TYPE_COMMAND_BUFFER_ALLOCATE_INFO};
                        command.commandPool        = slot.Pool;
                        command.level              = VK_COMMAND_BUFFER_LEVEL_PRIMARY;
                        command.commandBufferCount = 1;
                        Check(vkAllocateCommandBuffers(C.Logical, &command, &slot.Command));
                        VkFenceCreateInfo fence {VK_STRUCTURE_TYPE_FENCE_CREATE_INFO};
                        fence.flags = VK_FENCE_CREATE_SIGNALED_BIT;
                        Check(vkCreateFence(C.Logical, &fence, nullptr, &slot.Fence));
                        VkSemaphoreCreateInfo semaphore {VK_STRUCTURE_TYPE_SEMAPHORE_CREATE_INFO};
                        Check(vkCreateSemaphore(C.Logical, &semaphore, nullptr, &slot.Acquired));
                    }
                    Recreate();
                }
                catch (...)
                {
                    Destroy();
                    throw;
                }
            }
            void DestroyImages()
            {
                for (auto& image : Images)
                {
                    vkDestroyFramebuffer(C.Logical, image.Framebuffer, nullptr);
                    vkDestroyImageView(C.Logical, image.View, nullptr);
                    vkDestroySemaphore(C.Logical, image.Complete, nullptr);
                }
                Images.clear();
            }
            void DestroyCapture()
            {
                if (CaptureBuffer)
                {
                    VmaAllocationInfo info {};
                    vmaGetAllocationInfo(C.Allocator, CaptureAllocation, &info);
                    C.BufferAllocationBytes -= info.size;
                    KANS_PROFILE_FREE(CaptureAllocation, VmaProfilerPool);
                    vmaDestroyBuffer(C.Allocator, CaptureBuffer, CaptureAllocation);
                }
                CaptureBuffer     = VK_NULL_HANDLE;
                CaptureAllocation = VK_NULL_HANDLE;
                CaptureMapped     = nullptr;
            }
            void Destroy()
            {
                vkDeviceWaitIdle(C.Logical);
                DestroyCapture();
                DestroyImages();
                vkDestroySemaphore(C.Logical, CaptureComplete, nullptr);
                vkDestroySwapchainKHR(C.Logical, Chain, nullptr);
                vkDestroyPipeline(C.Logical, WindowPipeline, nullptr);
                vkDestroyRenderPass(C.Logical, Pass, nullptr);
                for (auto& slot : Slots)
                {
                    vkDestroySemaphore(C.Logical, slot.Acquired, nullptr);
                    vkDestroyFence(C.Logical, slot.Fence, nullptr);
                    vkDestroyCommandPool(C.Logical, slot.Pool, nullptr);
                }
                if (OwnSurface)
                    vkDestroySurfaceKHR(C.Instance, Surface, nullptr);
            }
            ~Swapchain() { Destroy(); }
            void Size(int& width, int& height)
            {
                if (Window)
                    glfwGetFramebufferSize(Window, &width, &height);
                else if (Source)
                {
                    width  = Source->GetWidth();
                    height = Source->GetHeight();
                }
                else
                {
                    width  = FallbackExtent.width;
                    height = FallbackExtent.height;
                }
            }
            bool Recreate()
            {
                int width, height;
                Size(width, height);
                if (width <= 0 || height <= 0)
                    return false;
                Check(vkDeviceWaitIdle(C.Logical));
                VkSurfaceCapabilitiesKHR capabilities;
                Check(vkGetPhysicalDeviceSurfaceCapabilitiesKHR(C.Physical, Surface, &capabilities));
                auto extent = capabilities.currentExtent;
                if (extent.width == UINT32_MAX)
                    extent = {
                        std::clamp(
                            uint32_t(width), capabilities.minImageExtent.width, capabilities.maxImageExtent.width),
                        std::clamp(
                            uint32_t(height), capabilities.minImageExtent.height, capabilities.maxImageExtent.height)};
                if (!extent.width || !extent.height)
                    return false;
                uint32_t count;
                Check(vkGetPhysicalDeviceSurfacePresentModesKHR(C.Physical, Surface, &count, nullptr));
                std::vector<VkPresentModeKHR> modes(count);
                Check(vkGetPhysicalDeviceSurfacePresentModesKHR(C.Physical, Surface, &count, modes.data()));
                VkPresentModeKHR mode = VK_PRESENT_MODE_FIFO_KHR;
                if (!VSync)
                    for (auto preferred : {VK_PRESENT_MODE_MAILBOX_KHR, VK_PRESENT_MODE_IMMEDIATE_KHR})
                        if (std::find(modes.begin(), modes.end(), preferred) != modes.end())
                        {
                            mode = preferred;
                            break;
                        }
                uint32_t imageCount = std::max(2u, capabilities.minImageCount + 1);
                if (capabilities.maxImageCount)
                    imageCount = std::min(imageCount, capabilities.maxImageCount);
                VkSwapchainCreateInfoKHR create {VK_STRUCTURE_TYPE_SWAPCHAIN_CREATE_INFO_KHR};
                create.surface          = Surface;
                create.minImageCount    = imageCount;
                create.imageFormat      = Format.format;
                create.imageColorSpace  = Format.colorSpace;
                create.imageExtent      = extent;
                create.imageArrayLayers = 1;
                create.imageUsage       = VK_IMAGE_USAGE_COLOR_ATTACHMENT_BIT;
                if (capabilities.supportedUsageFlags & VK_IMAGE_USAGE_TRANSFER_SRC_BIT)
                    create.imageUsage |= VK_IMAGE_USAGE_TRANSFER_SRC_BIT;
                create.imageSharingMode = VK_SHARING_MODE_EXCLUSIVE;
                create.preTransform     = capabilities.currentTransform;
                for (auto alpha : {VK_COMPOSITE_ALPHA_OPAQUE_BIT_KHR,
                                   VK_COMPOSITE_ALPHA_PRE_MULTIPLIED_BIT_KHR,
                                   VK_COMPOSITE_ALPHA_POST_MULTIPLIED_BIT_KHR,
                                   VK_COMPOSITE_ALPHA_INHERIT_BIT_KHR})
                    if (capabilities.supportedCompositeAlpha & alpha)
                    {
                        create.compositeAlpha = alpha;
                        break;
                    }
                create.presentMode  = mode;
                create.clipped      = VK_TRUE;
                create.oldSwapchain = Chain;
                VkSwapchainKHR replacement;
                Check(vkCreateSwapchainKHR(C.Logical, &create, nullptr, &replacement));
                DestroyImages();
                vkDestroySwapchainKHR(C.Logical, Chain, nullptr);
                Chain  = replacement;
                Extent = extent;
                Check(vkGetSwapchainImagesKHR(C.Logical, Chain, &count, nullptr));
                std::vector<VkImage> images(count);
                Check(vkGetSwapchainImagesKHR(C.Logical, Chain, &count, images.data()));
                Images.resize(count);
                for (uint32_t i = 0; i < count; ++i)
                {
                    auto& image  = Images[i];
                    image.Native = images[i];
                    VkImageViewCreateInfo view {VK_STRUCTURE_TYPE_IMAGE_VIEW_CREATE_INFO};
                    view.image            = image.Native;
                    view.viewType         = VK_IMAGE_VIEW_TYPE_2D;
                    view.format           = Format.format;
                    view.subresourceRange = {VK_IMAGE_ASPECT_COLOR_BIT, 0, 1, 0, 1};
                    Check(vkCreateImageView(C.Logical, &view, nullptr, &image.View));
                    VkFramebufferCreateInfo framebuffer {VK_STRUCTURE_TYPE_FRAMEBUFFER_CREATE_INFO};
                    framebuffer.renderPass      = Pass;
                    framebuffer.attachmentCount = 1;
                    framebuffer.pAttachments    = &image.View;
                    framebuffer.width           = extent.width;
                    framebuffer.height          = extent.height;
                    framebuffer.layers          = 1;
                    Check(vkCreateFramebuffer(C.Logical, &framebuffer, nullptr, &image.Framebuffer));
                    VkSemaphoreCreateInfo semaphore {VK_STRUCTURE_TYPE_SEMAPHORE_CREATE_INFO};
                    Check(vkCreateSemaphore(C.Logical, &semaphore, nullptr, &image.Complete));
                }
                Dirty     = false;
                SlotIndex = 0;
                return true;
            }
            void Barrier(VkCommandBuffer       command,
                         VkImageLayout         from,
                         VkImageLayout         to,
                         VkPipelineStageFlags2 srcStage,
                         VkAccessFlags2        srcAccess,
                         VkPipelineStageFlags2 dstStage,
                         VkAccessFlags2        dstAccess)
            {
                VkImageMemoryBarrier2 barrier {VK_STRUCTURE_TYPE_IMAGE_MEMORY_BARRIER_2};
                barrier.srcStageMask        = srcStage;
                barrier.srcAccessMask       = srcAccess;
                barrier.dstStageMask        = dstStage;
                barrier.dstAccessMask       = dstAccess;
                barrier.oldLayout           = from;
                barrier.newLayout           = to;
                barrier.srcQueueFamilyIndex = barrier.dstQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED;
                barrier.image                                             = Images[ImageIndex].Native;
                barrier.subresourceRange                                  = {VK_IMAGE_ASPECT_COLOR_BIT, 0, 1, 0, 1};
                VkDependencyInfo dependency {VK_STRUCTURE_TYPE_DEPENDENCY_INFO};
                dependency.imageMemoryBarrierCount = 1;
                dependency.pImageMemoryBarriers    = &barrier;
                vkCmdPipelineBarrier2(command, &dependency);
            }
            void Render(ImDrawData* data)
            {
                Require(!Ready, "Previous swapchain image was not presented");
                int width, height;
                Size(width, height);
                if (width <= 0 || height <= 0)
                    return;
                if (Extent.width != uint32_t(width) || Extent.height != uint32_t(height))
                    Dirty = true;
                if (Dirty && !Recreate())
                    return;
                auto& slot = Slots[SlotIndex];
                C.WaitFence(slot.Fence);
                const auto result = [&] {
                    KANS_PROFILE_SCOPE("Vulkan acquire image");
                    auto acquire = [&] {
                        return vkAcquireNextImageKHR(
                            C.Logical, Chain, UINT64_MAX, slot.Acquired, VK_NULL_HANDLE, &ImageIndex);
                    };
#ifndef KS_DIST
                    return C.MeasureWait(&DeviceFrameCounters::AcquireWaitNanoseconds, acquire);
#else
                    return acquire();
#endif
                }();
                if (result == VK_ERROR_OUT_OF_DATE_KHR)
                {
                    Dirty = true;
                    return;
                }
                if (result == VK_SUBOPTIMAL_KHR)
                    Dirty = true;
                else
                    Check(result);
                Check(vkResetCommandPool(C.Logical, slot.Pool, 0));
                VkCommandBufferBeginInfo begin {VK_STRUCTURE_TYPE_COMMAND_BUFFER_BEGIN_INFO};
                begin.flags = VK_COMMAND_BUFFER_USAGE_ONE_TIME_SUBMIT_BIT;
                Check(vkBeginCommandBuffer(slot.Command, &begin));
                VkClearValue clear {};
                clear.color.float32[3] = 1;
                VkRenderPassBeginInfo pass {VK_STRUCTURE_TYPE_RENDER_PASS_BEGIN_INFO};
                pass.renderPass        = Pass;
                pass.framebuffer       = Images[ImageIndex].Framebuffer;
                pass.renderArea.extent = Extent;
                pass.clearValueCount   = 1;
                pass.pClearValues      = &clear;
                vkCmdBeginRenderPass(slot.Command, &pass, VK_SUBPASS_CONTENTS_INLINE);
                struct UVScope
                {
                    Device&     Owner;
                    ImDrawData* Data;
                    UVScope(Device& device, ImDrawData* data) : Owner(device), Data(data)
                    {
                        FlipVulkanImGuiRenderTargetUVs(Owner, Data);
                    }
                    ~UVScope() { FlipVulkanImGuiRenderTargetUVs(Owner, Data); }
                };
                {
                    KANS_PROFILE_SCOPE("ImGui Vulkan command recording");
                    UVScope uv(*GetSharedDevice(), data);
                    ImGui_ImplVulkan_RenderDrawData(data, slot.Command, WindowPipeline);
                }
                vkCmdEndRenderPass(slot.Command);
                Barrier(slot.Command,
                        VK_IMAGE_LAYOUT_COLOR_ATTACHMENT_OPTIMAL,
                        VK_IMAGE_LAYOUT_PRESENT_SRC_KHR,
                        VK_PIPELINE_STAGE_2_COLOR_ATTACHMENT_OUTPUT_BIT,
                        VK_ACCESS_2_COLOR_ATTACHMENT_WRITE_BIT,
                        VK_PIPELINE_STAGE_2_NONE,
                        0);
                Check(vkEndCommandBuffer(slot.Command));
                Check(vkResetFences(C.Logical, 1, &slot.Fence));
                VkPipelineStageFlags waitStage = VK_PIPELINE_STAGE_COLOR_ATTACHMENT_OUTPUT_BIT;
                VkSubmitInfo         submit {VK_STRUCTURE_TYPE_SUBMIT_INFO};
                submit.waitSemaphoreCount   = 1;
                submit.pWaitSemaphores      = &slot.Acquired;
                submit.pWaitDstStageMask    = &waitStage;
                submit.commandBufferCount   = 1;
                submit.pCommandBuffers      = &slot.Command;
                submit.signalSemaphoreCount = 1;
                submit.pSignalSemaphores    = &Images[ImageIndex].Complete;
                {
                    KANS_PROFILE_SCOPE("Vulkan queue submit");
                    Check(vkQueueSubmit(C.Queue, 1, &submit, slot.Fence));
                }
                KANS_FRAME_STAT(CountImGuiDraws(C, *data);)
                Ready = true;
            }
            void CaptureImage()
            {
                auto& slot = Slots[SlotIndex];
                // Debug path only: rendering must finish before reusing this pool.
                C.WaitFence(slot.Fence);
                Check(vkResetCommandPool(C.Logical, slot.Pool, 0));
                VkCommandBufferBeginInfo begin {VK_STRUCTURE_TYPE_COMMAND_BUFFER_BEGIN_INFO};
                begin.flags = VK_COMMAND_BUFFER_USAGE_ONE_TIME_SUBMIT_BIT;
                Check(vkBeginCommandBuffer(slot.Command, &begin));
                VkSurfaceCapabilitiesKHR capabilities;
                Check(vkGetPhysicalDeviceSurfaceCapabilitiesKHR(C.Physical, Surface, &capabilities));
                Require(capabilities.supportedUsageFlags & VK_IMAGE_USAGE_TRANSFER_SRC_BIT,
                        "Surface does not support frame capture");
                DestroyCapture();
                VkBufferCreateInfo buffer {VK_STRUCTURE_TYPE_BUFFER_CREATE_INFO};
                buffer.size  = size_t(Extent.width) * Extent.height * 4;
                buffer.usage = VK_BUFFER_USAGE_TRANSFER_DST_BIT;
                VmaAllocationCreateInfo allocation {};
                allocation.usage = VMA_MEMORY_USAGE_AUTO_PREFER_HOST;
                allocation.flags = VMA_ALLOCATION_CREATE_MAPPED_BIT | VMA_ALLOCATION_CREATE_HOST_ACCESS_RANDOM_BIT;
                VmaAllocationInfo info {};
                Check(vmaCreateBuffer(C.Allocator, &buffer, &allocation, &CaptureBuffer, &CaptureAllocation, &info));
                KANS_PROFILE_ALLOC(CaptureAllocation, info.size, VmaProfilerPool);
                CaptureMapped = info.pMappedData;
                C.BufferAllocationBytes += info.size;
                Barrier(slot.Command,
                        VK_IMAGE_LAYOUT_PRESENT_SRC_KHR,
                        VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL,
                        VK_PIPELINE_STAGE_2_COLOR_ATTACHMENT_OUTPUT_BIT,
                        VK_ACCESS_2_COLOR_ATTACHMENT_WRITE_BIT,
                        VK_PIPELINE_STAGE_2_COPY_BIT,
                        VK_ACCESS_2_TRANSFER_READ_BIT);
                VkBufferImageCopy copy {};
                copy.imageSubresource = {VK_IMAGE_ASPECT_COLOR_BIT, 0, 0, 1};
                copy.imageExtent      = {Extent.width, Extent.height, 1};
                vkCmdCopyImageToBuffer(slot.Command,
                                       Images[ImageIndex].Native,
                                       VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL,
                                       CaptureBuffer,
                                       1,
                                       &copy);
                VkBufferMemoryBarrier2 host {VK_STRUCTURE_TYPE_BUFFER_MEMORY_BARRIER_2};
                host.srcStageMask        = VK_PIPELINE_STAGE_2_COPY_BIT;
                host.srcAccessMask       = VK_ACCESS_2_TRANSFER_WRITE_BIT;
                host.dstStageMask        = VK_PIPELINE_STAGE_2_HOST_BIT;
                host.dstAccessMask       = VK_ACCESS_2_HOST_READ_BIT;
                host.srcQueueFamilyIndex = host.dstQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED;
                host.buffer                                         = CaptureBuffer;
                host.size                                           = VK_WHOLE_SIZE;
                VkDependencyInfo dependency {VK_STRUCTURE_TYPE_DEPENDENCY_INFO};
                dependency.bufferMemoryBarrierCount = 1;
                dependency.pBufferMemoryBarriers    = &host;
                vkCmdPipelineBarrier2(slot.Command, &dependency);
                Barrier(slot.Command,
                        VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL,
                        VK_IMAGE_LAYOUT_PRESENT_SRC_KHR,
                        VK_PIPELINE_STAGE_2_COPY_BIT,
                        VK_ACCESS_2_TRANSFER_READ_BIT,
                        VK_PIPELINE_STAGE_2_NONE,
                        0);
                Check(vkEndCommandBuffer(slot.Command));
                VkSemaphoreCreateInfo semaphore {VK_STRUCTURE_TYPE_SEMAPHORE_CREATE_INFO};
                Check(vkCreateSemaphore(C.Logical, &semaphore, nullptr, &CaptureComplete));
                Check(vkResetFences(C.Logical, 1, &slot.Fence));
                VkPipelineStageFlags stage = VK_PIPELINE_STAGE_TRANSFER_BIT;
                VkSubmitInfo         submit {VK_STRUCTURE_TYPE_SUBMIT_INFO};
                submit.waitSemaphoreCount   = 1;
                submit.pWaitSemaphores      = &Images[ImageIndex].Complete;
                submit.pWaitDstStageMask    = &stage;
                submit.commandBufferCount   = 1;
                submit.pCommandBuffers      = &slot.Command;
                submit.signalSemaphoreCount = 1;
                submit.pSignalSemaphores    = &CaptureComplete;
                {
                    KANS_PROFILE_SCOPE("Vulkan queue submit");
                    Check(vkQueueSubmit(C.Queue, 1, &submit, slot.Fence));
                }
            }
            void Present()
            {
                if (!Ready)
                    return;
                if (!Capture.empty())
                    CaptureImage();
                VkPresentInfoKHR present {VK_STRUCTURE_TYPE_PRESENT_INFO_KHR};
                present.waitSemaphoreCount = 1;
                present.pWaitSemaphores    = CaptureComplete ? &CaptureComplete : &Images[ImageIndex].Complete;
                present.swapchainCount     = 1;
                present.pSwapchains        = &Chain;
                present.pImageIndices      = &ImageIndex;
                const auto result          = [&] {
                    KANS_PROFILE_SCOPE("Vulkan queue present");
#ifndef KS_DIST
                    return C.MeasureWait(&DeviceFrameCounters::PresentWaitNanoseconds,
                                         [&] { return vkQueuePresentKHR(C.Queue, &present); });
#else
                    return vkQueuePresentKHR(C.Queue, &present);
#endif
                }();
                if (result == VK_ERROR_OUT_OF_DATE_KHR || result == VK_SUBOPTIMAL_KHR)
                    Dirty = true;
                else
                    Check(result);
                if (CaptureBuffer)
                {
                    C.WaitFence(Slots[SlotIndex].Fence);
                    Check(vmaInvalidateAllocation(C.Allocator, CaptureAllocation, 0, VK_WHOLE_SIZE));
                    if (result == VK_SUCCESS || result == VK_SUBOPTIMAL_KHR)
                    {
                        WriteFrameCapture(Capture,
                                          Extent.width,
                                          Extent.height,
                                          static_cast<uint8_t*>(CaptureMapped),
                                          false,
                                          Format.format == VK_FORMAT_B8G8R8A8_UNORM);
                        Capture.clear();
                    }
                    Check(vkQueueWaitIdle(C.Queue)); // present has consumed the capture semaphore.
                    vkDestroySemaphore(C.Logical, CaptureComplete, nullptr);
                    CaptureComplete = VK_NULL_HANDLE;
                    DestroyCapture();
                }
                Ready     = false;
                SlotIndex = (SlotIndex + 1) % Slots.size();
            }
        };
        class OffscreenUI final : public ImGuiRenderer
        {
            std::shared_ptr<Device> Owner = GetSharedDevice();
            const Context&          C     = GetVulkanContext(*Owner);
            VkRenderPass            Pass  = VK_NULL_HANDLE;
            VkDescriptorPool        Pool  = VK_NULL_HANDLE;

        public:
            Handle<Texture> Target;
            OffscreenUI()
            {
                Require(!(ImGui::GetIO().ConfigFlags & ImGuiConfigFlags_ViewportsEnable),
                        "Headless ImGui cannot create platform windows");
                VkAttachmentDescription attachment {};
                attachment.format         = VK_FORMAT_R8G8B8A8_UNORM;
                attachment.samples        = VK_SAMPLE_COUNT_1_BIT;
                attachment.loadOp         = VK_ATTACHMENT_LOAD_OP_CLEAR;
                attachment.storeOp        = VK_ATTACHMENT_STORE_OP_STORE;
                attachment.stencilLoadOp  = VK_ATTACHMENT_LOAD_OP_DONT_CARE;
                attachment.stencilStoreOp = VK_ATTACHMENT_STORE_OP_DONT_CARE;
                attachment.initialLayout = attachment.finalLayout = VK_IMAGE_LAYOUT_COLOR_ATTACHMENT_OPTIMAL;
                VkAttachmentReference reference {0, VK_IMAGE_LAYOUT_COLOR_ATTACHMENT_OPTIMAL};
                VkSubpassDescription  subpass {};
                subpass.pipelineBindPoint    = VK_PIPELINE_BIND_POINT_GRAPHICS;
                subpass.colorAttachmentCount = 1;
                subpass.pColorAttachments    = &reference;
                VkRenderPassCreateInfo create {VK_STRUCTURE_TYPE_RENDER_PASS_CREATE_INFO};
                create.attachmentCount = 1;
                create.pAttachments    = &attachment;
                create.subpassCount    = 1;
                create.pSubpasses      = &subpass;
                Check(vkCreateRenderPass(C.Logical, &create, nullptr, &Pass));
                VkDescriptorPoolSize       sizes[] = {{VK_DESCRIPTOR_TYPE_SAMPLED_IMAGE, 16},
                                                      {VK_DESCRIPTOR_TYPE_SAMPLER, 16}};
                VkDescriptorPoolCreateInfo pool {VK_STRUCTURE_TYPE_DESCRIPTOR_POOL_CREATE_INFO};
                pool.flags         = VK_DESCRIPTOR_POOL_CREATE_FREE_DESCRIPTOR_SET_BIT;
                pool.maxSets       = 32;
                pool.poolSizeCount = 2;
                pool.pPoolSizes    = sizes;
                Check(vkCreateDescriptorPool(C.Logical, &pool, nullptr, &Pool));
                ImGui_ImplVulkan_InitInfo info {};
                info.ApiVersion     = VK_API_VERSION_1_3;
                info.Instance       = C.Instance;
                info.PhysicalDevice = C.Physical;
                info.Device         = C.Logical;
                info.QueueFamily    = C.QueueFamily;
                info.Queue          = C.Queue;
                info.DescriptorPool = Pool;
                info.MinImageCount = info.ImageCount = 2;
                info.PipelineInfoMain.MSAASamples    = VK_SAMPLE_COUNT_1_BIT;
                info.CheckVkResultFn                 = Check;
                info.PipelineInfoMain.RenderPass     = Pass;
                Require(ImGui_ImplVulkan_Init(&info), "Headless ImGui initialization failed");
                // The backend uploads dynamic font atlas texture requests during rendering.
            }
            ~OffscreenUI() override
            {
                Owner->WaitIdle();
                EndImGuiTextures();
                Target.reset();
                Owner->WaitIdle();
                ImGui_ImplVulkan_Shutdown();
                vkDestroyDescriptorPool(C.Logical, Pool, nullptr);
                vkDestroyRenderPass(C.Logical, Pass, nullptr);
            }
            void NewFrame() override
            {
                BeginImGuiTextures();
                ImGui_ImplVulkan_NewFrame();
            }
            void Render(ImDrawData* data) override
            {
                PrepareVulkanImGuiTextures(*Owner, data);
                Target = RenderVulkanImGuiOffscreen(*Owner, data, Pass);
                KANS_FRAME_STAT(CountImGuiDraws(C, *data);)
            }
            void RenderPlatformWindows() override {}
            void EndFrame() override
            {
                Owner->WaitIdle();
                EndImGuiTextures();
                Owner->WaitIdle();
            }
        };
        class VulkanUI;
        std::unordered_map<Window*, VulkanUI*> Renderers;
        class VulkanUI final : public ImGuiRenderer
        {
            std::shared_ptr<Device>                                        DeviceOwner;
            Window*                                                        MainWindow;
            const Context&                                                 C;
            Swapchain                                                      Main;
            VkDescriptorPool                                               FontPool = VK_NULL_HANDLE;
            std::unordered_map<ImGuiViewport*, std::unique_ptr<Swapchain>> Platform;
            inline static VulkanUI*                                        Current = nullptr;
            static void                                                    CreatePlatform(ImGuiViewport* viewport)
            {
                auto&        self = *Current;
                VkSurfaceKHR surface;
                GLFWwindow*  native = static_cast<GLFWwindow*>(viewport->PlatformHandle);
                if (self.C.Options.HeadlessSurface)
                {
                    native      = nullptr;
                    auto create = reinterpret_cast<PFN_vkCreateHeadlessSurfaceEXT>(
                        vkGetInstanceProcAddr(self.C.Instance, "vkCreateHeadlessSurfaceEXT"));
                    VkHeadlessSurfaceCreateInfoEXT info {VK_STRUCTURE_TYPE_HEADLESS_SURFACE_CREATE_INFO_EXT};
                    Check(create(self.C.Instance, &info, nullptr, &surface));
                }
                else
                    Check(glfwCreateWindowSurface(self.C.Instance, native, nullptr, &surface));
                auto chain = std::make_unique<Swapchain>(
                    self.C, native, surface, true, self.Main.VSync, self.DeviceOwner->GetDesc().FramesInFlight);
                chain->FallbackExtent = {uint32_t(viewport->Size.x), uint32_t(viewport->Size.y)};
                chain->Dirty          = true;
                ImGui_ImplVulkan_PipelineInfo pipeline {};
                pipeline.RenderPass  = chain->Pass;
                pipeline.MSAASamples = VK_SAMPLE_COUNT_1_BIT;
                chain->WindowPipeline =
                    ImGui_ImplVulkan_CreatePipeline(self.C.Logical, nullptr, VK_NULL_HANDLE, &pipeline);
                viewport->RendererUserData = IM_NEW(ImGui_ImplVulkan_ViewportData)();
                self.Platform.emplace(viewport, std::move(chain));
            }
            static void DestroyPlatform(ImGuiViewport* viewport)
            {
                Current->DeviceOwner->WaitIdle();
                Current->Platform.erase(viewport);
                if (auto* data = static_cast<ImGui_ImplVulkan_ViewportData*>(viewport->RendererUserData))
                {
                    ImGui_ImplVulkan_DestroyWindowRenderBuffers(Current->C.Logical, &data->RenderBuffers, nullptr);
                    IM_DELETE(data);
                    viewport->RendererUserData = nullptr;
                }
            }
            static void ResizePlatform(ImGuiViewport* viewport, ImVec2 size)
            {
                auto& chain          = *Current->Platform.at(viewport);
                chain.FallbackExtent = {uint32_t(std::max(0.0f, size.x)), uint32_t(std::max(0.0f, size.y))};
                chain.Dirty          = true;
            }
            static void RenderPlatform(ImGuiViewport* viewport, void*)
            {
                PrepareVulkanImGuiTextures(*Current->DeviceOwner, viewport->DrawData);
                Current->Platform.at(viewport)->Render(viewport->DrawData);
            }
            static void PresentPlatform(ImGuiViewport* viewport, void*) { Current->Platform.at(viewport)->Present(); }

        public:
            explicit VulkanUI(Window* window) :
                DeviceOwner(GetSharedDevice()), MainWindow(window), C(GetVulkanContext(*DeviceOwner)),
                Main(C,
                     static_cast<GLFWwindow*>(window->GetNativeWindow()),
                     C.Surface,
                     false,
                     window->IsVSync(),
                     DeviceOwner->GetDesc().FramesInFlight,
                     window)
            {
                Require(!Current, "Only one Vulkan ImGui renderer may be active");
                VkDescriptorPoolSize       sizes[] = {{VK_DESCRIPTOR_TYPE_SAMPLED_IMAGE, 16},
                                                      {VK_DESCRIPTOR_TYPE_SAMPLER, 16}};
                VkDescriptorPoolCreateInfo pool {VK_STRUCTURE_TYPE_DESCRIPTOR_POOL_CREATE_INFO};
                pool.flags         = VK_DESCRIPTOR_POOL_CREATE_FREE_DESCRIPTOR_SET_BIT;
                pool.maxSets       = 32;
                pool.poolSizeCount = 2;
                pool.pPoolSizes    = sizes;
                Check(vkCreateDescriptorPool(C.Logical, &pool, nullptr, &FontPool));
                if (window->GetNativeWindow())
                    Require(ImGui_ImplGlfw_InitForVulkan(static_cast<GLFWwindow*>(window->GetNativeWindow()), true),
                            "ImGui GLFW Vulkan initialization failed");
                ImGui_ImplVulkan_InitInfo info {};
                info.ApiVersion                   = VK_API_VERSION_1_3;
                info.Instance        = C.Instance;
                info.PhysicalDevice  = C.Physical;
                info.Device          = C.Logical;
                info.QueueFamily     = C.QueueFamily;
                info.Queue           = C.Queue;
                info.DescriptorPool  = FontPool;
                info.MinImageCount   = 2;
                info.ImageCount      = std::max(2u, DeviceOwner->GetDesc().FramesInFlight);
                info.PipelineInfoMain.MSAASamples = VK_SAMPLE_COUNT_1_BIT;
                info.CheckVkResultFn = Check;
                info.PipelineInfoMain.RenderPass  = Main.Pass;
                Require(ImGui_ImplVulkan_Init(&info), "ImGui Vulkan initialization failed");
                Current                         = this;
                auto& platform                  = ImGui::GetPlatformIO();
                platform.Renderer_CreateWindow  = CreatePlatform;
                platform.Renderer_DestroyWindow = DestroyPlatform;
                platform.Renderer_SetWindowSize = ResizePlatform;
                platform.Renderer_RenderWindow  = RenderPlatform;
                platform.Renderer_SwapBuffers   = PresentPlatform;
                Renderers.emplace(window, this);
            }
            ~VulkanUI() override
            {
                DeviceOwner->WaitIdle();
                EndImGuiTextures();
                ImGui::DestroyPlatformWindows();
                ImGui_ImplVulkan_Shutdown();
                if (MainWindow->GetNativeWindow())
                    ImGui_ImplGlfw_Shutdown();
                vkDestroyDescriptorPool(C.Logical, FontPool, nullptr);
                auto& platform                  = ImGui::GetPlatformIO();
                platform.Renderer_CreateWindow  = nullptr;
                platform.Renderer_DestroyWindow = nullptr;
                platform.Renderer_SetWindowSize = nullptr;
                platform.Renderer_RenderWindow  = nullptr;
                platform.Renderer_SwapBuffers   = nullptr;
                Renderers.erase(MainWindow);
                Current = nullptr;
            }
            void NewFrame() override
            {
                BeginImGuiTextures();
                ImGui_ImplVulkan_NewFrame();
                if (MainWindow->GetNativeWindow())
                    ImGui_ImplGlfw_NewFrame();
            }
            void Render(ImDrawData* data) override
            {
                PrepareVulkanImGuiTextures(*DeviceOwner, data);
                Main.Render(data);
            }
            void RenderPlatformWindows() override
            {
                ImGui::UpdatePlatformWindows();
                ImGui::RenderPlatformWindowsDefault();
            }
            // Retention ends at main-window present, after all platform draws.
            void EndFrame() override {}
            void Present() { Main.Present(); }
            void VSync(bool value)
            {
                Main.VSync = value;
                Main.Dirty = true;
                for (auto& entry : Platform)
                {
                    entry.second->VSync = value;
                    entry.second->Dirty = true;
                }
            }
            void Capture(const std::string& path) { Main.Capture = path; }
        };
    } // namespace
    void* CreateVulkanWindow(const WindowSpecification& specification)
    {
        glfwWindowHint(GLFW_CLIENT_API, GLFW_NO_API);
        auto* monitor = specification.Fullscreen ? glfwGetPrimaryMonitor() : nullptr;
        auto* mode    = monitor ? glfwGetVideoMode(monitor) : nullptr;
        auto* window  = glfwCreateWindow(mode ? mode->width : specification.Width,
                                         mode ? mode->height : specification.Height,
                                         specification.Title.c_str(),
                                         monitor,
                                         nullptr);
        if (!window)
            throw std::runtime_error("Cannot create GLFW Vulkan window");
        return window;
    }
    std::unique_ptr<ImGuiRenderer> CreateVulkanImGuiRenderer(Window* window)
    {
        if (!window)
            return std::make_unique<OffscreenUI>();
        return std::make_unique<VulkanUI>(window);
    }
    Handle<Texture> GetVulkanImGuiOffscreenTarget(ImGuiRenderer& renderer)
    {
        return dynamic_cast<OffscreenUI&>(renderer).Target;
    }
    void BeginVulkanWindowFrame(Device& device, Window&) { BeginVulkanDeviceWindowFrame(device); }
    void PresentVulkanWindow(Device& device, Window& window)
    {
        auto found = Renderers.find(&window);
        Require(found != Renderers.end(), "Vulkan presentation requires an ImGui renderer");
        found->second->Present();
        FinishVulkanDeviceWindowFrame(device);
    }
    void SetVulkanWindowVSync(Device&, Window& window, bool enabled)
    {
        auto found = Renderers.find(&window);
        if (found != Renderers.end())
            found->second->VSync(enabled);
    }
    void RequestVulkanFrameCapture(Device&, Window& window, const std::string& path)
    {
        auto found = Renderers.find(&window);
        Require(found != Renderers.end(), "Frame capture requires a Vulkan UI renderer");
        found->second->Capture(path);
    }
} // namespace Kans::RenderHardware
