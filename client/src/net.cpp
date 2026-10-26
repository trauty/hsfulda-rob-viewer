#include "robv/client/net.h"

#include "robv/common/log.h"
#include "robv/common/uri.h"

#include <SDL3/SDL.h>
#include <algorithm>
#include <atomic>
#include <cerrno>
#include <chrono>
#include <cstring>
#include <mutex>
#include <netdb.h>
#include <netinet/in.h>
#include <netinet/tcp.h>
#include <poll.h>
#include <sys/eventfd.h>
#include <sys/socket.h>
#include <thread>
#include <unistd.h>
#include <vector>

namespace robv::net
{
    namespace
    {
        constexpr u64 ms_ns = 1'000'000ull;
        constexpr u64 ping_period_ns = 1000 * ms_ns;
        constexpr u64 silence_timeout_ns = 5000 * ms_ns;
        constexpr u64 connect_timeout_ns = 3000 * ms_ns;
        constexpr u64 min_backoff_ns = 500 * ms_ns;
        constexpr u64 max_backoff_ns = 5000 * ms_ns;
        // keeps the server's address for this client fresh
        constexpr u64 punch_period_ns = 1000 * ms_ns;
        constexpr u64 keyframe_request_gap_ns = 250 * ms_ns;

        constexpr size_t max_queued_units = 60;

        enum class phase_e : u8
        {
            idle,
            connecting,
            greeting,
            ready
        };

        struct backend_t
        {
            std::thread thread;
            int wake_fd = -1;
            u32 wake_event = 0;
            std::string client_name;
            std::atomic<bool> ui_wake_pending{false};

            std::mutex mutex;
            bool quit = false;
            bool want_connected = false;
            std::string target;
            u64 target_generation = 0;
            std::vector<u8> outbox;
            bool keyframe_wanted = false;

            connection_t shared;
            u64 version = 0;
            std::vector<rtp_unit_t> video;
        };

        // net thread only
        struct link_t
        {
            phase_e phase = phase_e::idle;
            int fd = -1;
            std::vector<u8> in;
            std::vector<u8> out;
            u64 generation = 0;
            bool want_connected = false;
            std::string target;
            u64 retry_at_ns = 0;
            u64 backoff_ns = min_backoff_ns;
            u64 connect_deadline_ns = 0;
            u64 last_receive_ns = 0;
            u64 next_ping_ns = 0;

            sockaddr_in server{};
            int video_fd = -1;
            u32 client_id = 0;
            rtp_depacketizer_t rtp;
            u64 next_punch_ns = 0;
            u64 last_keyframe_request_ns = 0;
            std::vector<u8> punch;
            // clock offset samples: a short round trip bounds the error by half of it
            f32 best_rtt_ms = 0.0f;
            u32 pongs_since_best = 0;
            std::vector<rtp_unit_t> units;
        };

        u64 steady_ns() { return clock_ns(); }

        void wake_net(backend_t& b)
        {
            const u64 one = 1;
            [[maybe_unused]] const ssize_t n = write(b.wake_fd, &one, sizeof(one));
        }

        void wake_ui(backend_t& b)
        {
            if (b.ui_wake_pending.exchange(true)) { return; }
            SDL_Event event{};
            event.type = b.wake_event;
            SDL_PushEvent(&event);
        }

        // edits the shared connection under the lock and tells the UI
        template <typename fn_t>
        void publish(backend_t& b, fn_t&& edit)
        {
            {
                std::lock_guard lock(b.mutex);
                edit(b.shared);
                b.version++;
            }
            wake_ui(b);
        }

        void close_link(link_t& link)
        {
            if (link.fd >= 0) { ::close(link.fd); }
            if (link.video_fd >= 0) { ::close(link.video_fd); }
            link.fd = -1;
            link.video_fd = -1;
            link.client_id = 0;
            link.rtp = {};
            link.units.clear();
            link.phase = phase_e::idle;
            link.in.clear();
            link.out.clear();
        }

