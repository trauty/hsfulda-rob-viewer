#pragma once

// clang-format off
#include "robv/common/log.h"
#include "robv/common/types.h"
#include <volk.h>
#include <vk_mem_alloc.h>
// clang-format on

namespace robv::vk { const char* result_string(VkResult result); } // namespace robv::vk

// for functions returning bool: logs the failing call, returns false
#define ROBV_VK_TRY(expr)                                                                                              \
    do                                                                                                                 \
    {                                                                                                                  \
        const VkResult robv_vk_result = (expr);                                                                        \
        if (robv_vk_result != VK_SUCCESS)                                                                              \
        {                                                                                                              \
            ROBV_LOG_ERROR("VULKAN", "{} failed: {}", #expr, robv::vk::result_string(robv_vk_result));                 \
            return false;                                                                                              \
        }                                                                                                              \
    } while (0)
