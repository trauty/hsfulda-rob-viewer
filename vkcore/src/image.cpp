#include "robv/vk/image.h"

namespace robv::vk
{
    bool create_image(const vk_context_t& ctx, const vk_image_desc_t& desc, vk_image_t& out)
    {
        out = {};

        const VkImageCreateInfo image_info{
            .sType = VK_STRUCTURE_TYPE_IMAGE_CREATE_INFO,
            .imageType = VK_IMAGE_TYPE_2D,
            .format = desc.format,
            .extent = {desc.extent.width, desc.extent.height, 1},
            .mipLevels = 1,
            .arrayLayers = 1,
            .samples = VK_SAMPLE_COUNT_1_BIT,
            .tiling = VK_IMAGE_TILING_OPTIMAL,
            .usage = desc.usage,
            .sharingMode = VK_SHARING_MODE_EXCLUSIVE,
            .initialLayout = VK_IMAGE_LAYOUT_UNDEFINED,
        };
        const VmaAllocationCreateInfo alloc_info{.usage = VMA_MEMORY_USAGE_AUTO_PREFER_DEVICE};
        ROBV_VK_TRY(vmaCreateImage(ctx.allocator, &image_info, &alloc_info, &out.image, &out.allocation, nullptr));

        const VkImageViewCreateInfo view_info{
            .sType = VK_STRUCTURE_TYPE_IMAGE_VIEW_CREATE_INFO,
            .image = out.image,
            .viewType = VK_IMAGE_VIEW_TYPE_2D,
            .format = desc.format,
            .subresourceRange = {desc.aspect, 0, 1, 0, 1},
        };
        if (vkCreateImageView(ctx.device, &view_info, nullptr, &out.view) != VK_SUCCESS)
        {
            ROBV_LOG_ERROR("VULKAN", "vkCreateImageView failed");
            destroy_image(ctx, out);
            return false;
        }

        out.extent = desc.extent;
        out.format = desc.format;
        return true;
    }

    void destroy_image(const vk_context_t& ctx, vk_image_t& image)
    {
        if (image.view) { vkDestroyImageView(ctx.device, image.view, nullptr); }
        if (image.image) { vmaDestroyImage(ctx.allocator, image.image, image.allocation); }
        image = {};
    }
} // namespace robv::vk