        // drops the socket, the next attempt waits out the backoff
        void fail(backend_t& b, link_t& link, const std::string& reason, u64 now)
        {
            ROBV_LOG_INFO("NET", "{}: {}, retrying in {:.1f} s", link.target, reason,
                          static_cast<f64>(link.backoff_ns) * 1e-9);
            close_link(link);
            link.retry_at_ns = now + link.backoff_ns;
            const std::string status = reason + ", retrying";
            link.backoff_ns = std::min(link.backoff_ns * 2, max_backoff_ns);
            publish(b,
                    [&](connection_t& c)
                    {
                        c.state = connection_state_e::connecting;
                        c.status = status;
                        c.client_id = 0;
                    });
        }

        void start_connect(backend_t& b, link_t& link, u64 now)
        {
            std::string host;
            u16 port = 0;
            if (!uri::split_host_port(link.target, host, port))
            {
                link.want_connected = false;
                publish(b,
                        [&](connection_t& c)
                        {
                            c.state = connection_state_e::disconnected;
                            c.status = "expected host:port";
                        });
                return;
            }

            publish(b,
                    [&](connection_t& c)
                    {
                        c.state = connection_state_e::connecting;
                        c.status = "connecting";
                    });

            // the server listens on ipv4 only
            addrinfo hints{};
            hints.ai_family = AF_INET;
            hints.ai_socktype = SOCK_STREAM;
            addrinfo* found = nullptr;
            const int resolved = getaddrinfo(host.c_str(), std::to_string(port).c_str(), &hints, &found);
            if (resolved != 0 || !found)
            {
                fail(b, link, std::string("cannot resolve ") + host + ": " + gai_strerror(resolved), now);
                return;
            }

            std::memcpy(&link.server, found->ai_addr, sizeof(link.server));
            link.fd = socket(AF_INET, SOCK_STREAM | SOCK_NONBLOCK | SOCK_CLOEXEC, 0);
            const int connected = link.fd >= 0 ? ::connect(link.fd, found->ai_addr, found->ai_addrlen) : -1;
            const int error = errno;
            freeaddrinfo(found);
            if (link.fd < 0 || (connected != 0 && error != EINPROGRESS))
            {
                fail(b, link, std::strerror(error), now);
                return;
            }
            link.phase = phase_e::connecting;
            link.connect_deadline_ns = now + connect_timeout_ns;
        }

        // bound before hello, which carries the port: the server sends video there, outbound from its side
        // 0 when it fails, the server then waits for a punch instead
        u16 bind_video(link_t& link)
        {
            link.video_fd = socket(AF_INET, SOCK_DGRAM | SOCK_NONBLOCK | SOCK_CLOEXEC, 0);
            // a keyframe arrives as a burst of ~100 datagrams
            const int receive_buffer = 2 * 1024 * 1024;
            setsockopt(link.video_fd, SOL_SOCKET, SO_RCVBUF, &receive_buffer, sizeof(receive_buffer));
            sockaddr_in local{};
            local.sin_family = AF_INET;
            local.sin_addr.s_addr = htonl(INADDR_ANY);
            socklen_t size = sizeof(local);
            if (link.video_fd < 0 ||
                bind(link.video_fd, reinterpret_cast<const sockaddr*>(&local), sizeof(local)) != 0 ||
                getsockname(link.video_fd, reinterpret_cast<sockaddr*>(&local), &size) != 0)
            {
                ROBV_LOG_ERROR("NET", "video socket: {}", std::strerror(errno));
                return 0;
            }
            return ntohs(local.sin_port);
        }

        void finish_connect(backend_t& b, link_t& link, u64 now)
        {
            int error = 0;
            socklen_t size = sizeof(error);
            getsockopt(link.fd, SOL_SOCKET, SO_ERROR, &error, &size);
            if (error != 0)
            {
                fail(b, link, std::strerror(error), now);
                return;
            }

            const int no_delay = 1;
            setsockopt(link.fd, IPPROTO_TCP, TCP_NODELAY, &no_delay, sizeof(no_delay));
            link.phase = phase_e::greeting;
            link.last_receive_ns = now;
            link.next_ping_ns = now;
            const u16 video_port = bind_video(link);
            protocol::write(link.out, hello_msg_t{.client_name = b.client_name, .video_port = video_port});
            publish(b, [&](connection_t& c) { c.status = "waiting for welcome"; });
        }

