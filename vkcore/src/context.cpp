#include "robv/vk/context.h"

#include <algorithm>
#include <cstdlib>
#include <cstring>
#include <string_view>

namespace robv::vk
{
    namespace
    {
        constexpr const char* validation_layer = "VK_LAYER_KHRONOS_validation";

        VKAPI_ATTR VkBool32 VKAPI_CALL debug_callback(VkDebugUtilsMessageSeverityFlagBitsEXT severity,
                                                      VkDebugUtilsMessageTypeFlagsEXT,
                                                      const VkDebugUtilsMessengerCallbackDataEXT* data, void*)
        {
            const char* message = data && data->pMessage ? data->pMessage : "";
            if (severity & VK_DEBUG_UTILS_MESSAGE_SEVERITY_ERROR_BIT_EXT)
            {
                ROBV_LOG_ERROR("VALIDATION", "{}", message);
            }
            else if (severity & VK_DEBUG_UTILS_MESSAGE_SEVERITY_WARNING_BIT_EXT)
            {
                ROBV_LOG_WARN("VALIDATION", "{}", message);
            }
            else
            {
                ROBV_LOG_DEBUG("VALIDATION", "{}", message);
            }
            return VK_FALSE;
        }

        bool has_layer(const char* name)
        {
            u32 count = 0;
            vkEnumerateInstanceLayerProperties(&count, nullptr);
            std::vector<VkLayerProperties> layers(count);
            vkEnumerateInstanceLayerProperties(&count, layers.data());
            return std::any_of(layers.begin(), layers.end(),
                               [&](const VkLayerProperties& layer) { return std::strcmp(layer.layerName, name) == 0; });
        }

        std::vector<VkExtensionProperties> instance_extensions()
        {
            u32 count = 0;
            vkEnumerateInstanceExtensionProperties(nullptr, &count, nullptr);
            std::vector<VkExtensionProperties> extensions(count);
            vkEnumerateInstanceExtensionProperties(nullptr, &count, extensions.data());
            return extensions;
        }

        std::vector<VkExtensionProperties> device_extensions(VkPhysicalDevice device)
        {
            u32 count = 0;
            vkEnumerateDeviceExtensionProperties(device, nullptr, &count, nullptr);
            std::vector<VkExtensionProperties> extensions(count);
            vkEnumerateDeviceExtensionProperties(device, nullptr, &count, extensions.data());
            return extensions;
        }

        bool contains(const std::vector<VkExtensionProperties>& extensions, const char* name)
        {
            return std::any_of(extensions.begin(), extensions.end(), [&](const VkExtensionProperties& extension)
                               { return std::strcmp(extension.extensionName, name) == 0; });
        }

        struct queue_families_t
        {
            u32 graphics = UINT32_MAX;
            u32 compute = UINT32_MAX;
            u32 transfer = UINT32_MAX;
        };

        bool pick_queue_families(VkPhysicalDevice device, VkSurfaceKHR surface, bool need_presentation,
                                 queue_families_t& out)
        {
            u32 count = 0;
            vkGetPhysicalDeviceQueueFamilyProperties(device, &count, nullptr);
            std::vector<VkQueueFamilyProperties> families(count);
            vkGetPhysicalDeviceQueueFamilyProperties(device, &count, families.data());

            out = {};
            for (u32 i = 0; i < count; ++i)
            {
                if (!(families[i].queueFlags & VK_QUEUE_GRAPHICS_BIT)) { continue; }
                if (need_presentation)
                {
                    VkBool32 present = VK_FALSE;
                    if (surface) { vkGetPhysicalDeviceSurfaceSupportKHR(device, i, surface, &present); }
                    if (!present) { continue; }
                }
                out.graphics = i;
                break;
            }
            if (out.graphics == UINT32_MAX) { return false; }

            out.compute = out.graphics;
            for (u32 i = 0; i < count; ++i)
            {
                const VkQueueFlags flags = families[i].queueFlags;
                if ((flags & VK_QUEUE_COMPUTE_BIT) && !(flags & VK_QUEUE_GRAPHICS_BIT))
                {
                    out.compute = i;
                    break;
                }
            }

            out.transfer = out.compute;
            for (u32 i = 0; i < count; ++i)
            {
                const VkQueueFlags flags = families[i].queueFlags;
                if ((flags & VK_QUEUE_TRANSFER_BIT) && !(flags & (VK_QUEUE_GRAPHICS_BIT | VK_QUEUE_COMPUTE_BIT)))
                {
                    out.transfer = i;
                    break;
                }
            }
            return true;
        }

