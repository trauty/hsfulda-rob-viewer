#include "robv/server/source.h"

#include "robv/common/log.h"
#include "robv/common/protocol.h"

#include <algorithm>
#include <arpa/inet.h>
#include <cerrno>
#include <cmath>
#include <cstring>
#include <fcntl.h>
#include <memory>
#include <netinet/in.h>
#include <sys/socket.h>
#include <unistd.h>

#if defined(ROBV_WITH_MCAP)
    #include <mcap/reader.hpp>
#endif

namespace robv::source
{
    namespace
    {
        struct synthetic_t
        {
            point_cloud_frame_t frame;
            u64 period_ns = 0;
            u64 next_ns = 0;
            u64 stamp_ns = 0;
        };

        // fibonacci sphere
        // x y z intensity float32, 16 byte stride
        void make_sphere(point_cloud_frame_t& frame, u32 points, f32 radius)
        {
            constexpr f32 golden_angle = 2.39996323f;
            constexpr u32 stride = 16;

            std::shared_ptr<std::vector<std::byte>> blob =
                std::make_shared<std::vector<std::byte>>(static_cast<size_t>(points) * stride);
            for (u32 i = 0; i < points; ++i)
            {
                const f32 t = points > 1 ? static_cast<f32>(i) / static_cast<f32>(points - 1) : 0.5f;
                const f32 z = 1.0f - 2.0f * t;
                const f32 r = std::sqrt(std::max(0.0f, 1.0f - z * z));
                const f32 phi = golden_angle * static_cast<f32>(i);
                const f32 values[4] = {radius * r * std::cos(phi), radius * r * std::sin(phi), radius * z, 255.0f * t};
                std::memcpy(blob->data() + static_cast<size_t>(i) * stride, values, sizeof(values));
            }

            frame = {};
            frame.frame_id = "map";
            frame.width = points;
            frame.height = 1;
            frame.point_stride = stride;
            frame.row_stride = points * stride;
            frame.fields = {
                {"x",         0,  field_type_e::float32, 1},
                {"y",         4,  field_type_e::float32, 1},
                {"z",         8,  field_type_e::float32, 1},
                {"intensity", 12, field_type_e::float32, 1},
            };
            frame.data = std::move(blob);
        }

        bool open_synthetic(source_t& source)
        {
            const std::string shape(source.uri.authority.empty() ? "sphere" : source.uri.authority);
            if (shape != "sphere")
            {
                ROBV_LOG_ERROR("SOURCE", "synthetic://{} is not a shape, only sphere exists", shape);
                return false;
            }

            const u64 points = std::clamp<u64>(uri::query_u64(source.uri, "points", 200000), 1, 50'000'000);
            const f64 rate = std::clamp(uri::query_f64(source.uri, "rate", 10.0), 0.1, 1000.0);
            const f64 radius = uri::query_f64(source.uri, "radius", 10.0);

            synthetic_t* s = new synthetic_t;
            make_sphere(s->frame, static_cast<u32>(points), static_cast<f32>(radius));
            s->period_ns = static_cast<u64>(1e9 / rate);
            source.backend = s;

            source.caps.live = true;
            source.caps.pausable = true;
            source.caps.topics = {"/synthetic/" + shape};

            ROBV_LOG_INFO("SOURCE", "synthetic {} with {} points at {} Hz", shape, points, rate);
            return true;
        }

        // blob is shared, only the stamp moves
        bool poll_synthetic(source_t& source, u64 now_ns, point_cloud_frame_t& out)
        {
            synthetic_t* s = static_cast<synthetic_t*>(source.backend);
            if (source.paused || now_ns < s->next_ns) { return false; }

            // fixed rate, a stall or the first poll restarts the schedule instead of bursting
            s->next_ns += s->period_ns;
            if (s->next_ns <= now_ns) { s->next_ns = now_ns + s->period_ns; }
            s->stamp_ns = now_ns;
            out = s->frame;
            out.stamp_ns = s->stamp_ns;
            return true;
        }

        // mcap: reads the summary only, so topics and time range are actually real
        // message decoding still to do

#if defined(ROBV_WITH_MCAP)
        struct mcap_source_t
        {
            mcap::McapReader reader;
            std::string topic;
            bool warned = false;
        };

