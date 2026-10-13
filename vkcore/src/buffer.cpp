#include "robv/vk/buffer.h"

namespace robv::vk
{
    bool create_buffer(const vk_context_t& ctx, VkDeviceSize size, VkBufferUsageFlags usage, vk_memory_e memory,
                       vk_buffer_t& out)
    {
        const VkBufferCreateInfo buffer_info{
            .sType = VK_STRUCTURE_TYPE_BUFFER_CREATE_INFO,
            .size = size,
            .usage = usage,
            .sharingMode = VK_SHARING_MODE_EXCLUSIVE,
        };

        VmaAllocationCreateInfo alloc_info{.usage = VMA_MEMORY_USAGE_AUTO};
        switch (memory)
        {
        case vk_memory_e::gpu_only: alloc_info.usage = VMA_MEMORY_USAGE_AUTO_PREFER_DEVICE; break;
        case vk_memory_e::upload:
            alloc_info.flags =
                VMA_ALLOCATION_CREATE_HOST_ACCESS_SEQUENTIAL_WRITE_BIT | VMA_ALLOCATION_CREATE_MAPPED_BIT;
            break;
        case vk_memory_e::readback:
            alloc_info.flags = VMA_ALLOCATION_CREATE_HOST_ACCESS_RANDOM_BIT | VMA_ALLOCATION_CREATE_MAPPED_BIT;
            break;
        }

        out = {};
        VmaAllocationInfo info{};
        ROBV_VK_TRY(vmaCreateBuffer(ctx.allocator, &buffer_info, &alloc_info, &out.buffer, &out.allocation, &info));
        out.size = size;
        out.mapped = info.pMappedData;
        return true;
    }

    void destroy_buffer(const vk_context_t& ctx, vk_buffer_t& buffer)
    {
        if (buffer.buffer) { vmaDestroyBuffer(ctx.allocator, buffer.buffer, buffer.allocation); }
        buffer = {};
    }
} // namespace robv::vk
