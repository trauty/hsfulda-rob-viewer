#pragma once

#include "robv/vk/context.h"

namespace robv
{
    enum class vk_memory_e : u8
    {
        gpu_only,
        // host visible, mapped, CPU writes
        upload,
        // host visible, mapped, CPU reads
        readback
    };

    struct vk_buffer_t
    {
        VkBuffer buffer = VK_NULL_HANDLE;
        VmaAllocation allocation = VK_NULL_HANDLE;
        VkDeviceSize size = 0;
        // null for gpu_only
        void* mapped = nullptr;
    };
} // namespace robv

namespace robv::vk
{
    bool create_buffer(const vk_context_t& ctx, VkDeviceSize size, VkBufferUsageFlags usage, vk_memory_e memory,
                       vk_buffer_t& out);
    void destroy_buffer(const vk_context_t& ctx, vk_buffer_t& buffer);
} // namespace robv::vk
