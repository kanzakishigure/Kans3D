#include "kspch.h"
// GLFW exposes its surface API only when Vulkan declarations are present.
#define GLFW_INCLUDE_VULKAN
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <GLFW/glfw3.h>
#include "VulkanInternal.h"

namespace Kans::RenderHardware::VulkanDetail
{
    bool ResolveCpuRequirement(bool requested, const char* forceCpu)
    {
        return requested || (forceCpu && std::strcmp(forceCpu, "1") == 0);
    }
    bool AcceptDeviceType(VkPhysicalDeviceType type, bool requireCpu)
    {
        return !requireCpu || type == VK_PHYSICAL_DEVICE_TYPE_CPU;
    }
    namespace
    {
        VKAPI_ATTR VkBool32 VKAPI_CALL Debug(VkDebugUtilsMessageSeverityFlagBitsEXT severity,
                                             VkDebugUtilsMessageTypeFlagsEXT,
                                             const VkDebugUtilsMessengerCallbackDataEXT* data,
                                             void*                                       user)
        {
            if (severity & VK_DEBUG_UTILS_MESSAGE_SEVERITY_ERROR_BIT_EXT)
            {
                auto& options = *static_cast<VulkanDeviceOptions*>(user);
                if (options.ValidationError)
                    options.ValidationError(data->pMessage);
                else
                    std::fprintf(stderr, "Vulkan validation error: %s\n", data->pMessage);
            }
            return VK_FALSE;
        }
    } // namespace
    Context::Context(const VulkanDeviceOptions& options) : Options(options)
    {
        const char* forceCpu     = std::getenv("KANS_VULKAN_REQUIRE_CPU");
        Options.RequireCpuDevice = ResolveCpuRequirement(Options.RequireCpuDevice, forceCpu);
        try
        {
            VkApplicationInfo app {VK_STRUCTURE_TYPE_APPLICATION_INFO};
            app.pApplicationName     = "Kans3D headless RHI";
            app.pEngineName          = "Kans3D";
            app.apiVersion           = VK_API_VERSION_1_3;
            const char* layer        = "VK_LAYER_KHRONOS_validation";
            std::vector<const char*> extensions;
            if (Options.NativeWindow)
            {
                uint32_t glfwCount      = 0;
                auto     glfwExtensions = glfwGetRequiredInstanceExtensions(&glfwCount);
                Require(glfwExtensions && glfwCount, "GLFW Vulkan surface extensions unavailable");
                extensions.assign(glfwExtensions, glfwExtensions + glfwCount);
            }
            if (Options.HeadlessSurface)
            {
                extensions.push_back(VK_KHR_SURFACE_EXTENSION_NAME);
                extensions.push_back(VK_EXT_HEADLESS_SURFACE_EXTENSION_NAME);
            }
            if (Options.Validation)
                extensions.push_back(VK_EXT_DEBUG_UTILS_EXTENSION_NAME);
            if (Options.Validation && Options.SynchronizationValidation)
                extensions.push_back(VK_EXT_VALIDATION_FEATURES_EXTENSION_NAME);
            VkDebugUtilsMessengerCreateInfoEXT debug {VK_STRUCTURE_TYPE_DEBUG_UTILS_MESSENGER_CREATE_INFO_EXT};
            debug.messageSeverity                        = VK_DEBUG_UTILS_MESSAGE_SEVERITY_ERROR_BIT_EXT;
            debug.messageType                            = VK_DEBUG_UTILS_MESSAGE_TYPE_GENERAL_BIT_EXT |
                                                           VK_DEBUG_UTILS_MESSAGE_TYPE_VALIDATION_BIT_EXT |
                                                           VK_DEBUG_UTILS_MESSAGE_TYPE_PERFORMANCE_BIT_EXT;
            debug.pfnUserCallback                        = Debug;
            debug.pUserData                              = &Options;
            VkValidationFeatureEnableEXT synchronization = VK_VALIDATION_FEATURE_ENABLE_SYNCHRONIZATION_VALIDATION_EXT;
            VkValidationFeaturesEXT      features {VK_STRUCTURE_TYPE_VALIDATION_FEATURES_EXT};
            features.enabledValidationFeatureCount = Options.SynchronizationValidation ? 1 : 0;
            features.pEnabledValidationFeatures    = &synchronization;
            features.pNext                         = &debug;
            VkInstanceCreateInfo instance {VK_STRUCTURE_TYPE_INSTANCE_CREATE_INFO};
            instance.pApplicationInfo = &app;
            if (Options.Validation)
            {
                uint32_t count = 0;
                Check(vkEnumerateInstanceLayerProperties(&count, nullptr));
                std::vector<VkLayerProperties> layers(count);
                Check(vkEnumerateInstanceLayerProperties(&count, layers.data()));
                Require(std::any_of(layers.begin(),
                                    layers.end(),
                                    [layer](const auto& p) { return std::strcmp(p.layerName, layer) == 0; }),
                        "Vulkan validation layer is required");
                instance.enabledLayerCount       = 1;
                instance.ppEnabledLayerNames     = &layer;

                instance.pNext = Options.SynchronizationValidation ? static_cast<const void*>(&features) :
                                                                     static_cast<const void*>(&debug);
            }
            instance.enabledExtensionCount   = uint32_t(extensions.size());
            instance.ppEnabledExtensionNames = extensions.data();
            Check(vkCreateInstance(&instance, nullptr, &Instance));
            if (Options.NativeWindow)
                Check(glfwCreateWindowSurface(
                    Instance, static_cast<GLFWwindow*>(Options.NativeWindow), nullptr, &Surface));
            if (Options.HeadlessSurface)
            {
                auto createHeadless = reinterpret_cast<PFN_vkCreateHeadlessSurfaceEXT>(
                    vkGetInstanceProcAddr(Instance, "vkCreateHeadlessSurfaceEXT"));
                Require(createHeadless != nullptr, "VK_EXT_headless_surface is unavailable");
                VkHeadlessSurfaceCreateInfoEXT headless {VK_STRUCTURE_TYPE_HEADLESS_SURFACE_CREATE_INFO_EXT};
                Check(createHeadless(Instance, &headless, nullptr, &Surface));
            }
            if (Options.Validation)
            {
                auto create = reinterpret_cast<PFN_vkCreateDebugUtilsMessengerEXT>(
                    vkGetInstanceProcAddr(Instance, "vkCreateDebugUtilsMessengerEXT"));
                Require(create != nullptr, "Debug messenger entry point is missing");
                Check(create(Instance, &debug, nullptr, &Messenger));
            }
            uint32_t count = 0;
            Check(vkEnumeratePhysicalDevices(Instance, &count, nullptr));
            std::vector<VkPhysicalDevice> devices(count);
            Check(vkEnumeratePhysicalDevices(Instance, &count, devices.data()));
            for (auto physical : devices)
            {
                VkPhysicalDeviceProperties properties;
                vkGetPhysicalDeviceProperties(physical, &properties);
                // This check precedes vkCreateDevice: never open an unintended GPU.
                if (!AcceptDeviceType(properties.deviceType, Options.RequireCpuDevice) ||
                    properties.apiVersion < VK_API_VERSION_1_3)
                    continue;
                VkPhysicalDeviceVulkan13Features f13 {VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_VULKAN_1_3_FEATURES};
                VkPhysicalDeviceFeatures2        supported {VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_FEATURES_2};
                supported.pNext = &f13;
                vkGetPhysicalDeviceFeatures2(physical, &supported);
                if (!f13.dynamicRendering || !f13.synchronization2)
                    continue;
                uint32_t families = 0;
                vkGetPhysicalDeviceQueueFamilyProperties(physical, &families, nullptr);
                std::vector<VkQueueFamilyProperties> queues(families);
                vkGetPhysicalDeviceQueueFamilyProperties(physical, &families, queues.data());
                for (uint32_t i = 0; i < families; ++i)
                    if (queues[i].queueCount &&
                        ((queues[i].queueFlags & (VK_QUEUE_GRAPHICS_BIT | VK_QUEUE_COMPUTE_BIT)) ==
                         (VK_QUEUE_GRAPHICS_BIT | VK_QUEUE_COMPUTE_BIT)))
                    {
                        VkBool32 present = VK_TRUE;
                        if (Surface)
                            Check(vkGetPhysicalDeviceSurfaceSupportKHR(physical, i, Surface, &present));
                        if (!present)
                            continue;
                        Physical    = physical;
                        Properties  = properties;
                        QueueFamily = i;
                        TimestampValidBits = queues[i].timestampValidBits;
                        break;
                    }
                if (Physical)
                    break;
            }
            Require(Physical != VK_NULL_HANDLE,
                    "No permitted Vulkan 1.3 graphics device (CPU required unless explicitly opted in)");
            float                   priority = 1;
            VkDeviceQueueCreateInfo queue {VK_STRUCTURE_TYPE_DEVICE_QUEUE_CREATE_INFO};
            queue.queueFamilyIndex = QueueFamily;
            queue.queueCount       = 1;
            queue.pQueuePriorities = &priority;
            VkPhysicalDeviceVulkan13Features enabled {VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_VULKAN_1_3_FEATURES};
            enabled.dynamicRendering = VK_TRUE;
            enabled.synchronization2 = VK_TRUE;
            VkPhysicalDeviceFeatures supportedFeatures {};
            vkGetPhysicalDeviceFeatures(Physical, &supportedFeatures);
            Require(supportedFeatures.fillModeNonSolid && supportedFeatures.independentBlend,
                    "Required graphics features are unavailable");
            VkPhysicalDeviceFeatures graphics {};
            graphics.sampleRateShading              = supportedFeatures.sampleRateShading;
            graphics.fillModeNonSolid               = VK_TRUE;
            graphics.independentBlend               = VK_TRUE;
            graphics.vertexPipelineStoresAndAtomics    = supportedFeatures.vertexPipelineStoresAndAtomics;
            graphics.fragmentStoresAndAtomics          = supportedFeatures.fragmentStoresAndAtomics;
            graphics.multiDrawIndirect                 = supportedFeatures.multiDrawIndirect;
            graphics.drawIndirectFirstInstance         = supportedFeatures.drawIndirectFirstInstance;
            graphics.shaderStorageImageExtendedFormats = supportedFeatures.shaderStorageImageExtendedFormats;
            graphics.shaderStorageImageReadWithoutFormat  = supportedFeatures.shaderStorageImageReadWithoutFormat;
            graphics.shaderStorageImageWriteWithoutFormat = supportedFeatures.shaderStorageImageWriteWithoutFormat;
            VkPhysicalDeviceVulkan12Features f12 {VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_VULKAN_1_2_FEATURES};
            VkPhysicalDeviceFeatures2        features12 {VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_FEATURES_2};
            features12.pNext = &f12;
            vkGetPhysicalDeviceFeatures2(Physical, &features12);
            VkPhysicalDeviceVulkan12Features enabled12 {VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_VULKAN_1_2_FEATURES};
            enabled12.drawIndirectCount = f12.drawIndirectCount;
            enabled.pNext               = &enabled12;
            // Match the neutral one-pixel line contract used by the GL path.
            // The optional extension supplies diamond-exit/Bresenham rasterization.
            uint32_t extensionCount = 0;
            Check(vkEnumerateDeviceExtensionProperties(Physical, nullptr, &extensionCount, nullptr));
            std::vector<VkExtensionProperties> deviceExtensions(extensionCount);
            Check(vkEnumerateDeviceExtensionProperties(Physical, nullptr, &extensionCount, deviceExtensions.data()));
            const char* lineExtension = VK_EXT_LINE_RASTERIZATION_EXTENSION_NAME;
            bool hasLines = std::any_of(deviceExtensions.begin(), deviceExtensions.end(), [&](const auto& extension) {
                return std::strcmp(extension.extensionName, lineExtension) == 0;
            });
            VkPhysicalDeviceLineRasterizationFeaturesEXT lines {
                VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_LINE_RASTERIZATION_FEATURES_EXT};
            if (hasLines)
            {
                VkPhysicalDeviceFeatures2 lineSupport {VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_FEATURES_2};
                lineSupport.pNext = &lines;
                vkGetPhysicalDeviceFeatures2(Physical, &lineSupport);
                BresenhamLines       = lines.bresenhamLines;
                lines                = {VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_LINE_RASTERIZATION_FEATURES_EXT};
                lines.bresenhamLines = BresenhamLines;
                if (BresenhamLines)
                    enabled12.pNext = &lines;
            }
            VkDeviceCreateInfo device {VK_STRUCTURE_TYPE_DEVICE_CREATE_INFO};
            device.pNext                = &enabled;
            device.pEnabledFeatures     = &graphics;
            std::vector<const char*> enabledExtensions;
            if (BresenhamLines)
                enabledExtensions.push_back(lineExtension);
            if (Surface)
                enabledExtensions.push_back(VK_KHR_SWAPCHAIN_EXTENSION_NAME);
            device.enabledExtensionCount   = uint32_t(enabledExtensions.size());
            device.ppEnabledExtensionNames = enabledExtensions.data();
            device.queueCreateInfoCount = 1;
            device.pQueueCreateInfos    = &queue;
            Check(vkCreateDevice(Physical, &device, nullptr, &Logical));
            vkGetDeviceQueue(Logical, QueueFamily, 0, &Queue);
            VmaAllocatorCreateInfo allocator {};
            allocator.instance         = Instance;
            allocator.physicalDevice   = Physical;
            allocator.device           = Logical;
            allocator.vulkanApiVersion = VK_API_VERSION_1_3;
            Check(vmaCreateAllocator(&allocator, &Allocator));
        }
        catch (...)
        {
            Destroy();
            throw;
        }
    }
    Context::~Context() { Destroy(); }
    void Context::Destroy()
    {
        if (Allocator)
        {
            vmaDestroyAllocator(Allocator);
            Allocator = VK_NULL_HANDLE;
        }
        if (Logical)
        {
            vkDestroyDevice(Logical, nullptr);
            Logical = VK_NULL_HANDLE;
        }
        if (Messenger)
        {
            auto destroy = reinterpret_cast<PFN_vkDestroyDebugUtilsMessengerEXT>(
                vkGetInstanceProcAddr(Instance, "vkDestroyDebugUtilsMessengerEXT"));
            destroy(Instance, Messenger, nullptr);
            Messenger = VK_NULL_HANDLE;
        }
        if (Surface)
        {
            vkDestroySurfaceKHR(Instance, Surface, nullptr);
            Surface = VK_NULL_HANDLE;
        }
        if (Instance)
        {
            vkDestroyInstance(Instance, nullptr);
            Instance = VK_NULL_HANDLE;
        }
    }
    FormatInfo Info(Format format)
    {
        switch (format)
        {
            case Format::RGBA8Unorm:
                return {VK_FORMAT_R8G8B8A8_UNORM, 4};
            case Format::RGBA8Srgb:
                return {VK_FORMAT_R8G8B8A8_SRGB, 4};
            case Format::RGBA16Float:
                return {VK_FORMAT_R16G16B16A16_SFLOAT, 8};
            case Format::RGBA32Float:
                return {VK_FORMAT_R32G32B32A32_SFLOAT, 16};
            case Format::R32Float:
                return {VK_FORMAT_R32_SFLOAT, 4};
            case Format::R16Float:
                return {VK_FORMAT_R16_SFLOAT, 2};
            case Format::RG16Float:
                return {VK_FORMAT_R16G16_SFLOAT, 4};
            case Format::RG8Unorm:
                return {VK_FORMAT_R8G8_UNORM, 2};
            case Format::R32Uint:
                return {VK_FORMAT_R32_UINT, 4};
            case Format::R8Unorm:
                return {VK_FORMAT_R8_UNORM, 1};
            case Format::Depth32Float:
                return {VK_FORMAT_D32_SFLOAT, 4, VK_IMAGE_ASPECT_DEPTH_BIT};
            case Format::Depth24Stencil8:
                return {VK_FORMAT_D24_UNORM_S8_UINT, 4, VK_IMAGE_ASPECT_DEPTH_BIT | VK_IMAGE_ASPECT_STENCIL_BIT};
        }
        throw std::invalid_argument("Unknown texture format");
    }
} // namespace Kans::RenderHardware::VulkanDetail
