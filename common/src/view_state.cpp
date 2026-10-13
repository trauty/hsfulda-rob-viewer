#include "robv/common/view_state.h"

#include <algorithm>
#include <cmath>

namespace robv::view_state
{
    namespace
    {
        constexpr f32 pi = 3.14159265358979f;
        constexpr f32 max_pitch = pi * 0.5f - 0.01f;
        constexpr f32 min_distance = 0.1f;
        constexpr f32 max_distance = 10000.0f;
        constexpr f32 max_zoom_step = 4.0f;

        bool finite(const input_intent_t& intent)
        { return std::isfinite(intent.dx) && std::isfinite(intent.dy) && std::isfinite(intent.scalar); }

        f32 wrap_angle(f32 angle)
        {
            angle = std::fmod(angle + pi, 2.0f * pi);
            if (angle < 0.0f) { angle += 2.0f * pi; }
            return angle - pi;
        }

        void orbit(camera_t& camera, f32 dx, f32 dy)
        {
            camera.yaw = wrap_angle(camera.yaw + dx);
            camera.pitch = std::clamp(camera.pitch + dy, -max_pitch, max_pitch);
        }

        // moves the target in the view plane so the point under the finger stays under it
        void pan(camera_t& camera, f32 dx, f32 dy)
        {
            const f32 view_height = 2.0f * camera.distance * std::tan(camera.fov_y * 0.5f);

            const f32 cy = std::cos(camera.yaw);
            const f32 sy = std::sin(camera.yaw);
            const f32 cp = std::cos(camera.pitch);
            const f32 sp = std::sin(camera.pitch);

            // eye at target + distance * (cp*cy, cp*sy, sp), right and up span the view plane
            const vec3_t right{-sy, cy, 0.0f};
            const vec3_t up{-sp * cy, -sp * sy, cp};

            const f32 move_x = -dx * view_height;
            const f32 move_y = dy * view_height;
            camera.target.x += right.x * move_x + up.x * move_y;
            camera.target.y += right.y * move_x + up.y * move_y;
            camera.target.z += right.z * move_x + up.z * move_y;
        }

        void zoom(camera_t& camera, f32 amount)
        {
            amount = std::clamp(amount, -max_zoom_step, max_zoom_step);
            camera.distance = std::clamp(camera.distance * std::exp(-amount), min_distance, max_distance);
        }

        void seek(playback_state_t& playback, f32 fraction)
        {
            fraction = std::clamp(fraction, 0.0f, 1.0f);
            const u64 range = playback.end_ns > playback.begin_ns ? playback.end_ns - playback.begin_ns : 0;
            playback.cursor_ns =
                playback.begin_ns + static_cast<u64>(static_cast<f64>(range) * static_cast<f64>(fraction));
        }
    } // namespace

    bool apply(view_state_t& state, const input_intent_t& intent)
    {
        if (!finite(intent)) { return false; }

        camera_t& camera = state.camera;
        render_settings_t& render = state.render;
        playback_state_t& playback = state.playback;

        switch (intent.kind)
        {
        case intent_kind_e::orbit: orbit(camera, intent.dx, intent.dy); break;
        case intent_kind_e::pan: pan(camera, intent.dx, intent.dy); break;
        case intent_kind_e::zoom: zoom(camera, intent.scalar); break;
        case intent_kind_e::reset_camera: camera = camera_t{}; break;

        case intent_kind_e::set_color_mode:
        {
            const i32 mode = static_cast<i32>(intent.scalar);
            if (mode < 0 || mode > static_cast<i32>(color_mode_e::flat)) { return false; }
            render.color_mode = static_cast<color_mode_e>(mode);
            break;
        }
        case intent_kind_e::set_point_size: render.point_size = std::clamp(intent.scalar, 1.0f, 32.0f); break;
        case intent_kind_e::set_intensity_range:
            if (intent.dx >= intent.dy) { return false; }
            render.intensity_min = intent.dx;
            render.intensity_max = intent.dy;
            break;
        case intent_kind_e::set_grid: render.show_grid = intent.scalar != 0.0f; break;
        case intent_kind_e::set_axes: render.show_axes = intent.scalar != 0.0f; break;
        case intent_kind_e::set_eye_dome_lighting: render.eye_dome_lighting = intent.scalar != 0.0f; break;

        case intent_kind_e::play: playback.playing = true; break;
        case intent_kind_e::pause: playback.playing = false; break;
        case intent_kind_e::set_rate: playback.rate = std::clamp(intent.scalar, 0.05f, 16.0f); break;
        case intent_kind_e::seek: seek(playback, intent.scalar); break;

        default: return false;
        }

        ++state.tick;
        return true;
    }

    bool is_valid_intent_kind(u8 value)
    {
        switch (static_cast<intent_kind_e>(value))
        {
        case intent_kind_e::orbit:
        case intent_kind_e::pan:
        case intent_kind_e::zoom:
        case intent_kind_e::reset_camera:
        case intent_kind_e::set_color_mode:
        case intent_kind_e::set_point_size:
        case intent_kind_e::set_intensity_range:
        case intent_kind_e::set_grid:
        case intent_kind_e::set_axes:
        case intent_kind_e::set_eye_dome_lighting:
        case intent_kind_e::play:
        case intent_kind_e::pause:
        case intent_kind_e::set_rate:
        case intent_kind_e::seek: return true;
        }
        return false;
    }
} // namespace robv::view_state
