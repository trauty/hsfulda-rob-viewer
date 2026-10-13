#include "robv/server/renderer.h"

#include "robv/common/log.h"
#include "robv/shaders/rgb_to_nv12.h"
#include "robv/vk/shader.h"

#include <algorithm>
#include <cmath>
#include <cstring>
#include <glm/gtc/matrix_transform.hpp>

namespace robv::renderer
{
    namespace
    {
        constexpr f32 two_pi = 6.28318530718f;

        // white bar along the bottom, length is the point count
        void draw_cloud_bar(planar_frame_t& frame, const point_cloud_frame_t* cloud)
        {
            if (!cloud) { return; }

            const u32 bar_height = std::max(2u, frame.height / 40) & ~1u;
            const f64 fill =
                std::clamp(std::log10(static_cast<f64>(point_cloud::point_count(*cloud)) + 1.0) / 7.0, 0.0, 1.0);
            const u32 bar_width = static_cast<u32>(fill * frame.width) & ~1u;

            u8* y = video::y_plane(frame);
            u8* uv = video::uv_plane(frame);
            for (u32 row = frame.height - bar_height; row < frame.height; ++row)
            {
                std::memset(y + static_cast<size_t>(row) * frame.y_stride, 235, bar_width);
            }
            for (u32 row = (frame.height - bar_height) / 2; row < frame.height / 2; ++row)
            {
                std::memset(uv + static_cast<size_t>(row) * frame.uv_stride, 128, bar_width);
            }
        }
    } // namespace

    bool init(renderer_t& renderer, const renderer_config_t& config, const vk_context_t& vk)
    {
        renderer = {};
        renderer.config = config;
        renderer.vk = &vk;

        // runs after the point pass in the real renderer
        // loaded now to prove shader embedding and device setup
        renderer.rgb_to_nv12 = vk::create_shader_module(vk, shaders::rgb_to_nv12);
        if (!renderer.rgb_to_nv12) { return false; }

        ROBV_LOG_INFO("RENDER", "{} renderer {}x{}, rgb_to_nv12.slang loaded ({} bytes of SPIR-V)",
                      kind_name(config.kind), config.width, config.height, sizeof(shaders::rgb_to_nv12));
        return true;
    }

    void shutdown(renderer_t& renderer)
    {
        if (renderer.vk && renderer.rgb_to_nv12)
        {
            vkDestroyShaderModule(renderer.vk->device, renderer.rgb_to_nv12, nullptr);
        }
        renderer = {};
    }

    // null: colour bars scrolled by yaw, tinted by distance, so any intent visibly changes the stream
    bool draw(renderer_t& renderer, const point_cloud_frame_t* cloud, const view_state_t& state, planar_frame_t& out)
    {
        if (out.width != renderer.config.width || out.height != renderer.config.height || out.data.empty())
        {
            video::allocate_nv12(out, renderer.config.width, renderer.config.height);
        }

        const f32 phase = (state.camera.yaw + two_pi * 0.5f) / two_pi;
        const f32 hue =
            std::log2(std::max(state.camera.distance, 0.1f)) * 0.1f + static_cast<f32>(state.render.color_mode) * 0.25f;
        video::fill_test_pattern(out, phase, hue);
        draw_cloud_bar(out, cloud);

        out.tick = state.tick;
        ++renderer.frames;
        return true;
    }

    camera_matrices_t camera_matrices(const camera_t& camera, f32 aspect)
    {
        const f32 cp = std::cos(camera.pitch);
        const glm::vec3 target(camera.target.x, camera.target.y, camera.target.z);
        const glm::vec3 offset(cp * std::cos(camera.yaw), cp * std::sin(camera.yaw), std::sin(camera.pitch));

        camera_matrices_t out;
        out.eye = target + offset * camera.distance;
        out.view = glm::lookAt(out.eye, target, glm::vec3(0.0f, 0.0f, 1.0f));
        out.projection = glm::perspective(camera.fov_y, aspect, camera.near_plane, camera.far_plane);
        out.projection[1][1] *= -1.0f;
        return out;
    }

    bool parse_kind(std::string_view text, renderer_kind_e& out)
    {
        if (text == "null")
        {
            out = renderer_kind_e::null;
            return true;
        }
        return false;
    }

    const char* kind_name(renderer_kind_e kind)
    {
        switch (kind)
        {
        case renderer_kind_e::null: return "null";
        }
        return "unknown";
    }
} // namespace robv::renderer
