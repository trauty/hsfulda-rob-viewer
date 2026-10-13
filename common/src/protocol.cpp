#include "robv/common/protocol.h"

#include <algorithm>
#include <bit>
#include <cstring>
#include <limits>

namespace robv::protocol
{
    namespace
    {

        void put_u8(std::vector<u8>& out, u8 value) { out.push_back(value); }

        void put_u16(std::vector<u8>& out, u16 value)
        {
            out.push_back(static_cast<u8>(value));
            out.push_back(static_cast<u8>(value >> 8));
        }

        void put_u32(std::vector<u8>& out, u32 value)
        {
            for (u32 shift = 0; shift < 32; shift += 8) { out.push_back(static_cast<u8>(value >> shift)); }
        }

        void put_u64(std::vector<u8>& out, u64 value)
        {
            for (u32 shift = 0; shift < 64; shift += 8) { out.push_back(static_cast<u8>(value >> shift)); }
        }

        void put_f32(std::vector<u8>& out, f32 value) { put_u32(out, std::bit_cast<u32>(value)); }

        void put_bool(std::vector<u8>& out, bool value) { put_u8(out, value ? 1 : 0); }

        // strings beyond what a u16 counts are cut, none are that long legitimately
        void put_string(std::vector<u8>& out, const std::string& value)
        {
            const size_t length = std::min<size_t>(value.size(), std::numeric_limits<u16>::max());
            put_u16(out, static_cast<u16>(length));
            out.insert(out.end(), value.begin(), value.begin() + static_cast<std::ptrdiff_t>(length));
        }

        void put_vec3(std::vector<u8>& out, const vec3_t& value)
        {
            put_f32(out, value.x);
            put_f32(out, value.y);
            put_f32(out, value.z);
        }

        void put_field(std::vector<u8>& out, const point_field_t& field)
        {
            put_string(out, field.name);
            put_u32(out, field.offset);
            put_u8(out, static_cast<u8>(field.type));
            put_u32(out, field.count);
        }

        size_t begin_frame(std::vector<u8>& out, msg_type_e type)
        {
            const size_t start = out.size();
            put_u32(out, 0);
            put_u8(out, protocol_version);
            put_u8(out, static_cast<u8>(type));
            return start;
        }

        void end_frame(std::vector<u8>& out, size_t start)
        {
            const u32 length = static_cast<u32>(out.size() - start - 4);
            for (u32 i = 0; i < 4; ++i) { out[start + i] = static_cast<u8>(length >> (i * 8)); }
        }

        // get_* past the end flips ok and returns zero
        // a message reads straight through, checked once at the end
        struct reader_t
        {
            std::span<const u8> bytes;
            size_t pos = 0;
            bool ok = true;
        };

        const u8* take(reader_t& r, size_t count)
        {
            if (!r.ok || r.bytes.size() - r.pos < count)
            {
                r.ok = false;
                return nullptr;
            }
            const u8* at = r.bytes.data() + r.pos;
            r.pos += count;
            return at;
        }

        u8 get_u8(reader_t& r)
        {
            const u8* at = take(r, 1);
            return at ? at[0] : 0;
        }

        u16 get_u16(reader_t& r)
        {
            const u8* at = take(r, 2);
            return at ? static_cast<u16>(at[0] | (at[1] << 8)) : 0;
        }

        u32 get_u32(reader_t& r)
        {
            const u8* at = take(r, 4);
            if (!at) { return 0; }
            u32 value = 0;
            for (u32 i = 0; i < 4; ++i) { value |= static_cast<u32>(at[i]) << (i * 8); }
            return value;
        }

        u64 get_u64(reader_t& r)
        {
            const u8* at = take(r, 8);
            if (!at) { return 0; }
            u64 value = 0;
            for (u32 i = 0; i < 8; ++i) { value |= static_cast<u64>(at[i]) << (i * 8); }
            return value;
        }

        f32 get_f32(reader_t& r) { return std::bit_cast<f32>(get_u32(r)); }

        bool get_bool(reader_t& r)
        {
            const u8 value = get_u8(r);
            if (value > 1) { r.ok = false; }
            return value == 1;
        }

        std::string get_string(reader_t& r)
        {
            const u16 length = get_u16(r);
            const u8* at = take(r, length);
            return at ? std::string(reinterpret_cast<const char*>(at), length) : std::string();
        }

