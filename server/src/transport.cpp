#include "robv/server/transport.h"

#include "robv/common/log.h"
#include "robv/common/rtp.h"

#include <algorithm>
#include <arpa/inet.h>
#include <cerrno>
#include <cstring>
#include <netinet/in.h>
#include <netinet/tcp.h>
#include <sys/socket.h>
#include <unistd.h>

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

        constexpr size_t max_net_peers = 32;
        constexpr size_t max_send_backlog = 4u * 1024u * 1024u;

        struct net_peer_t
        {
            int fd = -1;
            u32 client_id = 0;
            std::string address;
            u32 ipv4 = 0;
            std::vector<u8> in;
            std::vector<u8> out;
            bool dead = false;
        };

        struct net_control_t
        {
            int listen_fd = -1;
            std::vector<net_peer_t> peers;
        };

        // video: one UDP socket on the RTP port, punches come in and RTP goes out from it
        struct video_peer_t
        {
            u32 client_id = 0;
            u32 ipv4 = 0;
            sockaddr_in address{};
            bool reachable = false;
        };

        struct net_video_t
        {
            int fd = -1;
            rtp_packetizer_t packetizer;
            rtp_packets_t packets;
            std::vector<video_peer_t> peers;
            u64 dropped = 0;
        };

        void append_frame_bytes(control_server_t& server, size_t recipients)
        {
            server.messages_out += recipients;
            server.bytes_out += server.scratch.size() * recipients;
        }

        void close_fd(int& fd)
        {
            if (fd >= 0) { ::close(fd); }
            fd = -1;
        }

        void drop(net_peer_t& peer, const char* reason)
        {
            if (peer.dead) { return; }
            ROBV_LOG_INFO("NET", "{} (client {}) dropped: {}", peer.address, peer.client_id, reason);
            peer.dead = true;
        }

        void flush(net_peer_t& peer)
        {
            size_t sent = 0;
            while (!peer.dead && sent < peer.out.size())
            {
                const ssize_t n = send(peer.fd, peer.out.data() + sent, peer.out.size() - sent, MSG_NOSIGNAL);
                if (n > 0)
                {
                    sent += static_cast<size_t>(n);
                    continue;
                }
                if (n < 0 && errno == EINTR) { continue; }
                if (n < 0 && (errno == EAGAIN || errno == EWOULDBLOCK)) { break; }
                drop(peer, n < 0 ? std::strerror(errno) : "send returned 0");
            }
            peer.out.erase(peer.out.begin(), peer.out.begin() + static_cast<std::ptrdiff_t>(sent));
        }

        void queue(net_peer_t& peer, std::span<const u8> bytes)
        {
            if (peer.dead) { return; }
            if (peer.out.size() + bytes.size() > max_send_backlog)
            {
                drop(peer, "not reading, send backlog full");
                return;
            }
            peer.out.insert(peer.out.end(), bytes.begin(), bytes.end());
            flush(peer);
        }

        bool open_net(control_server_t& server)
        {
            net_control_t* net = new net_control_t;
            server.backend = net;

            sockaddr_in addr{};
            addr.sin_family = AF_INET;
            addr.sin_port = htons(server.port);
            addr.sin_addr.s_addr = htonl(INADDR_ANY);

            net->listen_fd = socket(AF_INET, SOCK_STREAM | SOCK_NONBLOCK | SOCK_CLOEXEC, 0);
            const int reuse = 1;
            setsockopt(net->listen_fd, SOL_SOCKET, SO_REUSEADDR, &reuse, sizeof(reuse));
            if (net->listen_fd < 0 || bind(net->listen_fd, reinterpret_cast<sockaddr*>(&addr), sizeof(addr)) != 0 ||
                listen(net->listen_fd, 8) != 0)
            {
                ROBV_LOG_ERROR("NET", "control port {}: {}", server.port, std::strerror(errno));
                return false;
            }
            ROBV_LOG_INFO("NET", "control server listening on 0.0.0.0:{}", server.port);
            return true;
        }

        void close_net(control_server_t& server)
        {
            net_control_t* net = static_cast<net_control_t*>(server.backend);
            if (!net) { return; }
            for (net_peer_t& peer : net->peers) { close_fd(peer.fd); }
            close_fd(net->listen_fd);
            delete net;
        }

        void accept_peers(net_control_t& net)
        {
            for (;;)
            {
                sockaddr_in addr{};
                socklen_t addr_size = sizeof(addr);
                const int fd = accept4(net.listen_fd, reinterpret_cast<sockaddr*>(&addr), &addr_size,
                                       SOCK_NONBLOCK | SOCK_CLOEXEC);
                if (fd < 0) { return; }

                char host[INET_ADDRSTRLEN] = {};
                inet_ntop(AF_INET, &addr.sin_addr, host, sizeof(host));
                const std::string address = std::string(host) + ":" + std::to_string(ntohs(addr.sin_port));
                if (net.peers.size() >= max_net_peers)
                {
                    ROBV_LOG_WARN("NET", "{} refused, already {} connections", address, net.peers.size());
                    ::close(fd);
                    continue;
                }

                const int no_delay = 1;
                setsockopt(fd, IPPROTO_TCP, TCP_NODELAY, &no_delay, sizeof(no_delay));
                net.peers.push_back({.fd = fd, .address = address, .ipv4 = addr.sin_addr.s_addr});
                ROBV_LOG_DEBUG("NET", "{} connected, waiting for hello", address);
            }
        }

        void receive(net_peer_t& peer)
        {
            u8 chunk[16 * 1024];
            while (!peer.dead)
            {
                const ssize_t n = recv(peer.fd, chunk, sizeof(chunk), 0);
                if (n > 0)
                {
                    peer.in.insert(peer.in.end(), chunk, chunk + n);
                    continue;
                }
                if (n < 0 && errno == EINTR) { continue; }
                if (n < 0 && (errno == EAGAIN || errno == EWOULDBLOCK)) { return; }
                drop(peer, n == 0 ? "closed by the client" : std::strerror(errno));
            }
        }

        // one frame from a peer, false drops it
        bool handle_frame(control_server_t& server, net_peer_t& peer, const frame_view_t& frame,
                          std::vector<control_event_t>& out)
        {
            if (peer.client_id == 0)
            {
                hello_msg_t hello;
                if (frame.type != msg_type_e::hello || !protocol::read(frame.payload, hello)) { return false; }

                peer.client_id = server.next_client_id++;
                server.peers.push_back({peer.client_id, hello.client_name, peer.ipv4, hello.video_port});
                ROBV_LOG_INFO("NET", "{} is client {} '{}'", peer.address, peer.client_id, hello.client_name);
                out.push_back({.kind = control_event_kind_e::connected, .client_id = peer.client_id});
                return true;
            }

            switch (frame.type)
            {
            case msg_type_e::input_intent:
            {
                control_event_t event{.kind = control_event_kind_e::intent, .client_id = peer.client_id};
                if (!protocol::read(frame.payload, event.intent)) { return false; }
                out.push_back(event);
                return true;
            }
            case msg_type_e::ping:
            {
                control_event_t event{.kind = control_event_kind_e::ping, .client_id = peer.client_id};
                if (!protocol::read(frame.payload, event.ping)) { return false; }
                out.push_back(event);
                return true;
            }
            case msg_type_e::request_keyframe:
            {
                keyframe_request_msg_t msg;
                if (!protocol::read(frame.payload, msg)) { return false; }
                out.push_back({.kind = control_event_kind_e::keyframe_request, .client_id = peer.client_id});
                return true;
            }
            default: return false;
            }
        }

        void parse_frames(control_server_t& server, net_peer_t& peer, std::vector<control_event_t>& out)
        {
            size_t offset = 0;
            while (!peer.dead)
            {
                frame_view_t frame;
                size_t consumed = 0;
                const parse_result_e result =
                    protocol::next_frame(std::span<const u8>(peer.in).subspan(offset), frame, consumed);
                if (result == parse_result_e::need_more) { break; }
                if (result != parse_result_e::ok)
                {
                    drop(peer, protocol::parse_result_name(result));
                    break;
                }
                offset += consumed;
                if (!handle_frame(server, peer, frame, out))
                {
                    ROBV_LOG_WARN("NET", "{} sent an unexpected or malformed {}", peer.address,
                                  protocol::msg_type_name(frame.type));
                    drop(peer, "bad message");
                }
            }
            peer.in.erase(peer.in.begin(), peer.in.begin() + static_cast<std::ptrdiff_t>(offset));
        }

        void poll_net(control_server_t& server, std::vector<control_event_t>& out)
        {
            net_control_t* net = static_cast<net_control_t*>(server.backend);
            accept_peers(*net);

            for (net_peer_t& peer : net->peers)
            {
                receive(peer);
                parse_frames(server, peer, out);
                flush(peer);
            }

            for (net_peer_t& peer : net->peers)
            {
                if (!peer.dead) { continue; }
                if (peer.client_id != 0)
                {
                    std::erase_if(server.peers, [&](const peer_t& p) { return p.client_id == peer.client_id; });
                    out.push_back({.kind = control_event_kind_e::disconnected, .client_id = peer.client_id});
                }
                close_fd(peer.fd);
            }
            std::erase_if(net->peers, [](const net_peer_t& peer) { return peer.dead; });
        }

        void poll_null(control_server_t& server, u64 now_ns, std::vector<control_event_t>& out)
        {
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
                zoom.intent = {.seq = ++sim->seq,
                               .kind = intent_kind_e::zoom,
                               .scalar = (sim->polls / 60) % 2 == 0 ? 0.3f : -0.3f};
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

            if (server.kind != transport_kind_e::net) { return; }
            for (net_peer_t& peer : static_cast<net_control_t*>(server.backend)->peers)
            {
                if (peer.client_id == client_id) { queue(peer, server.scratch); }
            }
        }
    } // namespace

    bool open_video_sink(video_sink_t& sink, transport_kind_e kind, const video_info_t& info)
    {
        sink = {};
        sink.kind = kind;
        sink.info = info;

        if (kind == transport_kind_e::net)
        {
            net_video_t* v = new net_video_t;
            sink.backend = v;
            v->packetizer.ssrc = info.ssrc;

            sockaddr_in addr{};
            addr.sin_family = AF_INET;
            addr.sin_port = htons(info.rtp_port);
            addr.sin_addr.s_addr = htonl(INADDR_ANY);
            v->fd = socket(AF_INET, SOCK_DGRAM | SOCK_NONBLOCK | SOCK_CLOEXEC, 0);
            const int reuse = 1;
            setsockopt(v->fd, SOL_SOCKET, SO_REUSEADDR, &reuse, sizeof(reuse));
            // a keyframe is a burst of ~100 packets per client, the default buffer drops some of them
            const int send_buffer = 4 * 1024 * 1024;
            setsockopt(v->fd, SOL_SOCKET, SO_SNDBUF, &send_buffer, sizeof(send_buffer));
            if (v->fd < 0 || bind(v->fd, reinterpret_cast<sockaddr*>(&addr), sizeof(addr)) != 0)
            {
                ROBV_LOG_ERROR("NET", "RTP port {}: {}", info.rtp_port, std::strerror(errno));
                return false;
            }
        }
        ROBV_LOG_INFO("NET", "{} video sink, {} {}x{}, RTP port {}, ssrc {:08x}", kind_name(kind),
                      video::codec_name(info.codec), info.width, info.height, info.rtp_port, info.ssrc);
        return true;
    }

    void close_video_sink(video_sink_t& sink)
    {
        if (sink.kind == transport_kind_e::net && sink.backend)
        {
            net_video_t* v = static_cast<net_video_t*>(sink.backend);
            if (v->fd >= 0) { ::close(v->fd); }
            delete v;
        }
        sink = {};
    }

    void add_video_peer(video_sink_t& sink, u32 client_id, u32 ipv4, u16 video_port)
    {
        if (std::find(sink.peers.begin(), sink.peers.end(), client_id) == sink.peers.end())
        {
            sink.peers.push_back(client_id);
        }
        if (sink.kind == transport_kind_e::net)
        {
            net_video_t* v = static_cast<net_video_t*>(sink.backend);
            std::erase_if(v->peers, [&](const video_peer_t& p) { return p.client_id == client_id; });
            video_peer_t& peer = v->peers.emplace_back(video_peer_t{.client_id = client_id, .ipv4 = ipv4});
            if (ipv4 != 0 && video_port != 0)
            {
                peer.address.sin_family = AF_INET;
                peer.address.sin_addr.s_addr = ipv4;
                peer.address.sin_port = htons(video_port);
                peer.reachable = true;
                char host[INET_ADDRSTRLEN] = {};
                inet_ntop(AF_INET, &peer.address.sin_addr, host, sizeof(host));
                ROBV_LOG_INFO("NET", "video to client {} at {}:{}", client_id, host, video_port);
            }
        }
    }

    void remove_video_peer(video_sink_t& sink, u32 client_id)
    {
        sink.peers.erase(std::remove(sink.peers.begin(), sink.peers.end(), client_id), sink.peers.end());
        if (sink.kind == transport_kind_e::net)
        {
            net_video_t* v = static_cast<net_video_t*>(sink.backend);
            std::erase_if(v->peers, [&](const video_peer_t& p) { return p.client_id == client_id; });
        }
    }

    // one packetization, the same datagrams to every reachable client
    void send_video(video_sink_t& sink, const encoded_packet_t& packet)
    {
        if (sink.kind != transport_kind_e::net)
        {
            sink.packets += sink.peers.size();
            sink.bytes += packet.data.size() * sink.peers.size();
            return;
        }

        net_video_t* v = static_cast<net_video_t*>(sink.backend);
        rtp::packetize(v->packetizer, packet.data, rtp::timestamp(packet.pts_ns), v->packets);
        for (const video_peer_t& peer : v->peers)
        {
            if (!peer.reachable) { continue; }
            u32 begin = 0;
            for (const u32 end : v->packets.ends)
            {
                const ssize_t sent = sendto(v->fd, v->packets.bytes.data() + begin, end - begin, MSG_DONTWAIT,
                                            reinterpret_cast<const sockaddr*>(&peer.address), sizeof(peer.address));
                if (sent < 0) { ++v->dropped; }
                else
                {
                    ++sink.packets;
                    sink.bytes += static_cast<u64>(sent);
                }
                begin = end;
            }
        }
    }

    bool poll_video(video_sink_t& sink)
    {
        if (sink.kind != transport_kind_e::net) { return false; }
        net_video_t* v = static_cast<net_video_t*>(sink.backend);

        bool joined = false;
        for (;;)
        {
            u8 datagram[64];
            sockaddr_in from{};
            socklen_t from_size = sizeof(from);
            const ssize_t n = recvfrom(v->fd, datagram, sizeof(datagram), MSG_DONTWAIT,
                                       reinterpret_cast<sockaddr*>(&from), &from_size);
            if (n < 0) { break; }

            u32 client_id = 0;
            if (!rtp::read_punch(std::span<const u8>(datagram, static_cast<size_t>(n)), client_id)) { continue; }
            for (video_peer_t& peer : v->peers)
            {
                // only from the host the control connection came from
                if (peer.client_id != client_id || (peer.ipv4 != 0 && peer.ipv4 != from.sin_addr.s_addr)) { continue; }
                const bool moved =
                    peer.address.sin_port != from.sin_port || peer.address.sin_addr.s_addr != from.sin_addr.s_addr;
                if (peer.reachable && !moved) { continue; }

                char host[INET_ADDRSTRLEN] = {};
                inet_ntop(AF_INET, &from.sin_addr, host, sizeof(host));
                ROBV_LOG_INFO("NET", "video to client {} at {}:{}", client_id, host, ntohs(from.sin_port));
                peer.address = from;
                peer.reachable = true;
                joined = true;
            }
        }
        return joined;
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
        case transport_kind_e::net: return open_net(server);
        }
        return false;
    }

    void close_control(control_server_t& server)
    {
        switch (server.kind)
        {
        case transport_kind_e::null: delete static_cast<null_control_t*>(server.backend); break;
        case transport_kind_e::net: close_net(server); break;
        }
        server.backend = nullptr;
        server.peers.clear();
    }

    void poll_control(control_server_t& server, u64 now_ns, std::vector<control_event_t>& out)
    {
        if (!server.backend) { return; }
        switch (server.kind)
        {
        case transport_kind_e::null: poll_null(server, now_ns, out); break;
        case transport_kind_e::net: poll_net(server, out); break;
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

        if (server.kind != transport_kind_e::net) { return; }
        for (net_peer_t& peer : static_cast<net_control_t*>(server.backend)->peers)
        {
            if (peer.client_id != 0) { queue(peer, server.scratch); }
        }
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
