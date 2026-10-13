#pragma once

#include "robv/common/video.h"

#include <span>
#include <string_view>

namespace robv
{
    enum class decoder_kind_e : u8
    {
        // test pattern, ignores the bitstream
        null,
        // AMediaCodec to AImageReader to AHardwareBuffer, imported with
        // VK_ANDROID_external_memory_android_hardware_buffer and sampled via VkSamplerYcbcrConversion
        mediacodec,
        ffmpeg,
    };

    struct decoder_config_t
    {
        decoder_kind_e kind = decoder_kind_e::null;
        video_codec_e codec = video_codec_e::h264;
        u32 width = 1280;
        u32 height = 720;
    };

    // null fills cpu, zero copy backends will hand out a GPU image instead
    struct decoded_frame_t
    {
        u64 pts_ns = 0;
        u32 width = 0;
        u32 height = 0;
        planar_frame_t cpu;
    };

    struct decoder_t
    {
        decoder_config_t config;
        u64 units_in = 0;
        u64 frames_out = 0;
        // null: next test frame due
        u64 next_frame_ns = 0;
        void* backend = nullptr;
    };
} // namespace robv

namespace robv::decoder
{
    bool open(decoder_t& decoder, const decoder_config_t& config);
    void close(decoder_t& decoder);

    // one Annex B access unit from the RTP depacketizer
    bool submit(decoder_t& decoder, std::span<const u8> access_unit, u64 pts_ns);
    // newest frame since the last call, never blocks
    bool acquire(decoder_t& decoder, u64 now_ns, decoded_frame_t& out);

    bool is_compiled_in(decoder_kind_e kind);
    bool parse_kind(std::string_view text, decoder_kind_e& out);
    const char* kind_name(decoder_kind_e kind);
} // namespace robv::decoder
