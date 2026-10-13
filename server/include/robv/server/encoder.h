#pragma once

#include "robv/common/video.h"

#include <string_view>
#include <vector>

namespace robv
{
    enum class encoder_kind_e : u8
    {
        // counts frames, emits nothing
        null,
        // V4L2 M2M via the Jetson Multimedia API
        jetson,
    };

    struct encoder_config_t
    {
        encoder_kind_e kind = encoder_kind_e::null;
        video_codec_e codec = video_codec_e::h264;
        u32 width = 1280;
        u32 height = 720;
        u32 fps = 30;
        u32 bitrate_kbps = 8000;
        // 0: keyframes only on request (a late joiner asks)
        u32 idr_interval = 0;
    };

    // Annex B, one access unit
    struct encoded_packet_t
    {
        std::vector<u8> data;
        u64 pts_ns = 0;
        bool keyframe = false;
    };

    struct encoder_t
    {
        encoder_config_t config;
        bool keyframe_requested = true;
        u64 frames_in = 0;
        u64 packets_out = 0;
        u64 bytes_out = 0;
        void* backend = nullptr;
    };
} // namespace robv

namespace robv::encoder
{
    bool open(encoder_t& encoder, const encoder_config_t& config);
    void close(encoder_t& encoder);

    // appends zero or more packets, hardware may hand a frame back a call or two late
    bool encode(encoder_t& encoder, const planar_frame_t& frame, std::vector<encoded_packet_t>& out);

    // next frame becomes an IDR with parameter sets in front
    void request_keyframe(encoder_t& encoder);

    pixel_format_e input_format(encoder_kind_e kind);
    bool is_compiled_in(encoder_kind_e kind);
    bool parse_kind(std::string_view text, encoder_kind_e& out);
    const char* kind_name(encoder_kind_e kind);
} // namespace robv::encoder
