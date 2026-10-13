#include "robv/vk/shader.h"

namespace robv::vk
{
    VkShaderModule create_shader_module(const vk_context_t& ctx, std::span<const u32> spirv)
    {
        constexpr u32 spirv_magic = 0x07230203;
        if (spirv.empty() || spirv[0] != spirv_magic)
        {
            ROBV_LOG_ERROR("VULKAN", "Not SPIR-V ({} words)", spirv.size());
            return VK_NULL_HANDLE;
        }

        const VkShaderModuleCreateInfo info{
            .sType = VK_STRUCTURE_TYPE_SHADER_MODULE_CREATE_INFO,
            .codeSize = spirv.size_bytes(),
            .pCode = spirv.data(),
        };

        VkShaderModule module = VK_NULL_HANDLE;
        const VkResult result = vkCreateShaderModule(ctx.device, &info, nullptr, &module);
        if (result != VK_SUCCESS)
        {
            ROBV_LOG_ERROR("VULKAN", "vkCreateShaderModule failed: {}", result_string(result));
            return VK_NULL_HANDLE;
        }
        return module;
    }
} // namespace robv::vk