        u32 type_score(VkPhysicalDeviceType type)
        {
            switch (type)
            {
            case VK_PHYSICAL_DEVICE_TYPE_DISCRETE_GPU: return 1000;
            case VK_PHYSICAL_DEVICE_TYPE_INTEGRATED_GPU: return 500;
            case VK_PHYSICAL_DEVICE_TYPE_VIRTUAL_GPU: return 100;
            case VK_PHYSICAL_DEVICE_TYPE_CPU: return 10;
            default: return 1;
            }
        }

        struct candidate_t
        {
            VkPhysicalDevice device = VK_NULL_HANDLE;
            VkPhysicalDeviceProperties properties{};
            queue_families_t families;
            std::vector<VkExtensionProperties> extensions;
            bool has_ycbcr = false;
            bool has_storage_formats = false;
            bool has_large_points = false;
            u32 score = 0;
        };

        // empty when usable, else the reason
        std::string check_device(VkPhysicalDevice device, const vk_context_config_t& config, VkSurfaceKHR surface,
                                 candidate_t& out)
        {
            out = {};
            out.device = device;
            vkGetPhysicalDeviceProperties(device, &out.properties);

            if (out.properties.apiVersion < config.api_version)
            {
                return "supports Vulkan " + version_string(out.properties.apiVersion) + ", " +
                       version_string(config.api_version) + " is required";
            }

            out.extensions = device_extensions(device);
            std::vector<const char*> required = config.extra_device_extensions;
            if (config.need_presentation) { required.push_back(VK_KHR_SWAPCHAIN_EXTENSION_NAME); }
            for (const char* name : required)
            {
                if (!contains(out.extensions, name)) { return std::string("lacks ") + name; }
            }

            VkPhysicalDeviceVulkan11Features features11{.sType = VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_VULKAN_1_1_FEATURES};
            VkPhysicalDeviceVulkan12Features features12{.sType = VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_VULKAN_1_2_FEATURES,
                                                        .pNext = &features11};
            VkPhysicalDeviceFeatures2 features{.sType = VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_FEATURES_2,
                                               .pNext = &features12};
            vkGetPhysicalDeviceFeatures2(device, &features);
            if (!features12.timelineSemaphore) { return "lacks timeline semaphores"; }
            out.has_ycbcr = features11.samplerYcbcrConversion == VK_TRUE;
            out.has_storage_formats = features.features.shaderStorageImageExtendedFormats == VK_TRUE;
            out.has_large_points = features.features.largePoints == VK_TRUE;

            if (!pick_queue_families(device, surface, config.need_presentation, out.families))
            {
                return config.need_presentation ? "has no graphics queue that can present to the surface"
                                                : "has no graphics queue";
            }

            out.score = type_score(out.properties.deviceType);
            return {};
        }
    } // namespace