        void request_keyframe_now(link_t& link, u64 now)
        {
            if (now - link.last_keyframe_request_ns < keyframe_request_gap_ns) { return; }
            link.last_keyframe_request_ns = now;
            protocol::write(link.out, keyframe_request_msg_t{});
        }

        void send_punch(link_t& link, u64 now)
        {
            rtp::write_punch(link.punch, link.client_id);
            [[maybe_unused]] const ssize_t n = send(link.video_fd, link.punch.data(), link.punch.size(), MSG_DONTWAIT);
            link.next_punch_ns = now + punch_period_ns;
        }

        bool open_video(link_t& link, const welcome_msg_t& msg, u64 now)
        {
            sockaddr_in rtp_address = link.server;
            rtp_address.sin_port = htons(msg.video.rtp_port);
            if (link.video_fd < 0 ||
                ::connect(link.video_fd, reinterpret_cast<const sockaddr*>(&rtp_address), sizeof(rtp_address)) != 0)
            {
                ROBV_LOG_ERROR("NET", "video socket: {}", std::strerror(errno));
                return false;
            }
            link.client_id = msg.client_id;
            link.rtp = {};
            link.rtp.ssrc = msg.video.ssrc;
            send_punch(link, now);
            return true;
        }

        // every datagram waiting, complete units go to the UI in one batch
        void receive_video(backend_t& b, link_t& link, u64 now)
        {
            u8 datagram[2048];
            for (;;)
            {
                const ssize_t n = recv(link.video_fd, datagram, sizeof(datagram), MSG_DONTWAIT);
                if (n < 0) { break; }
                bool lost = false;
                const u64 dropped = link.rtp.dropped;
                rtp::depacketize(link.rtp, std::span<const u8>(datagram, static_cast<size_t>(n)), link.units, lost);
                // still no IDR to start from, its packets may have gone before the socket was read
                const bool starved = link.rtp.waiting_for_idr && link.rtp.dropped != dropped;
                if (lost || starved) { request_keyframe_now(link, now); }
            }
            if (link.units.empty()) { return; }

            bool overflow = false;
            {
                std::lock_guard lock(b.mutex);
                for (rtp_unit_t& u : link.units) { b.video.push_back(std::move(u)); }
                // the UI is not taking frames, the old ones are useless and the decoder restarts at an IDR
                if (b.video.size() > max_queued_units)
                {
                    b.video.clear();
                    overflow = true;
                }
            }
            link.units.clear();
            if (overflow)
            {
                link.rtp.waiting_for_idr = true;
                request_keyframe_now(link, now);
            }
            wake_ui(b);
        }

