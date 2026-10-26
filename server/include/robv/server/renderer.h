#pragma once

#include "robv/common/point_cloud.h"
#include "robv/common/video.h"
#include "robv/common/view_state.h"
#include "robv/vk/context.h"

#include <glm/glm.hpp>
#include <string_view>

namespace robv
{
    enum class renderer_kind_e : u8
    {
        // CPU test pattern driven by the view state
        null,
        // points, grid and axes with Vulkan, rgb_to_nv12 on the GPU, NV12 read back
        vulkan,
    };

    struct renderer_config_t
    {
        renderer_kind_e kind = renderer_kind_e::null;
        u32 width = 1280;
        u32 height = 720;
    };

    // point cloud + view state in, NV12 out
    struct renderer_t
    {
        renderer_config_t config;
        const vk_context_t* vk = nullptr;
        u64 frames = 0;
        // clouds uploaded to the GPU, a source repeating its blob is not uploaded again
        u64 uploads = 0;
        void* backend = nullptr;
    };

    struct camera_matrices_t
    {
        glm::mat4 view{1.0f};
        glm::mat4 projection{1.0f};
        glm::vec3 eye{0.0f};
    };
} // namespace robv

namespace robv::renderer
{
    bool init(renderer_t& renderer, const renderer_config_t& config, const vk_context_t& vk);
    void shutdown(renderer_t& renderer);

    // cloud may be null before the source produced anything
    bool draw(renderer_t& renderer, const point_cloud_frame_t* cloud, const view_state_t& state, planar_frame_t& out);

    camera_matrices_t camera_matrices(const camera_t& camera, f32 aspect);

    bool parse_kind(std::string_view text, renderer_kind_e& out);
    const char* kind_name(renderer_kind_e kind);
} // namespace robv::renderer
