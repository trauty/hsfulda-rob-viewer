#pragma once

#include "robv/common/point_cloud.h"
#include "robv/common/types.h"
#include "robv/common/video.h"
#include "robv/common/view_state.h"

#include <span>
#include <string>
#include <vector>

// control channel and bridge feed: length prefixed TCP frames, little endian
//
//   frame   u32 length (of everything after it)  u8 version  u8 type  payload
//   string  u16 length  bytes
//   blob    u32 length  bytes
//   f32     IEEE 754 bits as u32
//
// video is out of band, H.264 or HEVC over RTP, see docs/protocol.md
namespace robv
{
    inline constexpr u8 protocol_version = 1;
    inline constexpr u32 max_frame_size = 64u * 1024u * 1024u;
    inline constexpr u32 frame_header_size = 6;

    enum class msg_type_e : u8
    {
        // client to server
        hello = 1,
        input_intent = 2,
        ping = 3,
        // server to client
        welcome = 16,
        state_snapshot = 17,
        source_info = 18,
        pong = 19,
        // bridge to server
        cloud_frame = 32
    };

    struct hello_msg_t
    {
        u8 protocol_version = robv::protocol_version;
        std::string client_name;

        bool operator==(const hello_msg_t&) const = default;
    };

    struct ping_msg_t
    {
        u64 t_client_ns = 0;

        bool operator==(const ping_msg_t&) const = default;
    };

    struct video_info_t
    {
        video_codec_e codec = video_codec_e::h264;
        u16 width = 0;
        u16 height = 0;
        u16 rtp_port = 0;
        u32 ssrc = 0;

        bool operator==(const video_info_t&) const = default;
    };

    struct welcome_msg_t
    {
        u32 client_id = 0;
        u8 protocol_version = robv::protocol_version;
        video_info_t video;

        bool operator==(const welcome_msg_t&) const = default;
    };

    struct state_snapshot_msg_t
    {
        view_state_t state;

        bool operator==(const state_snapshot_msg_t&) const = default;
    };

    struct source_info_msg_t
    {
        std::vector<std::string> topics;
        std::vector<point_field_t> fields;
        u64 point_count = 0;
        std::string source_uri;

        bool operator==(const source_info_msg_t&) const = default;
    };

    struct pong_msg_t
    {
        u64 t_client_ns = 0;
        u64 t_server_ns = 0;

        bool operator==(const pong_msg_t&) const = default;
    };

    // payload points into the caller's buffer
    struct frame_view_t
    {
        u8 version = 0;
        msg_type_e type = msg_type_e::hello;
        std::span<const u8> payload;
    };

    enum class parse_result_e : u8
    {
        ok,
        need_more,
        bad_version,
        too_large,
        malformed
    };
} // namespace robv

namespace robv::protocol
{
    // each appends one frame to out
    void write(std::vector<u8>& out, const hello_msg_t& msg);
    void write(std::vector<u8>& out, const input_intent_t& msg);
    void write(std::vector<u8>& out, const ping_msg_t& msg);
    void write(std::vector<u8>& out, const welcome_msg_t& msg);
    void write(std::vector<u8>& out, const state_snapshot_msg_t& msg);
    void write(std::vector<u8>& out, const source_info_msg_t& msg);
    void write(std::vector<u8>& out, const pong_msg_t& msg);
    void write(std::vector<u8>& out, const point_cloud_frame_t& msg);

    // ok: consumed is the frame size
    // need_more: read more and retry
    // anything else: close the connection
    parse_result_e next_frame(std::span<const u8> bytes, frame_view_t& out, size_t& consumed);

    // false on a truncated or malformed payload, out unspecified
    bool read(std::span<const u8> payload, hello_msg_t& out);
    bool read(std::span<const u8> payload, input_intent_t& out);
    bool read(std::span<const u8> payload, ping_msg_t& out);
    bool read(std::span<const u8> payload, welcome_msg_t& out);
    bool read(std::span<const u8> payload, state_snapshot_msg_t& out);
    bool read(std::span<const u8> payload, source_info_msg_t& out);
    bool read(std::span<const u8> payload, pong_msg_t& out);
    bool read(std::span<const u8> payload, point_cloud_frame_t& out);

    const char* msg_type_name(msg_type_e type);
    const char* parse_result_name(parse_result_e result);
} // namespace robv::protocol
