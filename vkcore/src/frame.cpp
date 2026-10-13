#include "robv/vk/frame.h"

namespace robv::vk
{
    bool create_timeline(const vk_context_t& ctx, vk_timeline_t& out)
    {
        const VkSemaphoreTypeCreateInfo type_info{
            .sType = VK_STRUCTURE_TYPE_SEMAPHORE_TYPE_CREATE_INFO,
            .semaphoreType = VK_SEMAPHORE_TYPE_TIMELINE,
            .initialValue = 0,
        };
        const VkSemaphoreCreateInfo info{.sType = VK_STRUCTURE_TYPE_SEMAPHORE_CREATE_INFO, .pNext = &type_info};

        out = {};
        ROBV_VK_TRY(vkCreateSemaphore(ctx.device, &info, nullptr, &out.semaphore));
        return true;
    }

    void destroy_timeline(const vk_context_t& ctx, vk_timeline_t& timeline)
    {
        if (timeline.semaphore) { vkDestroySemaphore(ctx.device, timeline.semaphore, nullptr); }
        timeline = {};
    }

    u64 next_value(vk_timeline_t& timeline) { return ++timeline.value; }

    bool wait(const vk_context_t& ctx, const vk_timeline_t& timeline, u64 value, u64 timeout_ns)
    {
        if (value == 0) { return true; }

        const VkSemaphoreWaitInfo info{
            .sType = VK_STRUCTURE_TYPE_SEMAPHORE_WAIT_INFO,
            .semaphoreCount = 1,
            .pSemaphores = &timeline.semaphore,
            .pValues = &value,
        };
        const VkResult result = vkWaitSemaphores(ctx.device, &info, timeout_ns);
        if (result != VK_SUCCESS && result != VK_TIMEOUT)
        {
            ROBV_LOG_ERROR("VULKAN", "vkWaitSemaphores failed: {}", result_string(result));
        }
        return result == VK_SUCCESS;
    }

    u64 completed_value(const vk_context_t& ctx, const vk_timeline_t& timeline)
    {
        u64 value = 0;
        vkGetSemaphoreCounterValue(ctx.device, timeline.semaphore, &value);
        return value;
    }
} // namespace robv::vk