        vec3_t get_vec3(reader_t& r)
        {
            vec3_t value;
            value.x = get_f32(r);
            value.y = get_f32(r);
            value.z = get_f32(r);
            return value;
        }

        point_field_t get_field(reader_t& r)
        {
            point_field_t field;
            field.name = get_string(r);
            field.offset = get_u32(r);
            const u8 type = get_u8(r);
            field.count = get_u32(r);
            if (!point_cloud::is_valid_field_type(type)) { r.ok = false; }
            field.type = static_cast<field_type_e>(type);
            return field;
        }

        // trailing bytes are an error too, a version bump is how a message grows
        bool finish(const reader_t& r) { return r.ok && r.pos == r.bytes.size(); }

        bool is_known_type(u8 value)
        {
            switch (static_cast<msg_type_e>(value))
            {
            case msg_type_e::hello:
            case msg_type_e::input_intent:
            case msg_type_e::ping:
            case msg_type_e::welcome:
            case msg_type_e::state_snapshot:
            case msg_type_e::source_info:
            case msg_type_e::pong:
            case msg_type_e::cloud_frame: return true;
            }
            return false;
        }
    } // namespace

    void write(std::vector<u8>& out, const hello_msg_t& msg)
    {
        const size_t start = begin_frame(out, msg_type_e::hello);
        put_u8(out, msg.protocol_version);
        put_string(out, msg.client_name);
        end_frame(out, start);
    }

    void write(std::vector<u8>& out, const input_intent_t& msg)
    {
        const size_t start = begin_frame(out, msg_type_e::input_intent);
        put_u32(out, msg.seq);
        put_u8(out, static_cast<u8>(msg.kind));
        put_f32(out, msg.dx);
        put_f32(out, msg.dy);
        put_f32(out, msg.scalar);
        end_frame(out, start);
    }

    void write(std::vector<u8>& out, const ping_msg_t& msg)
    {
        const size_t start = begin_frame(out, msg_type_e::ping);
        put_u64(out, msg.t_client_ns);
        end_frame(out, start);
    }

    void write(std::vector<u8>& out, const welcome_msg_t& msg)
    {
        const size_t start = begin_frame(out, msg_type_e::welcome);
        put_u32(out, msg.client_id);
        put_u8(out, msg.protocol_version);
        put_u8(out, static_cast<u8>(msg.video.codec));
        put_u16(out, msg.video.width);
        put_u16(out, msg.video.height);
        put_u16(out, msg.video.rtp_port);
        put_u32(out, msg.video.ssrc);
        end_frame(out, start);
    }

    void write(std::vector<u8>& out, const state_snapshot_msg_t& msg)
    {
        const view_state_t& s = msg.state;
        const size_t start = begin_frame(out, msg_type_e::state_snapshot);

        put_u64(out, s.tick);

        put_vec3(out, s.camera.target);
        put_f32(out, s.camera.yaw);
        put_f32(out, s.camera.pitch);
        put_f32(out, s.camera.distance);
        put_f32(out, s.camera.fov_y);
        put_f32(out, s.camera.near_plane);
        put_f32(out, s.camera.far_plane);

        put_u8(out, static_cast<u8>(s.render.color_mode));
        put_f32(out, s.render.point_size);
        put_f32(out, s.render.intensity_min);
        put_f32(out, s.render.intensity_max);
        put_bool(out, s.render.show_grid);
        put_bool(out, s.render.show_axes);
        put_bool(out, s.render.eye_dome_lighting);

        put_bool(out, s.playback.playing);
        put_f32(out, s.playback.rate);
        put_u64(out, s.playback.cursor_ns);
        put_u64(out, s.playback.begin_ns);
        put_u64(out, s.playback.end_ns);

        put_string(out, s.active_topic);
        end_frame(out, start);
    }

    void write(std::vector<u8>& out, const source_info_msg_t& msg)
    {
        const size_t start = begin_frame(out, msg_type_e::source_info);

        const u16 topic_count = static_cast<u16>(std::min<size_t>(msg.topics.size(), 0xffff));
        put_u16(out, topic_count);
        for (u16 i = 0; i < topic_count; ++i) { put_string(out, msg.topics[i]); }

        const u16 field_count = static_cast<u16>(std::min<size_t>(msg.fields.size(), 0xffff));
        put_u16(out, field_count);
        for (u16 i = 0; i < field_count; ++i) { put_field(out, msg.fields[i]); }

        put_u64(out, msg.point_count);
        put_string(out, msg.source_uri);
        end_frame(out, start);
    }