        // false: the server sent something a client never receives
        bool handle_frame(backend_t& b, link_t& link, const frame_view_t& frame, u64 now)
        {
            switch (frame.type)
            {
            case msg_type_e::welcome:
            {
                welcome_msg_t msg;
                if (!protocol::read(frame.payload, msg)) { return false; }
                link.phase = phase_e::ready;
                link.backoff_ns = min_backoff_ns;
                if (!open_video(link, msg, now)) { return false; }
                ROBV_LOG_INFO("NET", "{}: client {}, {} {}x{}", link.target, msg.client_id,
                              video::codec_name(msg.video.codec), msg.video.width, msg.video.height);
                publish(b,
                        [&](connection_t& c)
                        {
                            c.state = connection_state_e::connected;
                            c.status = "connected";
                            c.client_id = msg.client_id;
                            c.video = msg.video;
                        });
                return true;
            }
            case msg_type_e::source_info:
            {
                source_info_msg_t msg;
                if (!protocol::read(frame.payload, msg)) { return false; }
                publish(b, [&](connection_t& c) { c.source = std::move(msg); });
                return true;
            }
            case msg_type_e::state_snapshot:
            {
                state_snapshot_msg_t msg;
                if (!protocol::read(frame.payload, msg)) { return false; }
                publish(b,
                        [&](connection_t& c)
                        {
                            c.view = std::move(msg.state);
                            c.has_view = true;
                        });
                return true;
            }
            case msg_type_e::pong:
            {
                pong_msg_t msg;
                if (!protocol::read(frame.payload, msg) || msg.t_client_ns > now) { return false; }
                const f32 rtt_ms = static_cast<f32>(now - msg.t_client_ns) * 1e-6f;
                // a fresh best sample, or an aged one replaced so a drifting clock is followed
                const bool better =
                    link.pongs_since_best >= 10 || link.best_rtt_ms == 0.0f || rtt_ms < link.best_rtt_ms;
                link.pongs_since_best = better ? 0 : link.pongs_since_best + 1;
                if (better) { link.best_rtt_ms = rtt_ms; }
                const i64 offset =
                    static_cast<i64>(msg.t_server_ns) - static_cast<i64>(msg.t_client_ns + (now - msg.t_client_ns) / 2);
                publish(b,
                        [&](connection_t& c)
                        {
                            c.rtt_ms = rtt_ms;
                            if (better)
                            {
                                c.clock_offset_ns = offset;
                                c.has_clock_offset = true;
                            }
                        });
                return true;
            }
            default: return false;
            }
        }

        void receive(backend_t& b, link_t& link, u64 now)
        {
            u8 chunk[16 * 1024];
            for (;;)
            {
                const ssize_t n = recv(link.fd, chunk, sizeof(chunk), 0);
                if (n > 0)
                {
                    link.in.insert(link.in.end(), chunk, chunk + n);
                    link.last_receive_ns = now;
                    continue;
                }
                if (n < 0 && errno == EINTR) { continue; }
                if (n < 0 && (errno == EAGAIN || errno == EWOULDBLOCK)) { break; }
                fail(b, link, n == 0 ? "server closed the connection" : std::strerror(errno), now);
                return;
            }

            size_t offset = 0;
            for (;;)
            {
                frame_view_t frame;
                size_t consumed = 0;
                const parse_result_e result =
                    protocol::next_frame(std::span<const u8>(link.in).subspan(offset), frame, consumed);
                if (result == parse_result_e::need_more) { break; }
                if (result != parse_result_e::ok)
                {
                    fail(b, link, std::string("bad frame from server: ") + protocol::parse_result_name(result), now);
                    return;
                }
                offset += consumed;
                if (!handle_frame(b, link, frame, now))
                {
                    fail(b, link, std::string("unexpected ") + protocol::msg_type_name(frame.type), now);
                    return;
                }
            }
            link.in.erase(link.in.begin(), link.in.begin() + static_cast<std::ptrdiff_t>(offset));
        }

        void flush(backend_t& b, link_t& link, u64 now)
        {
            size_t sent = 0;
            while (sent < link.out.size())
            {
                const ssize_t n = send(link.fd, link.out.data() + sent, link.out.size() - sent, MSG_NOSIGNAL);
                if (n > 0)
                {
                    sent += static_cast<size_t>(n);
                    continue;
                }
                if (n < 0 && errno == EINTR) { continue; }
                if (n < 0 && (errno == EAGAIN || errno == EWOULDBLOCK)) { break; }
                fail(b, link, std::strerror(errno), now);
                return;
            }
            link.out.erase(link.out.begin(), link.out.begin() + static_cast<std::ptrdiff_t>(sent));
        }