        bool open_mcap(source_t& source)
        {
            mcap_source_t* m = new mcap_source_t;
            source.backend = m;

            const mcap::Status opened = m->reader.open(source.uri.path);
            if (!opened.ok())
            {
                ROBV_LOG_ERROR("SOURCE", "mcap {}: {}", source.uri.path, opened.message);
                return false;
            }
            const mcap::Status summary = m->reader.readSummary(mcap::ReadSummaryMethod::AllowFallbackScan);
            if (!summary.ok())
            {
                ROBV_LOG_ERROR("SOURCE", "mcap {} summary: {}", source.uri.path, summary.message);
                return false;
            }

            using schema_map_t = std::unordered_map<mcap::SchemaId, mcap::SchemaPtr>;
            using channel_map_t = std::unordered_map<mcap::ChannelId, mcap::ChannelPtr>;
            const schema_map_t schemas = m->reader.schemas();
            const channel_map_t channels = m->reader.channels();
            for (const channel_map_t::value_type& entry : channels)
            {
                const mcap::ChannelPtr& channel = entry.second;
                const schema_map_t::const_iterator schema = schemas.find(channel->schemaId);
                if (schema != schemas.end() && schema->second->name == "sensor_msgs/msg/PointCloud2")
                {
                    source.caps.topics.push_back(channel->topic);
                }
            }
            std::sort(source.caps.topics.begin(), source.caps.topics.end());

            if (const std::optional<mcap::Statistics>& stats = m->reader.statistics())
            {
                source.caps.begin_ns = stats->messageStartTime;
                source.caps.end_ns = stats->messageEndTime;
            }
            source.caps.seekable = true;
            source.caps.pausable = true;

            m->topic = std::string(uri::query_value(source.uri, "topic"));
            if (m->topic.empty() && !source.caps.topics.empty()) { m->topic = source.caps.topics.front(); }

            ROBV_LOG_INFO("SOURCE", "mcap {}: {} PointCloud2 topics, {:.1f} s, using '{}'", source.uri.path,
                          source.caps.topics.size(), static_cast<f64>(source.caps.end_ns - source.caps.begin_ns) * 1e-9,
                          m->topic);
            return true;
        }

        bool poll_mcap(source_t& source, u64, point_cloud_frame_t&)
        {
            mcap_source_t* m = static_cast<mcap_source_t*>(source.backend);
            if (!m->warned)
            {
                ROBV_LOG_WARN("SOURCE", "mcap playback is not implemented yet, the source only reports its summary");
                m->warned = true;
            }
            return false;
        }

        void close_mcap(source_t& source)
        {
            mcap_source_t* m = static_cast<mcap_source_t*>(source.backend);
            m->reader.close();
            delete m;
        }
#endif

        // socket: listens for cloud_frame messages from the bridge or the robot
        struct socket_source_t
        {
            int listen_fd = -1;
            int peer_fd = -1;
            std::vector<u8> buffer;
        };

        void close_fd(int& fd)
        {
            if (fd >= 0) { ::close(fd); }
            fd = -1;
        }

        bool open_socket(source_t& source)
        {
            std::string host;
            u16 port = 0;
            if (!uri::split_host_port(source.uri.authority, host, port))
            {
                ROBV_LOG_ERROR("SOURCE", "socket:// needs host:port, got '{}'", source.uri.authority);
                return false;
            }

            sockaddr_in addr{};
            addr.sin_family = AF_INET;
            addr.sin_port = htons(port);
            if (inet_pton(AF_INET, host.c_str(), &addr.sin_addr) != 1)
            {
                ROBV_LOG_ERROR("SOURCE", "socket:// host must be an IPv4 address, got '{}'", host);
                return false;
            }

            socket_source_t* s = new socket_source_t;
            source.backend = s;

            s->listen_fd = socket(AF_INET, SOCK_STREAM | SOCK_NONBLOCK | SOCK_CLOEXEC, 0);
            const int reuse = 1;
            setsockopt(s->listen_fd, SOL_SOCKET, SO_REUSEADDR, &reuse, sizeof(reuse));
            if (s->listen_fd < 0 || bind(s->listen_fd, reinterpret_cast<sockaddr*>(&addr), sizeof(addr)) != 0 ||
                listen(s->listen_fd, 1) != 0)
            {
                ROBV_LOG_ERROR("SOURCE", "socket {}:{}: {}", host, port, std::strerror(errno));
                return false;
            }

            source.caps.live = true;
            ROBV_LOG_INFO("SOURCE", "socket listening on {}:{}", host, port);
            return true;
        }