    void write(std::vector<u8>& out, const pong_msg_t& msg)
    {
        const size_t start = begin_frame(out, msg_type_e::pong);
        put_u64(out, msg.t_client_ns);
        put_u64(out, msg.t_server_ns);
        end_frame(out, start);
    }

    void write(std::vector<u8>& out, const point_cloud_frame_t& msg)
    {
        const size_t start = begin_frame(out, msg_type_e::cloud_frame);

        put_u64(out, msg.stamp_ns);
        put_string(out, msg.frame_id);
        put_u32(out, msg.width);
        put_u32(out, msg.height);
        put_u32(out, msg.point_stride);
        put_u32(out, msg.row_stride);
        put_bool(out, msg.is_dense);
        put_bool(out, msg.little_endian);

        const u16 field_count = static_cast<u16>(std::min<size_t>(msg.fields.size(), 0xffff));
        put_u16(out, field_count);
        for (u16 i = 0; i < field_count; ++i) { put_field(out, msg.fields[i]); }

        const size_t size = msg.data ? msg.data->size() : 0;
        put_u32(out, static_cast<u32>(size));
        if (size > 0)
        {
            const size_t at = out.size();
            out.resize(at + size);
            std::memcpy(out.data() + at, msg.data->data(), size);
        }
        end_frame(out, start);
    }

    parse_result_e next_frame(std::span<const u8> bytes, frame_view_t& out, size_t& consumed)
    {
        consumed = 0;
        if (bytes.size() < 4) { return parse_result_e::need_more; }

        const u32 length = static_cast<u32>(bytes[0]) | (static_cast<u32>(bytes[1]) << 8) |
                           (static_cast<u32>(bytes[2]) << 16) | (static_cast<u32>(bytes[3]) << 24);

        // judged before waiting for the body, garbage lengths must not buffer gigabytes
        if (length > max_frame_size) { return parse_result_e::too_large; }
        if (length < frame_header_size - 4) { return parse_result_e::malformed; }
        if (bytes.size() - 4 < length) { return parse_result_e::need_more; }

        out.version = bytes[4];
        if (out.version != protocol_version) { return parse_result_e::bad_version; }
        if (!is_known_type(bytes[5])) { return parse_result_e::malformed; }

        out.type = static_cast<msg_type_e>(bytes[5]);
        out.payload = bytes.subspan(frame_header_size, length - (frame_header_size - 4));
        consumed = 4 + static_cast<size_t>(length);
        return parse_result_e::ok;
    }

    bool read(std::span<const u8> payload, hello_msg_t& out)
    {
        reader_t r{payload};
        out.protocol_version = get_u8(r);
        out.client_name = get_string(r);
        return finish(r);
    }

    bool read(std::span<const u8> payload, input_intent_t& out)
    {
        reader_t r{payload};
        out.seq = get_u32(r);
        const u8 kind = get_u8(r);
        out.dx = get_f32(r);
        out.dy = get_f32(r);
        out.scalar = get_f32(r);
        out.kind = static_cast<intent_kind_e>(kind);
        return finish(r) && view_state::is_valid_intent_kind(kind);
    }

    bool read(std::span<const u8> payload, ping_msg_t& out)
    {
        reader_t r{payload};
        out.t_client_ns = get_u64(r);
        return finish(r);
    }

    bool read(std::span<const u8> payload, welcome_msg_t& out)
    {
        reader_t r{payload};
        out.client_id = get_u32(r);
        out.protocol_version = get_u8(r);
        const u8 codec = get_u8(r);
        out.video.width = get_u16(r);
        out.video.height = get_u16(r);
        out.video.rtp_port = get_u16(r);
        out.video.ssrc = get_u32(r);
        out.video.codec = static_cast<video_codec_e>(codec);
        const bool known_codec =
            codec == static_cast<u8>(video_codec_e::h264) || codec == static_cast<u8>(video_codec_e::hevc);
        return finish(r) && known_codec;
    }

