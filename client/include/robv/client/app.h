#pragma once

#include "robv/client/decoder.h"
#include "robv/client/presenter.h"
#include "robv/client/vsync.h"
#include "robv/common/view_state.h"
#include "robv/vk/context.h"

#include <string>

struct SDL_Window;
union SDL_Event;

namespace robv
{
    enum class connection_state_e : u8
    {
        disconnected,
        connecting,
        connected
    };

    struct connection_t
    {
        connection_state_e state = connection_state_e::disconnected;
        std::string server = "192.168.1.10:9400";
        u32 client_id = 0;
        f32 rtt_ms = 0.0f;
        // latest server snapshot
        view_state_t view;
        std::string status = "control channel not implemented yet";
    };

    struct app_config_t
    {
        decoder_kind_e decoder = decoder_kind_e::null;
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
        connection_t connection;
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
