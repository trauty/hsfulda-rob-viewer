#include "socket_sender.h"

#include "robv/common/protocol.h"
#include "robv/common/uri.h"

#include <arpa/inet.h>
#include <cerrno>
#include <netinet/in.h>
#include <netinet/tcp.h>
#include <sys/socket.h>
#include <unistd.h>

namespace robv::socket_sender
{
    namespace
    {
        void disconnect(socket_sender_t& sender)
        {
            if (sender.fd >= 0) { ::close(sender.fd); }
            sender.fd = -1;
        }
    } // namespace

    bool init(socket_sender_t& sender, const std::string& server)
    {
        sender = {};
        in_addr probe{};
        return uri::split_host_port(server, sender.host, sender.port) &&
               inet_pton(AF_INET, sender.host.c_str(), &probe) == 1;
    }

    void shutdown(socket_sender_t& sender) { disconnect(sender); }

    bool ensure_connected(socket_sender_t& sender)
    {
        if (sender.fd >= 0) { return true; }

        sockaddr_in addr{};
        addr.sin_family = AF_INET;
        addr.sin_port = htons(sender.port);
        inet_pton(AF_INET, sender.host.c_str(), &addr.sin_addr);

        sender.fd = socket(AF_INET, SOCK_STREAM | SOCK_CLOEXEC, 0);
        if (sender.fd < 0) { return false; }

        // latency over packet count
        const int one = 1;
        setsockopt(sender.fd, IPPROTO_TCP, TCP_NODELAY, &one, sizeof(one));
        // a stalled server must not stall the ROS executor
        const timeval timeout{.tv_sec = 1, .tv_usec = 0};
        setsockopt(sender.fd, SOL_SOCKET, SO_SNDTIMEO, &timeout, sizeof(timeout));

        if (connect(sender.fd, reinterpret_cast<sockaddr*>(&addr), sizeof(addr)) != 0)
        {
            disconnect(sender);
            return false;
        }
        return true;
    }

    bool is_connected(const socket_sender_t& sender) { return sender.fd >= 0; }

    bool send(socket_sender_t& sender, const point_cloud_frame_t& frame)
    {
        if (!ensure_connected(sender))
        {
            ++sender.frames_dropped;
            return false;
        }

        sender.scratch.clear();
        protocol::write(sender.scratch, frame);

        size_t offset = 0;
        while (offset < sender.scratch.size())
        {
            const ssize_t n =
                ::send(sender.fd, sender.scratch.data() + offset, sender.scratch.size() - offset, MSG_NOSIGNAL);
            if (n < 0 && errno == EINTR) { continue; }
            if (n <= 0)
            {
                // a partial frame desyncs the stream, so the connection goes too
                disconnect(sender);
                ++sender.frames_dropped;
                return false;
            }
            offset += static_cast<size_t>(n);
        }

        ++sender.frames_sent;
        sender.bytes_sent += sender.scratch.size();
        return true;
    }
} // namespace robv::socket_sender
