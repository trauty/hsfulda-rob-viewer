#include "robv/server/transport.h"

#include "robv/common/log.h"

#include <algorithm>

namespace robv::transport
{
    namespace
    {
        // simulated client: connects on the first poll, orbits every frame, zooms, pings each second
        // exercises the same event path as a socket
        struct null_control_t
        {
            bool connected = false;
            u32 client_id = 0;
            u32 seq = 0;
            u64 polls = 0;
            u64 next_ping_ns = 0;
        };

        void append_frame_bytes(control_server_t& server, size_t recipients)
        {
            server.messages_out += recipients;
            server.bytes_out += server.scratch.size() * recipients;
        }

        // serialized even without a socket, so null also exercises the protocol encoder
        template <typename msg_t>
        void send_to(control_server_t& server, u32 client_id, const msg_t& msg)
        {
            const bool known = std::any_of(server.peers.begin(), server.peers.end(),
                                           [&](const peer_t& peer) { return peer.client_id == client_id; });
            if (!known) { return; }

            server.scratch.clear();
            protocol::write(server.scratch, msg);
            append_frame_bytes(server, 1);
        }
    } // namespace

    bool open_video_sink(video_sink_t& sink, transport_kind_e kind, const video_info_t& info)
    {
        sink = {};
        sink.kind = kind;
        sink.info = info;

        if (kind == transport_kind_e::net)
        {
            ROBV_LOG_ERROR("NET", "the RTP video sink is not implemented yet, run with --transport null");
            return false;
        }
        ROBV_LOG_INFO("NET", "{} video sink, {} {}x{}, ssrc {:08x}", kind_name(kind), video::codec_name(info.codec),
                      info.width, info.height, info.ssrc);
        return true;
    }

    void close_video_sink(video_sink_t& sink) { sink = {}; }

    void add_video_peer(video_sink_t& sink, u32 client_id)
    {
        if (std::find(sink.peers.begin(), sink.peers.end(), client_id) == sink.peers.end())
        {
            sink.peers.push_back(client_id);
        }
    }

    void remove_video_peer(video_sink_t& sink, u32 client_id)
    { sink.peers.erase(std::remove(sink.peers.begin(), sink.peers.end(), client_id), sink.peers.end()); }

    void send_video(video_sink_t& sink, const encoded_packet_t& packet)
    {
        sink.packets += sink.peers.size();
        sink.bytes += packet.data.size() * sink.peers.size();
    }

    bool open_control(control_server_t& server, transport_kind_e kind, u16 port)
    {
        server = {};
        server.kind = kind;
        server.port = port;

        switch (kind)
        {
        case transport_kind_e::null:
            server.backend = new null_control_t;
            ROBV_LOG_INFO("NET", "null control server, one simulated client");
            return true;
        case transport_kind_e::net:
            ROBV_LOG_ERROR("NET", "the TCP control server is not implemented yet, run with --transport null");
            return false;
        }
        return false;
    }

    void close_control(control_server_t& server)
    {
        if (server.kind == transport_kind_e::null) { delete static_cast<null_control_t*>(server.backend); }
        server.backend = nullptr;
        server.peers.clear();
    }

    void poll_control(control_server_t& server, u64 now_ns, std::vector<control_event_t>& out)
    {
        if (server.kind != transport_kind_e::null || !server.backend) { return; }
        null_control_t* sim = static_cast<null_control_t*>(server.backend);
        sim->polls++;

        if (!sim->connected)
        {
            sim->connected = true;
            sim->client_id = server.next_client_id++;
            sim->next_ping_ns = now_ns;
            server.peers.push_back({sim->client_id, "simulated"});
            out.push_back({.kind = control_event_kind_e::connected, .client_id = sim->client_id});
            return;
        }

        control_event_t orbit{.kind = control_event_kind_e::intent, .client_id = sim->client_id};
        orbit.intent = {.seq = ++sim->seq, .kind = intent_kind_e::orbit, .dx = 0.01f};
        out.push_back(orbit);

        if (sim->polls % 60 == 0)
        {
            control_event_t zoom{.kind = control_event_kind_e::intent, .client_id = sim->client_id};
            zoom.intent = {
                .seq = ++sim->seq, .kind = intent_kind_e::zoom, .scalar = (sim->polls / 60) % 2 == 0 ? 0.3f : -0.3f};
            out.push_back(zoom);
        }

        if (now_ns >= sim->next_ping_ns)
        {
            sim->next_ping_ns = now_ns + 1'000'000'000ull;
            control_event_t ping{.kind = control_event_kind_e::ping, .client_id = sim->client_id};
            ping.ping.t_client_ns = now_ns;
            out.push_back(ping);
        }
    }

    void send_welcome(control_server_t& server, u32 client_id, const welcome_msg_t& msg)
    { send_to(server, client_id, msg); }

    void send_source_info(control_server_t& server, u32 client_id, const source_info_msg_t& msg)
    { send_to(server, client_id, msg); }

    void send_pong(control_server_t& server, u32 client_id, const pong_msg_t& msg) { send_to(server, client_id, msg); }

    // serialized once, identical for every client
    void broadcast_state(control_server_t& server, const view_state_t& state)
    {
        if (server.peers.empty()) { return; }
        server.scratch.clear();
        protocol::write(server.scratch, state_snapshot_msg_t{state});
        append_frame_bytes(server, server.peers.size());
    }

    bool parse_kind(std::string_view text, transport_kind_e& out)
    {
        if (text == "null") { out = transport_kind_e::null; }
        else if (text == "net") { out = transport_kind_e::net; }
        else
        {
            return false;
        }
        return true;
    }

    const char* kind_name(transport_kind_e kind)
    {
        switch (kind)
        {
        case transport_kind_e::null: return "null";
        case transport_kind_e::net: return "net";
        }
        return "unknown";
    }
} // namespace robv::transport
