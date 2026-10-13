#pragma once

#include "robv/common/protocol.h"
#include "robv/server/encoder.h"

#include <string>
#include <string_view>
#include <vector>

// video: RTP over UDP to every peer, one encode and N sends
// control: TCP protocol from protocol.h
namespace robv
{
    enum class transport_kind_e : u8
    {
        // no sockets, a simulated client orbits slowly
        null,
        net,
    };

    struct peer_t
    {
        u32 client_id = 0;
        std::string name;
    };

    enum class control_event_kind_e : u8
    {
        connected,
        disconnected,
        intent,
        ping
    };

    struct control_event_t
    {
        control_event_kind_e kind = control_event_kind_e::connected;
        u32 client_id = 0;
        input_intent_t intent;
        ping_msg_t ping;
    };

    struct video_sink_t
    {
        transport_kind_e kind = transport_kind_e::null;
        video_info_t info;
        std::vector<u32> peers;
        u64 packets = 0;
        u64 bytes = 0;
        void* backend = nullptr;
    };

    struct control_server_t
    {
        transport_kind_e kind = transport_kind_e::null;
        u16 port = 0;
        std::vector<peer_t> peers;
        u32 next_client_id = 1;
        u64 messages_out = 0;
        u64 bytes_out = 0;
        std::vector<u8> scratch;
        void* backend = nullptr;
    };
} // namespace robv

namespace robv::transport
{
    bool open_video_sink(video_sink_t& sink, transport_kind_e kind, const video_info_t& info);
    void close_video_sink(video_sink_t& sink);
    void add_video_peer(video_sink_t& sink, u32 client_id);
    void remove_video_peer(video_sink_t& sink, u32 client_id);
    void send_video(video_sink_t& sink, const encoded_packet_t& packet);

    bool open_control(control_server_t& server, transport_kind_e kind, u16 port);
    void close_control(control_server_t& server);
    // appends what arrived since the last poll, never blocks
    void poll_control(control_server_t& server, u64 now_ns, std::vector<control_event_t>& out);
    void send_welcome(control_server_t& server, u32 client_id, const welcome_msg_t& msg);
    void send_source_info(control_server_t& server, u32 client_id, const source_info_msg_t& msg);
    void send_pong(control_server_t& server, u32 client_id, const pong_msg_t& msg);
    void broadcast_state(control_server_t& server, const view_state_t& state);

    bool parse_kind(std::string_view text, transport_kind_e& out);
    const char* kind_name(transport_kind_e kind);
} // namespace robv::transport
