#pragma once

#include "robv/common/protocol.h"
#include "robv/common/rtp.h"

#include <string>
#include <string_view>

// control channel to robv-server: hello, intents and pings out; welcome, source info, snapshots and pongs in
// video: RTP over UDP from the port in welcome, reassembled into access units on the same thread
// sockets live on their own thread, which blocks in poll() and wakes the UI thread with an SDL event
// only when something arrived, so an idle client costs no wakeups
namespace robv
{
    enum class connection_state_e : u8
    {
        disconnected,
        connecting,
        connected
    };

    // what the UI shows, copied out of the net thread by net::poll
    struct connection_t
    {
        connection_state_e state = connection_state_e::disconnected;
        std::string server;
        std::string status = "not connected";
        u32 client_id = 0;
        f32 rtt_ms = 0.0f;
        bool has_view = false;
        // latest server snapshot
        view_state_t view;
        video_info_t video;
        source_info_msg_t source;
        // RTP totals, refreshed once a second
        u64 video_units = 0;
        u64 video_lost = 0;
        // server steady clock minus ours, from the pong with the lowest round trip of the last few
        i64 clock_offset_ns = 0;
        bool has_clock_offset = false;
    };

    struct net_t
    {
        // the UI thread's last copy
        u64 seen_version = 0;
        u32 next_seq = 1;
        void* backend = nullptr;
    };
} // namespace robv

namespace robv::net
{
    bool start(net_t& net, std::string_view client_name, u32 wake_event);
    void stop(net_t& net);

    void connect(net_t& net, std::string_view server);
    void disconnect(net_t& net);

    void send_intent(net_t& net, input_intent_t intent);

    void take_video(net_t& net, std::vector<rtp_unit_t>& out);

    void request_keyframe(net_t& net);

    bool poll(net_t& net, connection_t& out);

    const char* state_name(connection_state_e state);

    u64 clock_ns();
} // namespace robv::net
