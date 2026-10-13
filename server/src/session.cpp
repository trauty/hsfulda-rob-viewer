#include "robv/server/session.h"

#include "robv/common/log.h"

#include <algorithm>
#include <chrono>

namespace robv::session
{
    namespace
    {
        constexpr u64 stats_window_ns = 2'000'000'000ull;

        u64 steady_ns()
        {
            return static_cast<u64>(std::chrono::duration_cast<std::chrono::nanoseconds>(
                                        std::chrono::steady_clock::now().time_since_epoch())
                                        .count());
        }

        f64 ms_since(u64 start_ns) { return static_cast<f64>(steady_ns() - start_ns) * 1e-6; }

        source_info_msg_t make_source_info(const session_t& session)
        {
            source_info_msg_t info;
            info.topics = session.source.caps.topics;
            info.source_uri = session.source.uri_text;
            if (session.has_cloud)
            {
                info.fields = session.cloud.fields;
                info.point_count = point_cloud::point_count(session.cloud);
            }
            return info;
        }

        void handle_event(session_t& session, const control_event_t& event, u64 now_ns)
        {
            switch (event.kind)
            {
            case control_event_kind_e::connected:
            {
                const welcome_msg_t welcome{.client_id = event.client_id, .video = session.sink.info};
                transport::send_welcome(session.control, event.client_id, welcome);
                transport::send_source_info(session.control, event.client_id, make_source_info(session));
                transport::add_video_peer(session.sink, event.client_id);
                // a late joiner can only start decoding at an IDR
                encoder::request_keyframe(session.encoder);
                // and gets the state immediately
                session.last_state_tick = UINT64_MAX;
                ROBV_LOG_INFO("SESSION", "client {} connected, {} total", event.client_id,
                              session.control.peers.size());
                break;
            }
            case control_event_kind_e::disconnected:
                transport::remove_video_peer(session.sink, event.client_id);
                ROBV_LOG_INFO("SESSION", "client {} disconnected", event.client_id);
                break;
            case control_event_kind_e::intent:
                if (view_state::apply(session.state, event.intent)) { ++session.stats.intents; }
                else
                {
                    ROBV_LOG_DEBUG("SESSION", "ignored malformed intent {} from client {}", event.intent.seq,
                                   event.client_id);
                }
                break;
            case control_event_kind_e::ping:
                transport::send_pong(session.control, event.client_id,
                                     {.t_client_ns = event.ping.t_client_ns, .t_server_ns = now_ns});
                break;
            }
        }

        void log_stats(session_t& session, u64 now_ns)
        {
            session_stats_t& stats = session.stats;
            if (stats.window_start_ns == 0)
            {
                stats.window_start_ns = now_ns;
                return;
            }
            const u64 elapsed = now_ns - stats.window_start_ns;
            if (elapsed < stats_window_ns) { return; }

            const f64 seconds = static_cast<f64>(elapsed) * 1e-9;
            const f64 frames = static_cast<f64>(stats.window_frames);
            ROBV_LOG_INFO(
                "SESSION",
                "frame {} | {:.1f} fps | render {:.2f} ms encode {:.2f} ms | tick {} yaw {:+.2f} dist {:.1f} | "
                "clouds {} intents {} | video {} pkt {} B | control {} msg {} B",
                stats.frames, frames / seconds, frames > 0 ? stats.render_ms / frames : 0.0,
                frames > 0 ? stats.encode_ms / frames : 0.0, session.state.tick, session.state.camera.yaw,
                session.state.camera.distance, stats.clouds, stats.intents, session.sink.packets, session.sink.bytes,
                session.control.messages_out, session.control.bytes_out);

            stats.window_start_ns = now_ns;
            stats.window_frames = 0;
            stats.render_ms = 0.0;
            stats.encode_ms = 0.0;
        }
    } // namespace