    bool read(std::span<const u8> payload, state_snapshot_msg_t& out)
    {
        view_state_t& s = out.state;
        reader_t r{payload};

        s.tick = get_u64(r);

        s.camera.target = get_vec3(r);
        s.camera.yaw = get_f32(r);
        s.camera.pitch = get_f32(r);
        s.camera.distance = get_f32(r);
        s.camera.fov_y = get_f32(r);
        s.camera.near_plane = get_f32(r);
        s.camera.far_plane = get_f32(r);

        const u8 color_mode = get_u8(r);
        s.render.color_mode = static_cast<color_mode_e>(color_mode);
        s.render.point_size = get_f32(r);
        s.render.intensity_min = get_f32(r);
        s.render.intensity_max = get_f32(r);
        s.render.show_grid = get_bool(r);
        s.render.show_axes = get_bool(r);
        s.render.eye_dome_lighting = get_bool(r);

        s.playback.playing = get_bool(r);
        s.playback.rate = get_f32(r);
        s.playback.cursor_ns = get_u64(r);
        s.playback.begin_ns = get_u64(r);
        s.playback.end_ns = get_u64(r);

        s.active_topic = get_string(r);
        return finish(r) && color_mode <= static_cast<u8>(color_mode_e::flat);
    }

    bool read(std::span<const u8> payload, source_info_msg_t& out)
    {
        reader_t r{payload};

        const u16 topic_count = get_u16(r);
        out.topics.clear();
        for (u16 i = 0; i < topic_count && r.ok; ++i) { out.topics.push_back(get_string(r)); }

        const u16 field_count = get_u16(r);
        out.fields.clear();
        for (u16 i = 0; i < field_count && r.ok; ++i) { out.fields.push_back(get_field(r)); }

        out.point_count = get_u64(r);
        out.source_uri = get_string(r);
        return finish(r);
    }

    bool read(std::span<const u8> payload, pong_msg_t& out)
    {
        reader_t r{payload};
        out.t_client_ns = get_u64(r);
        out.t_server_ns = get_u64(r);
        return finish(r);
    }

    bool read(std::span<const u8> payload, point_cloud_frame_t& out)
    {
        reader_t r{payload};

        out.stamp_ns = get_u64(r);
        out.frame_id = get_string(r);
        out.width = get_u32(r);
        out.height = get_u32(r);
        out.point_stride = get_u32(r);
        out.row_stride = get_u32(r);
        out.is_dense = get_bool(r);
        out.little_endian = get_bool(r);

        const u16 field_count = get_u16(r);
        out.fields.clear();
        for (u16 i = 0; i < field_count && r.ok; ++i) { out.fields.push_back(get_field(r)); }

        const u32 size = get_u32(r);
        const u8* data = take(r, size);
        if (!finish(r)) { return false; }

        // PointCloud2 invariants, so nothing downstream reads past the blob
        const u64 expected = static_cast<u64>(out.row_stride) * out.height;
        const bool row_fits = static_cast<u64>(out.point_stride) * out.width <= out.row_stride;
        if (expected != size || !row_fits) { return false; }

        std::shared_ptr<std::vector<std::byte>> blob = std::make_shared<std::vector<std::byte>>(size);
        if (size > 0) { std::memcpy(blob->data(), data, size); }
        out.data = std::move(blob);
        return true;
    }

    const char* msg_type_name(msg_type_e type)
    {
        switch (type)
        {
        case msg_type_e::hello: return "hello";
        case msg_type_e::input_intent: return "input_intent";
        case msg_type_e::ping: return "ping";
        case msg_type_e::welcome: return "welcome";
        case msg_type_e::state_snapshot: return "state_snapshot";
        case msg_type_e::source_info: return "source_info";
        case msg_type_e::pong: return "pong";
        case msg_type_e::cloud_frame: return "cloud_frame";
        }
        return "unknown";
    }

    const char* parse_result_name(parse_result_e result)
    {
        switch (result)
        {
        case parse_result_e::ok: return "ok";
        case parse_result_e::need_more: return "need_more";
        case parse_result_e::bad_version: return "bad_version";
        case parse_result_e::too_large: return "too_large";
        case parse_result_e::malformed: return "malformed";
        }
        return "unknown";
    }
} // namespace robv::protocol