    bool init_instance(vk_context_t& ctx, const vk_context_config_t& config)
    {
        if (config.loader) { volkInitializeCustom(config.loader); }
        else if (volkInitialize() != VK_SUCCESS)
        {
            ROBV_LOG_ERROR("VULKAN", "No Vulkan loader found (libvulkan.so)");
            return false;
        }

        ctx.instance_version = volkGetInstanceVersion();
        if (ctx.instance_version < VK_API_VERSION_1_1)
        {
            ROBV_LOG_ERROR("VULKAN", "Vulkan loader is {}, at least 1.1 is required",
                           version_string(ctx.instance_version));
            return false;
        }

        const std::vector<VkExtensionProperties> available = instance_extensions();

        std::vector<const char*> layers;
        ctx.validation_enabled = false;
        if (config.enable_validation)
        {
            if (has_layer(validation_layer))
            {
                layers.push_back(validation_layer);
                ctx.validation_enabled = true;
            }
            else
            {
                ROBV_LOG_WARN("VULKAN", "{} is not installed, running without validation", validation_layer);
            }
        }

        std::vector<const char*> extensions = config.extra_instance_extensions;
        for (const char* name : extensions)
        {
            if (!contains(available, name))
            {
                ROBV_LOG_ERROR("VULKAN", "Instance extension {} is not available", name);
                return false;
            }
        }
        const bool debug_utils = ctx.validation_enabled && contains(available, VK_EXT_DEBUG_UTILS_EXTENSION_NAME);
        if (debug_utils) { extensions.push_back(VK_EXT_DEBUG_UTILS_EXTENSION_NAME); }

        const VkApplicationInfo app_info{
            .sType = VK_STRUCTURE_TYPE_APPLICATION_INFO,
            .pApplicationName = config.app_name.c_str(),
            .applicationVersion = VK_MAKE_API_VERSION(0, 0, 1, 0),
            .pEngineName = "robv",
            .engineVersion = VK_MAKE_API_VERSION(0, 0, 1, 0),
            .apiVersion = config.api_version,
        };

        const VkDebugUtilsMessengerCreateInfoEXT messenger_info{
            .sType = VK_STRUCTURE_TYPE_DEBUG_UTILS_MESSENGER_CREATE_INFO_EXT,
            .messageSeverity =
                VK_DEBUG_UTILS_MESSAGE_SEVERITY_WARNING_BIT_EXT | VK_DEBUG_UTILS_MESSAGE_SEVERITY_ERROR_BIT_EXT,
            .messageType = VK_DEBUG_UTILS_MESSAGE_TYPE_GENERAL_BIT_EXT |
                           VK_DEBUG_UTILS_MESSAGE_TYPE_VALIDATION_BIT_EXT |
                           VK_DEBUG_UTILS_MESSAGE_TYPE_PERFORMANCE_BIT_EXT,
            .pfnUserCallback = debug_callback,
        };

        const VkInstanceCreateInfo instance_info{
            .sType = VK_STRUCTURE_TYPE_INSTANCE_CREATE_INFO,
            .pNext = debug_utils ? &messenger_info : nullptr,
            .pApplicationInfo = &app_info,
            .enabledLayerCount = static_cast<u32>(layers.size()),
            .ppEnabledLayerNames = layers.data(),
            .enabledExtensionCount = static_cast<u32>(extensions.size()),
            .ppEnabledExtensionNames = extensions.data(),
        };

        ROBV_VK_TRY(vkCreateInstance(&instance_info, nullptr, &ctx.instance));
        volkLoadInstance(ctx.instance);

        if (debug_utils)
        {
            ROBV_VK_TRY(vkCreateDebugUtilsMessengerEXT(ctx.instance, &messenger_info, nullptr, &ctx.messenger));
        }

        ROBV_LOG_INFO("VULKAN", "Instance {} (loader {}){}", version_string(config.api_version),
                      version_string(ctx.instance_version), ctx.validation_enabled ? ", validation on" : "");
        return true;
    }