        // takes the UI's commands and queued intents, false on quit
        bool sync_commands(backend_t& b, link_t& link)
        {
            std::unique_lock lock(b.mutex);
            if (b.quit) { return false; }

            const bool retargeted = b.target_generation != link.generation;
            if (retargeted)
            {
                link.generation = b.target_generation;
                link.want_connected = b.want_connected;
                link.target = b.target;
                link.retry_at_ns = 0;
                link.backoff_ns = min_backoff_ns;
                close_link(link);
                b.shared.server = link.target;
                b.shared.client_id = 0;
                b.shared.has_view = false;
                b.shared.state = connection_state_e::disconnected;
                b.shared.status = "not connected";
                ++b.version;
            }

            // intents only mean something to the session that will apply them
            if (link.phase == phase_e::ready) { link.out.insert(link.out.end(), b.outbox.begin(), b.outbox.end()); }
            b.outbox.clear();
            const bool keyframe_wanted = b.keyframe_wanted;
            b.keyframe_wanted = false;
            if (retargeted) { b.video.clear(); }
            lock.unlock();

            // the UI only copies the connection on a wake, a cancel publishes nothing else
            if (retargeted) { wake_ui(b); }
            if (keyframe_wanted && link.phase == phase_e::ready)
            {
                link.rtp.waiting_for_idr = true;
                request_keyframe_now(link, steady_ns());
            }
            return true;
        }

