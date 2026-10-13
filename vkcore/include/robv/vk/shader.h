#pragma once

#include "robv/vk/context.h"

#include <span>

namespace robv::vk
{
    VkShaderModule create_shader_module(const vk_context_t& ctx, std::span<const u32> spirv);
} // namespace robv::vk
