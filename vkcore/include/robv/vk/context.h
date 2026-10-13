#pragma once

#include "robv/vk/vulkan.h"

#include <string>
#include <vector>

namespace robv
{
    struct vk_context_config_t
    {
        std::string app_name = "robv";
        u32 api_version = VK_API_VERSION_1_2;
        // false gives the headless server path: no surface, no swapchain extension
        bool need_presentation = false;
        bool enable_validation = false;
        std::vector<const char*> extra_instance_extensions;
        std::vector<const char*> extra_device_extensions;
        // used when present, e.g. the zero copy ones
        std::vector<const char*> optional_device_extensions;
        // null opens the system loader, the client passes SDL's
        PFN_vkGetInstanceProcAddr loader = nullptr;
    };

    struct vk_queue_t
    {
        VkQueue queue = VK_NULL_HANDLE;
        u32 family = UINT32_MAX;
    };

    struct vk_context_t
    {
        VkInstance instance = VK_NULL_HANDLE;
        VkDebugUtilsMessengerEXT messenger = VK_NULL_HANDLE;
        VkPhysicalDevice physical_device = VK_NULL_HANDLE;
        VkDevice device = VK_NULL_HANDLE;
        VmaAllocator allocator = VK_NULL_HANDLE;

        vk_queue_t graphics;
        vk_queue_t compute;
        vk_queue_t transfer;

        u32 instance_version = 0;
        u32 api_version = 0;
        bool validation_enabled = false;
        VkPhysicalDeviceProperties properties{};
        VkPhysicalDeviceDriverProperties driver{};
        std::vector<std::string> enabled_device_extensions;
    };
} // namespace robv

namespace robv::vk
{
    bool init_instance(vk_context_t& ctx, const vk_context_config_t& config);
    bool init_device(vk_context_t& ctx, const vk_context_config_t& config, VkSurfaceKHR surface);

    void shutdown(vk_context_t& ctx);

    bool has_device_extension(const vk_context_t& ctx, const char* name);

    const char* device_type_string(VkPhysicalDeviceType type);
    std::string version_string(u32 version);
} // namespace robv::vk