        void run(backend_t* b)
        {
            link_t link;
            while (sync_commands(*b, link))
            {
                u64 now = steady_ns();
                if (link.want_connected && link.phase == phase_e::idle && now >= link.retry_at_ns)
                {
                    start_connect(*b, link, now);
                }

                const bool linked = link.phase == phase_e::greeting || link.phase == phase_e::ready;
                if (linked && now >= link.next_ping_ns)
                {
                    protocol::write(link.out, ping_msg_t{.t_client_ns = now});
                    link.next_ping_ns = now + ping_period_ns;
                }
                const bool video_open = link.video_fd >= 0 && link.phase == phase_e::ready;
                if (video_open && now >= link.next_punch_ns)
                {
                    send_punch(link, now);
                    const u64 units = link.rtp.units;
                    const u64 lost = link.rtp.lost;
                    publish(*b,
                            [&](connection_t& c)
                            {
                                c.video_units = units;
                                c.video_lost = lost;
                            });
                }
                if (linked && !link.out.empty()) { flush(*b, link, now); }

                // sleep until a socket, the UI or the next deadline needs attention
                u64 deadline = UINT64_MAX;
                if (link.want_connected && link.phase == phase_e::idle) { deadline = link.retry_at_ns; }
                if (link.phase == phase_e::connecting) { deadline = link.connect_deadline_ns; }
                if (link.phase == phase_e::greeting || link.phase == phase_e::ready)
                {
                    deadline = std::min(link.next_ping_ns, link.last_receive_ns + silence_timeout_ns);
                }
                if (video_open) { deadline = std::min(deadline, link.next_punch_ns); }
                const int timeout_ms =
                    deadline == UINT64_MAX
                        ? -1
                        : static_cast<int>(std::min<u64>((deadline > now ? deadline - now : 0) / ms_ns + 1, 60'000));

                pollfd fds[3] = {
                    {.fd = b->wake_fd, .events = POLLIN},
                    {.fd = link.fd},
                    // read only after welcome gave the SSRC, earlier datagrams wait in the socket
                    {.fd = video_open ? link.video_fd : -1, .events = POLLIN},
                };
                if (link.phase == phase_e::connecting) { fds[1].events = POLLOUT; }
                else if (link.fd >= 0)
                {
                    fds[1].events = static_cast<short>(POLLIN | (link.out.empty() ? 0 : POLLOUT));
                }
                const nfds_t watched = link.video_fd >= 0 ? 3 : (link.fd >= 0 ? 2 : 1);
                const int ready = ::poll(fds, watched, timeout_ms);
                if (ready < 0 && errno != EINTR)
                {
                    ROBV_LOG_ERROR("NET", "poll: {}", std::strerror(errno));
                    break;
                }

                if (fds[0].revents & POLLIN)
                {
                    u64 count = 0;
                    [[maybe_unused]] const ssize_t n = read(b->wake_fd, &count, sizeof(count));
                }

                now = steady_ns();
                if (link.phase == phase_e::connecting)
                {
                    if (fds[1].revents & (POLLOUT | POLLERR | POLLHUP)) { finish_connect(*b, link, now); }
                    else if (now >= link.connect_deadline_ns) { fail(*b, link, "no answer", now); }
                }
                else if (link.fd >= 0)
                {
                    if (link.video_fd >= 0 && (fds[2].revents & POLLIN)) { receive_video(*b, link, now); }
                    if (fds[1].revents & (POLLIN | POLLERR | POLLHUP)) { receive(*b, link, now); }
                    if (link.fd >= 0 && (fds[1].revents & POLLOUT)) { flush(*b, link, now); }
                    if (link.fd >= 0 && now - link.last_receive_ns > silence_timeout_ns)
                    {
                        fail(*b, link, "server stopped answering", now);
                    }
                }
            }
            close_link(link);
        }
    } // namespace

    bool start(net_t& net, std::string_view client_name, u32 wake_event)
    {
        net = {};
        backend_t* b = new backend_t;
        b->client_name = client_name;
        b->wake_event = wake_event;
        b->wake_fd = eventfd(0, EFD_CLOEXEC | EFD_NONBLOCK);
        if (b->wake_fd < 0)
        {
            ROBV_LOG_ERROR("NET", "eventfd: {}", std::strerror(errno));
            delete b;
            return false;
        }
        b->thread = std::thread(run, b);
        net.backend = b;
        return true;
    }

    void stop(net_t& net)
    {
        backend_t* b = static_cast<backend_t*>(net.backend);
        if (!b) { return; }
        {
            std::lock_guard lock(b->mutex);
            b->quit = true;
        }
        wake_net(*b);
        b->thread.join();
        ::close(b->wake_fd);
        delete b;
        net.backend = nullptr;
    }

    void connect(net_t& net, std::string_view server)
    {
        backend_t* b = static_cast<backend_t*>(net.backend);
        if (!b) { return; }
        {
            std::lock_guard lock(b->mutex);
            b->want_connected = true;
            b->target = server;
            ++b->target_generation;
        }
        wake_net(*b);
    }

    void disconnect(net_t& net)
    {
        backend_t* b = static_cast<backend_t*>(net.backend);
        if (!b) { return; }
        {
            std::lock_guard lock(b->mutex);
            b->want_connected = false;
            ++b->target_generation;
        }
        wake_net(*b);
    }

    void send_intent(net_t& net, input_intent_t intent)
    {
        backend_t* b = static_cast<backend_t*>(net.backend);
        if (!b) { return; }
        intent.seq = net.next_seq++;
        {
            std::lock_guard lock(b->mutex);
            if (b->shared.state != connection_state_e::connected) { return; }
            protocol::write(b->outbox, intent);
        }
        wake_net(*b);
    }

    void take_video(net_t& net, std::vector<rtp_unit_t>& out)
    {
        out.clear();
        backend_t* b = static_cast<backend_t*>(net.backend);
        if (!b) { return; }
        std::lock_guard lock(b->mutex);
        out.swap(b->video);
    }

    void request_keyframe(net_t& net)
    {
        backend_t* b = static_cast<backend_t*>(net.backend);
        if (!b) { return; }
        {
            std::lock_guard lock(b->mutex);
            b->keyframe_wanted = true;
        }
        wake_net(*b);
    }

    bool poll(net_t& net, connection_t& out)
    {
        backend_t* b = static_cast<backend_t*>(net.backend);
        if (!b) { return false; }
        b->ui_wake_pending.store(false);

        std::lock_guard lock(b->mutex);
        if (b->version == net.seen_version) { return false; }
        net.seen_version = b->version;
        out = b->shared;
        return true;
    }

    u64 clock_ns()
    {
        return static_cast<u64>(
            std::chrono::duration_cast<std::chrono::nanoseconds>(std::chrono::steady_clock::now().time_since_epoch())
                .count());
    }

    const char* state_name(connection_state_e state)
    {
        switch (state)
        {
        case connection_state_e::disconnected: return "disconnected";
        case connection_state_e::connecting: return "connecting";
        case connection_state_e::connected: return "connected";
        }
        return "unknown";
    }
} // namespace robv::net
