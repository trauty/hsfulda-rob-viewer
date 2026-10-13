#pragma once

#include "robv/vk/context.h"

namespace robv
{
    // timeline semaphore and the last value a submit signals
    // one per queue paces any number of frames: frame N waits for N minus frames_in_flight
    struct vk_timeline_t
    {
        VkSemaphore semaphore = VK_NULL_HANDLE;
        u64 value = 0;
    };
} // namespace robv

namespace robv::vk
{
    bool create_timeline(const vk_context_t& ctx, vk_timeline_t& out);
    void destroy_timeline(const vk_context_t& ctx, vk_timeline_t& timeline);

    // next value to signal, bumps timeline.value
    u64 next_value(vk_timeline_t& timeline);

    // true once the GPU signalled value, false on timeout or error
    bool wait(const vk_context_t& ctx, const vk_timeline_t& timeline, u64 value, u64 timeout_ns = UINT64_MAX);
    u64 completed_value(const vk_context_t& ctx, const vk_timeline_t& timeline);
} // namespace robv::vk
