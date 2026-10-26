#pragma once

#include "robv/common/types.h"

#include <string>

namespace robv
{
    // orbit camera around target, z up like ROS
    // yaw 0 looks along minus x from plus x, pitch > 0 looks down
    struct camera_t
    {
        vec3_t target;
        f32 yaw = PI / 4.0f;
        f32 pitch = 0.5f;
        f32 distance = 20.0f;
        f32 fov_y = PI / 3.0f;
        f32 near_plane = 0.05f;
        f32 far_plane = 2000.0f;

        bool operator==(const camera_t&) const = default;
    };

    enum class color_mode_e : u8
    {
        intensity = 0,
        height = 1,
        rgb = 2,
        flat = 3
    };

    struct render_settings_t
    {
        color_mode_e color_mode = color_mode_e::intensity;
        f32 point_size = 2.0f;
        f32 intensity_min = 0.0f;
        f32 intensity_max = 255.0f;
        bool show_grid = true;
        bool show_axes = true;
        bool eye_dome_lighting = false;

        bool operator==(const render_settings_t&) const = default;
    };

    struct playback_state_t
    {
        bool playing = true;
        f32 rate = 1.0f;
        u64 cursor_ns = 0;
        u64 begin_ns = 0;
        u64 end_ns = 0;

        bool operator==(const playback_state_t&) const = default;
    };

    // state every client sees, server owns it
    struct view_state_t
    {
        u64 tick = 0;
        camera_t camera;
        render_settings_t render;
        playback_state_t playback;
        std::string active_topic;

        bool operator==(const view_state_t&) const = default;
    };

    // device independent units, clients convert from pixels
    // orbit   dx dy radians
    // pan     dx dy fractions of the view height at the target distance
    // zoom    scalar log factor, distance times exp of minus scalar
    // seek    scalar fraction 0..1 of the playback range
    // set_*   scalar is the value, bools are scalar != 0, set_intensity_range uses dx dy as min max
    enum class intent_kind_e : u8
    {
        orbit = 1,
        pan = 2,
        zoom = 3,
        reset_camera = 4,
        set_color_mode = 16,
        set_point_size = 17,
        set_intensity_range = 18,
        set_grid = 19,
        set_axes = 20,
        set_eye_dome_lighting = 21,
        play = 32,
        pause = 33,
        set_rate = 34,
        seek = 35
    };

    struct input_intent_t
    {
        u32 seq = 0;
        intent_kind_e kind = intent_kind_e::orbit;
        f32 dx = 0.0f;
        f32 dy = 0.0f;
        f32 scalar = 0.0f;

        bool operator==(const input_intent_t&) const = default;
    };
} // namespace robv

namespace robv::view_state
{
    // folds one intent in and bumps the tick, the only way state changes
    // concurrent drags sum, discrete settings are last write wins
    // false when malformed and ignored, tick unchanged
    bool apply(view_state_t& state, const input_intent_t& intent);

    bool is_valid_intent_kind(u8 value);
} // namespace robv::view_state
