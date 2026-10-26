#pragma once

#include "robv/server/encoder.h"
#include "robv/server/renderer.h"
#include "robv/server/source.h"
#include "robv/server/transport.h"
#include "robv/vk/context.h"

#include <string>
#include <vector>

namespace robv
{
    struct session_config_t
    {
        std::string source_uri = "synthetic://sphere";
        renderer_kind_e renderer = renderer_kind_e::null;
        encoder_kind_e encoder = encoder_kind_e::null;
        transport_kind_e transport = transport_kind_e::null;
        video_codec_e codec = video_codec_e::h264;
        u32 width = 1280;
        u32 height = 720;
        u32 fps = 30;
        u32 bitrate_kbps = 8000;
        u16 control_port = 9400;
        u16 rtp_port = 9402;
        u32 state_rate_hz = 30;
        bool validation = false;
    };

    struct session_stats_t
    {
        u64 frames = 0;
        u64 intents = 0;
        u64 clouds = 0;
        u64 packets = 0;
        u64 bytes = 0;
        f64 render_ms = 0.0;
        f64 encode_ms = 0.0;
        u64 window_start_ns = 0;
        u64 window_frames = 0;
    };

    // owns the view state, wires source to renderer to encoder to sinks
    // plain data advanced by tick(), no threads yet
    struct session_t
    {
        session_config_t config;
        vk_context_t vk;
        view_state_t state;
        source_t source;
        point_cloud_frame_t cloud;
        bool has_cloud = false;
        renderer_t renderer;
        planar_frame_t frame;
        encoder_t encoder;
        std::vector<encoded_packet_t> packets;
        video_sink_t sink;
        control_server_t control;
        std::vector<control_event_t> events;
        u64 last_state_ns = 0;
        u64 last_state_tick = UINT64_MAX;
        // a client losing packets asks every frame, an IDR is several times a P frame
        u64 last_keyframe_request_ns = 0;
        session_stats_t stats;
    };
} // namespace robv

namespace robv::session
{
    bool init(session_t& session, const session_config_t& config);
    void tick(session_t& session, u64 now_ns);
    void shutdown(session_t& session);
} // namespace robv::session