    bool init(session_t& session, const session_config_t& config)
    {
        session.config = config;
        session.state = {};

        vk_context_config_t vk_config;
        vk_config.app_name = "robv-server";
        vk_config.need_presentation = false;
        vk_config.enable_validation = config.validation;
        // zero copy handoff to the hardware encoder, if the driver has them
        vk_config.optional_device_extensions = {VK_KHR_EXTERNAL_MEMORY_FD_EXTENSION_NAME,
                                                VK_EXT_EXTERNAL_MEMORY_DMA_BUF_EXTENSION_NAME,
                                                VK_EXT_IMAGE_DRM_FORMAT_MODIFIER_EXTENSION_NAME};
        if (!vk::init_instance(session.vk, vk_config) || !vk::init_device(session.vk, vk_config, VK_NULL_HANDLE))
        {
            return false;
        }

        if (!source::open(session.source, config.source_uri)) { return false; }
        session.state.playback.begin_ns = session.source.caps.begin_ns;
        session.state.playback.end_ns = session.source.caps.end_ns;
        session.state.playback.cursor_ns = session.source.caps.begin_ns;
        if (!session.source.caps.topics.empty()) { session.state.active_topic = session.source.caps.topics.front(); }

        const renderer_config_t renderer_config{
            .kind = config.renderer, .width = config.width, .height = config.height};
        if (!renderer::init(session.renderer, renderer_config, session.vk)) { return false; }

        const encoder_config_t encoder_config{
            .kind = config.encoder,
            .codec = config.codec,
            .width = config.width,
            .height = config.height,
            .fps = config.fps,
            .bitrate_kbps = config.bitrate_kbps,
        };
        if (!encoder::open(session.encoder, encoder_config)) { return false; }

        // one stream, one ssrc, from the clock, differs per run
        const video_info_t video{
            .codec = config.codec,
            .width = static_cast<u16>(config.width),
            .height = static_cast<u16>(config.height),
            .rtp_port = config.rtp_port,
            .ssrc = static_cast<u32>(steady_ns() ^ (steady_ns() >> 32)),
        };
        if (!transport::open_video_sink(session.sink, config.transport, video)) { return false; }
        if (!transport::open_control(session.control, config.transport, config.control_port)) { return false; }

        return true;
    }

    void tick(session_t& session, u64 now_ns)
    {
        session.events.clear();
        transport::poll_control(session.control, now_ns, session.events);
        for (const control_event_t& event : session.events) { handle_event(session, event, now_ns); }

        source::set_paused(session.source, !session.state.playback.playing);
        if (source::poll(session.source, now_ns, session.cloud))
        {
            session.has_cloud = true;
            ++session.stats.clouds;
        }

        u64 start = steady_ns();
        renderer::draw(session.renderer, session.has_cloud ? &session.cloud : nullptr, session.state, session.frame);
        session.frame.pts_ns = now_ns;
        session.stats.render_ms += ms_since(start);

        start = steady_ns();
        session.packets.clear();
        encoder::encode(session.encoder, session.frame, session.packets);
        for (const encoded_packet_t& packet : session.packets) { transport::send_video(session.sink, packet); }
        session.stats.encode_ms += ms_since(start);

        // only broadcast snapshot when something changes
        const u64 state_period_ns = 1'000'000'000ull / std::max(1u, session.config.state_rate_hz);
        if (session.state.tick != session.last_state_tick && now_ns - session.last_state_ns >= state_period_ns)
        {
            transport::broadcast_state(session.control, session.state);
            session.last_state_ns = now_ns;
            session.last_state_tick = session.state.tick;
        }

        session.stats.frames++;
        session.stats.window_frames++;
        log_stats(session, now_ns);
    }

    void shutdown(session_t& session)
    {
        transport::close_control(session.control);
        transport::close_video_sink(session.sink);
        encoder::close(session.encoder);
        renderer::shutdown(session.renderer);
        source::close(session.source);
        vk::shutdown(session.vk);
    }
} // namespace robv::session
