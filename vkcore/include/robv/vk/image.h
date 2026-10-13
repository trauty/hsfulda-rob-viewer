#pragma once

#include "robv/vk/context.h"

namespace robv
{
    struct vk_image_desc_t
    {
        VkExtent2D extent{};
        VkFormat format = VK_FORMAT_R8G8B8A8_UNORM;
        VkImageUsageFlags usage = 0;
        VkImageAspectFlags aspect = VK_IMAGE_ASPECT_COLOR_BIT;
    };

    struct vk_image_t
    {
        VkImage image = VK_NULL_HANDLE;
        VkImageView view = VK_NULL_HANDLE;
        VmaAllocation allocation = VK_NULL_HANDLE;
        VkExtent2D extent{};
        VkFormat format = VK_FORMAT_UNDEFINED;
    };
} // namespace robv

namespace robv::vk
{
    // 2D, single mip, device local, with a view
    bool create_image(const vk_context_t& ctx, const vk_image_desc_t& desc, vk_image_t& out);
    void destroy_image(const vk_context_t& ctx, vk_image_t& image);
} // namespace robv::vk