    bool init_device(vk_context_t& ctx, const vk_context_config_t& config, VkSurfaceKHR surface)
    {
        u32 count = 0;
        vkEnumeratePhysicalDevices(ctx.instance, &count, nullptr);
        std::vector<VkPhysicalDevice> devices(count);
        vkEnumeratePhysicalDevices(ctx.instance, &count, devices.data());
        if (count == 0)
        {
            ROBV_LOG_ERROR("VULKAN", "No Vulkan devices. Is a driver (ICD) installed?");
            return false;
        }

        // ROBV_VK_DEVICE=<part of the name> picks a device by hand, for example on hybrid graphics laptops
        const char* wanted = std::getenv("ROBV_VK_DEVICE");

        candidate_t best;
        for (VkPhysicalDevice device : devices)
        {
            candidate_t candidate;
            const std::string reason = check_device(device, config, surface, candidate);
            const char* name = candidate.properties.deviceName;
            if (!reason.empty())
            {
                ROBV_LOG_INFO("VULKAN", "  skip {}: {}", name, reason);
                continue;
            }
            if (wanted && *wanted && std::string_view(name).find(wanted) == std::string_view::npos)
            {
                ROBV_LOG_INFO("VULKAN", "  skip {}: ROBV_VK_DEVICE={}", name, wanted);
                continue;
            }
            ROBV_LOG_INFO("VULKAN", "  candidate {} ({})", name, device_type_string(candidate.properties.deviceType));
            if (candidate.score > best.score) { best = std::move(candidate); }
        }

        if (!best.device)
        {
            ROBV_LOG_ERROR("VULKAN", "No usable Vulkan device, see the reasons above");
            return false;
        }

        ctx.physical_device = best.device;
        ctx.properties = best.properties;
        ctx.api_version = best.properties.apiVersion;
        ctx.ycbcr_conversion = best.has_ycbcr;
        ctx.large_points = best.has_large_points;

        std::vector<const char*> extensions = config.extra_device_extensions;
        if (config.need_presentation) { extensions.push_back(VK_KHR_SWAPCHAIN_EXTENSION_NAME); }
        for (const char* name : config.optional_device_extensions)
        {
            if (contains(best.extensions, name)) { extensions.push_back(name); }
        }

        const f32 priority = 1.0f;
        std::vector<VkDeviceQueueCreateInfo> queue_infos;
        for (const u32 family : {best.families.graphics, best.families.compute, best.families.transfer})
        {
            const bool seen =
                std::any_of(queue_infos.begin(), queue_infos.end(),
                            [&](const VkDeviceQueueCreateInfo& info) { return info.queueFamilyIndex == family; });
            if (seen) { continue; }
            queue_infos.push_back({
                .sType = VK_STRUCTURE_TYPE_DEVICE_QUEUE_CREATE_INFO,
                .queueFamilyIndex = family,
                .queueCount = 1,
                .pQueuePriorities = &priority,
            });
        }

        // YCbCr sampling shows MediaCodec output without a copy
        VkPhysicalDeviceVulkan11Features features11{
            .sType = VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_VULKAN_1_1_FEATURES,
            .samplerYcbcrConversion = best.has_ycbcr ? VK_TRUE : VK_FALSE,
        };
        VkPhysicalDeviceVulkan12Features features12{
            .sType = VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_VULKAN_1_2_FEATURES,
            .pNext = &features11,
            .timelineSemaphore = VK_TRUE,
        };
        // r8 and rg8 storage images, the NV12 planes the conversion writes
        // core in 1.3, near universal in 1.2
        // large points: point sizes other than 1, the point renderer's size setting
        const VkPhysicalDeviceFeatures2 features{
            .sType = VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_FEATURES_2,
            .pNext = &features12,
            .features = {.largePoints = best.has_large_points ? VK_TRUE : VK_FALSE,
                         .shaderStorageImageExtendedFormats = best.has_storage_formats ? VK_TRUE : VK_FALSE},
        };

        const VkDeviceCreateInfo device_info{
            .sType = VK_STRUCTURE_TYPE_DEVICE_CREATE_INFO,
            .pNext = &features,
            .queueCreateInfoCount = static_cast<u32>(queue_infos.size()),
            .pQueueCreateInfos = queue_infos.data(),
            .enabledExtensionCount = static_cast<u32>(extensions.size()),
            .ppEnabledExtensionNames = extensions.data(),
        };

        ROBV_VK_TRY(vkCreateDevice(ctx.physical_device, &device_info, nullptr, &ctx.device));
        volkLoadDevice(ctx.device);

        ctx.enabled_device_extensions.assign(extensions.begin(), extensions.end());

        ctx.graphics.family = best.families.graphics;
        ctx.compute.family = best.families.compute;
        ctx.transfer.family = best.families.transfer;
        vkGetDeviceQueue(ctx.device, ctx.graphics.family, 0, &ctx.graphics.queue);
        vkGetDeviceQueue(ctx.device, ctx.compute.family, 0, &ctx.compute.queue);
        vkGetDeviceQueue(ctx.device, ctx.transfer.family, 0, &ctx.transfer.queue);

        ctx.driver = {.sType = VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_DRIVER_PROPERTIES};
        VkPhysicalDeviceProperties2 properties2{.sType = VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_PROPERTIES_2,
                                                .pNext = &ctx.driver};
        vkGetPhysicalDeviceProperties2(ctx.physical_device, &properties2);

        const VmaVulkanFunctions functions{
            .vkGetInstanceProcAddr = vkGetInstanceProcAddr,
            .vkGetDeviceProcAddr = vkGetDeviceProcAddr,
        };
        const VmaAllocatorCreateInfo allocator_info{
            .physicalDevice = ctx.physical_device,
            .device = ctx.device,
            .pVulkanFunctions = &functions,
            .instance = ctx.instance,
            .vulkanApiVersion = config.api_version,
        };
        ROBV_VK_TRY(vmaCreateAllocator(&allocator_info, &ctx.allocator));

        ROBV_LOG_INFO("VULKAN", "Device {} ({}), Vulkan {}, {} {}", static_cast<const char*>(ctx.properties.deviceName),
                      device_type_string(ctx.properties.deviceType), version_string(ctx.api_version),
                      static_cast<const char*>(ctx.driver.driverName), static_cast<const char*>(ctx.driver.driverInfo));
        ROBV_LOG_INFO("VULKAN", "Queues graphics={} compute={} transfer={}", ctx.graphics.family, ctx.compute.family,
                      ctx.transfer.family);
        return true;
    }