        bool poll_socket(source_t& source, u64, point_cloud_frame_t& out)
        {
            socket_source_t* s = static_cast<socket_source_t*>(source.backend);

            if (s->peer_fd < 0)
            {
                s->peer_fd = accept4(s->listen_fd, nullptr, nullptr, SOCK_NONBLOCK | SOCK_CLOEXEC);
                if (s->peer_fd < 0) { return false; }
                s->buffer.clear();
                ROBV_LOG_INFO("SOURCE", "socket sender connected");
            }

            u8 chunk[64 * 1024];
            for (;;)
            {
                const ssize_t n = recv(s->peer_fd, chunk, sizeof(chunk), 0);
                if (n > 0)
                {
                    s->buffer.insert(s->buffer.end(), chunk, chunk + n);
                    continue;
                }
                if (n < 0 && (errno == EAGAIN || errno == EWOULDBLOCK || errno == EINTR)) { break; }
                ROBV_LOG_INFO("SOURCE", "socket sender disconnected");
                close_fd(s->peer_fd);
                break;
            }

            bool got = false;
            size_t offset = 0;
            for (;;)
            {
                frame_view_t view;
                size_t consumed = 0;
                const std::span<const u8> rest = std::span<const u8>(s->buffer).subspan(offset);
                const parse_result_e result = protocol::next_frame(rest, view, consumed);
                if (result == parse_result_e::need_more) { break; }
                if (result != parse_result_e::ok)
                {
                    ROBV_LOG_WARN("SOURCE", "socket sender sent a bad frame ({}), dropping it",
                                  protocol::parse_result_name(result));
                    close_fd(s->peer_fd);
                    s->buffer.clear();
                    return got;
                }
                offset += consumed;

                if (view.type != msg_type_e::cloud_frame) { continue; }
                if (protocol::read(view.payload, out)) { got = true; }
                else
                {
                    ROBV_LOG_WARN("SOURCE", "socket sender sent a malformed cloud_frame");
                }
            }
            s->buffer.erase(s->buffer.begin(), s->buffer.begin() + static_cast<std::ptrdiff_t>(offset));
            return got;
        }

        void close_socket(source_t& source)
        {
            socket_source_t* s = static_cast<socket_source_t*>(source.backend);
            close_fd(s->peer_fd);
            close_fd(s->listen_fd);
            delete s;
        }
    } // namespace

    bool open(source_t& source, std::string_view uri)
    {
        source = {};
        source.uri_text = uri;
        if (!uri::parse(uri, source.uri))
        {
            ROBV_LOG_ERROR("SOURCE", "'{}' is not a URI (scheme://...)", uri);
            return false;
        }

        bool ok = false;
        if (source.uri.scheme == "synthetic")
        {
            source.kind = source_kind_e::synthetic;
            ok = open_synthetic(source);
        }
        else if (source.uri.scheme == "mcap")
        {
            source.kind = source_kind_e::mcap;
#if defined(ROBV_WITH_MCAP)
            ok = open_mcap(source);
#else
            ROBV_LOG_ERROR("SOURCE", "this build has no MCAP support, see ROBV_WITH_MCAP");
#endif
        }
        else if (source.uri.scheme == "socket")
        {
            source.kind = source_kind_e::socket;
            ok = open_socket(source);
        }
        else
        {
            ROBV_LOG_ERROR("SOURCE", "unknown source scheme '{}', use synthetic, mcap or socket", source.uri.scheme);
        }

        if (!ok) { close(source); }
        return ok;
    }

    void close(source_t& source)
    {
        if (source.backend)
        {
            switch (source.kind)
            {
            case source_kind_e::synthetic: delete static_cast<synthetic_t*>(source.backend); break;
            case source_kind_e::mcap:
#if defined(ROBV_WITH_MCAP)
                close_mcap(source);
#endif
                break;
            case source_kind_e::socket: close_socket(source); break;
            }
        }
        source.backend = nullptr;
    }

    bool poll(source_t& source, u64 now_ns, point_cloud_frame_t& out)
    {
        if (!source.backend) { return false; }

        bool got = false;
        switch (source.kind)
        {
        case source_kind_e::synthetic: got = poll_synthetic(source, now_ns, out); break;
        case source_kind_e::mcap:
#if defined(ROBV_WITH_MCAP)
            got = poll_mcap(source, now_ns, out);
#endif
            break;
        case source_kind_e::socket: got = poll_socket(source, now_ns, out); break;
        }
        if (got) { ++source.frames; }
        return got;
    }

    bool seek(source_t& source, u64 stamp_ns)
    {
        if (!source.caps.seekable) { return false; }
        ROBV_LOG_DEBUG("SOURCE", "seek to {} ns", stamp_ns);
        return true;
    }

    void set_paused(source_t& source, bool paused)
    {
        if (source.caps.pausable) { source.paused = paused; }
    }

    const char* kind_name(source_kind_e kind)
    {
        switch (kind)
        {
        case source_kind_e::synthetic: return "synthetic";
        case source_kind_e::mcap: return "mcap";
        case source_kind_e::socket: return "socket";
        }
        return "unknown";
    }
} // namespace robv::source
