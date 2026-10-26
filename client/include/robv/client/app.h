#pragma once

#include "robv/client/decoder.h"
#include "robv/client/net.h"
#include "robv/client/presenter.h"
#include "robv/client/video_view.h"
#include "robv/client/vsync.h"
#include "robv/common/view_state.h"
#include "robv/vk/context.h"

#include <string>

struct SDL_Window;
union SDL_Event;

namespace robv
{
    struct gesture_t
    {
        // touch
        u64 fingers[2] = {};
        f32 finger_x[2] = {};
        f32 finger_y[2] = {};
        u32 finger_count = 0;

        // desktop
        bool mouse_orbit = false;
        bool mouse_pan = false;

        f32 orbit_dx = 0.0f;
        f32 orbit_dy = 0.0f;
        f32 pan_dx = 0.0f;
        f32 pan_dy = 0.0f;
        f32 zoom = 0.0f;
    };

    struct app_config_t
    {
        decoder_kind_e decoder = decoder_kind_e::null;
        std::string server; // host:port
        bool validation = false;
        // draw every refresh, for benchmarks and smoke tests
        bool continuous = false;
        u32 width = 1280;
        u32 height = 720;
    };

    struct app_t
    {
        app_config_t config;
        SDL_Window* window = nullptr;
        vk_context_config_t vk_config;
        vk_context_t vk;
        presenter_t presenter;
        decoder_t decoder;
        decoded_frame_t decoded;
        // access units handed over by the net thread, reused
        std::vector<rtp_unit_t> units;
        bool decoded_pending = false;
        // zero copy decoder images: the one on screen, and replaced ones until the frames drawing them finished
        struct retiring_image_t
        {
            void* image = nullptr;
            u64 done_at = 0;
        };

        void* shown_image = nullptr;
        std::vector<retiring_image_t> retiring_images;
        video_view_t video;
        net_t net;
        u32 net_event = 0;
        // decoder finishing on its thread (MediaCodec) wakes the loop with this
        u32 decoder_event = 0;
        connection_t connection;

        char server_text[128] = {};
        gesture_t gesture;

        f32 panel_min[2] = {};
        f32 panel_max[2] = {};
        bool imgui_ready = false;
        bool in_background = false;
        bool quit = false;

        u64 start_ns = 0;
        u64 frames = 0;
        f32 ui_scale = 1.0f;

        // on demand drawing, a present costs a GPU submit and a compositor transaction
        // frames owed after an event, idle refresh due time
        u32 pending_frames = 0;
        u64 next_idle_ns = 0;
        u64 last_draw_ns = 0;
        bool last_frame_presented = false;
        // frames start on a grid after each vsync where reported (Android), see app::wait
        vsync_t vsync;
        u64 last_slot_ns = 0;

        // panel stats: frames over the last second, CPU time of the last one
        u64 rate_window_ns = 0;
        u32 rate_window_frames = 0;
        f32 draws_per_second = 0.0f;
        f32 frame_cpu_ms = 0.0f;

        f32 video_age_ms = 0.0f;

        u64 stream_log_ns = 0;
        u64 stream_log_frames = 0;
    };
} // namespace robv

namespace robv::app
{
    // failure is logged, plus a message box where nobody reads logs
    bool init(app_t& app, const app_config_t& config);
    void shutdown(app_t& app);

    void handle_event(app_t& app, const SDL_Event& event);
    // blocks until an event or a due frame, then holds to one frame per refresh
    // the hold precedes input polling so a frame sees the newest touch
    void wait(app_t& app);
    // one frame if due: decode, ui, present
    void iterate(app_t& app);
} // namespace robv::app