    void shutdown(vk_context_t& ctx)
    {
        if (ctx.device) { vkDeviceWaitIdle(ctx.device); }
        if (ctx.allocator) { vmaDestroyAllocator(ctx.allocator); }
        if (ctx.device) { vkDestroyDevice(ctx.device, nullptr); }
        if (ctx.messenger) { vkDestroyDebugUtilsMessengerEXT(ctx.instance, ctx.messenger, nullptr); }
        if (ctx.instance) { vkDestroyInstance(ctx.instance, nullptr); }
        ctx = {};
    }

    bool has_device_extension(const vk_context_t& ctx, const char* name)
    {
        return std::any_of(ctx.enabled_device_extensions.begin(), ctx.enabled_device_extensions.end(),
                           [&](const std::string& extension) { return extension == name; });
    }

    const char* device_type_string(VkPhysicalDeviceType type)
    {
        switch (type)
        {
        case VK_PHYSICAL_DEVICE_TYPE_DISCRETE_GPU: return "discrete";
        case VK_PHYSICAL_DEVICE_TYPE_INTEGRATED_GPU: return "integrated";
        case VK_PHYSICAL_DEVICE_TYPE_VIRTUAL_GPU: return "virtual";
        case VK_PHYSICAL_DEVICE_TYPE_CPU: return "cpu";
        default: return "other";
        }
    }

    std::string version_string(u32 version)
    {
        return std::to_string(VK_API_VERSION_MAJOR(version)) + "." + std::to_string(VK_API_VERSION_MINOR(version)) +
               "." + std::to_string(VK_API_VERSION_PATCH(version));
    }

    const char* result_string(VkResult result)
    {
        switch (result)
        {
        case VK_SUCCESS: return "VK_SUCCESS";
        case VK_NOT_READY: return "VK_NOT_READY";
        case VK_TIMEOUT: return "VK_TIMEOUT";
        case VK_INCOMPLETE: return "VK_INCOMPLETE";
        case VK_SUBOPTIMAL_KHR: return "VK_SUBOPTIMAL_KHR";
        case VK_ERROR_OUT_OF_HOST_MEMORY: return "VK_ERROR_OUT_OF_HOST_MEMORY";
        case VK_ERROR_OUT_OF_DEVICE_MEMORY: return "VK_ERROR_OUT_OF_DEVICE_MEMORY";
        case VK_ERROR_INITIALIZATION_FAILED: return "VK_ERROR_INITIALIZATION_FAILED";
        case VK_ERROR_DEVICE_LOST: return "VK_ERROR_DEVICE_LOST";
        case VK_ERROR_MEMORY_MAP_FAILED: return "VK_ERROR_MEMORY_MAP_FAILED";
        case VK_ERROR_LAYER_NOT_PRESENT: return "VK_ERROR_LAYER_NOT_PRESENT";
        case VK_ERROR_EXTENSION_NOT_PRESENT: return "VK_ERROR_EXTENSION_NOT_PRESENT";
        case VK_ERROR_FEATURE_NOT_PRESENT: return "VK_ERROR_FEATURE_NOT_PRESENT";
        case VK_ERROR_INCOMPATIBLE_DRIVER: return "VK_ERROR_INCOMPATIBLE_DRIVER";
        case VK_ERROR_TOO_MANY_OBJECTS: return "VK_ERROR_TOO_MANY_OBJECTS";
        case VK_ERROR_FORMAT_NOT_SUPPORTED: return "VK_ERROR_FORMAT_NOT_SUPPORTED";
        case VK_ERROR_SURFACE_LOST_KHR: return "VK_ERROR_SURFACE_LOST_KHR";
        case VK_ERROR_NATIVE_WINDOW_IN_USE_KHR: return "VK_ERROR_NATIVE_WINDOW_IN_USE_KHR";
        case VK_ERROR_OUT_OF_DATE_KHR: return "VK_ERROR_OUT_OF_DATE_KHR";
        default: return "VK_ERROR_(other)";
        }
    }
} // namespace robv::vk
