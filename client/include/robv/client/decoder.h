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
        // AMediaCodec into an AImageReader, the image's AHardwareBuffer imported into Vulkan
        mediacodec,
        ffmpeg,
    };

    struct decoder_config_t
    {
        decoder_kind_e kind = decoder_kind_e::null;
        video_codec_e codec = video_codec_e::h264;
        u32 width = 1280;
        u32 height = 720;
        u32 wake_event = 0;
    };

    // null and ffmpeg fill cpu, mediacodec hands out its output buffer instead (zero copy)
    struct decoded_frame_t
    {
        u64 pts_ns = 0;
        u32 width = 0;
        u32 height = 0;
        planar_frame_t cpu;
        // AHardwareBuffer, valid until image is released with decoder::release
        void* hardware_buffer = nullptr;
        void* image = nullptr;
    };

    struct decoder_t
    {
        decoder_config_t config;
        u64 units_in = 0;
        // pictures out of the decoder, frames_out the ones the app took, newer ones replace older
        u64 decoded = 0;
        u64 frames_out = 0;
        // units the decoder rejected, each one asks the server for an IDR
        u64 errors = 0;
        // null: next test frame due
        u64 next_frame_ns = 0;
        void* backend = nullptr;
    };
} // namespace robv

namespace robv::decoder
{
    bool open(decoder_t& decoder, const decoder_config_t& config);
    void close(decoder_t& decoder);
    bool reset(decoder_t& decoder);

    // one Annex B access unit from the RTP depacketizer, false when it could not be decoded
    bool submit(decoder_t& decoder, std::span<const u8> access_unit, u64 pts_ns);
    // work a decoder wants on the app's thread, after its wake_event: MediaCodec renders decoded pictures here
    void service(decoder_t& decoder);
    // newest frame since the last call, never blocks
    // a frame with an image must be released once the GPU no longer reads it, before the decoder is reset or closed
    bool acquire(decoder_t& decoder, u64 now_ns, decoded_frame_t& out);
    void release(decoder_t& decoder, void* image);
    // when acquire next has a frame for the app to wake up, UINT64_MAX when unknown
    u64 next_frame_ns(const decoder_t& decoder);

    bool is_compiled_in(decoder_kind_e kind);
    bool parse_kind(std::string_view text, decoder_kind_e& out);
    const char* kind_name(decoder_kind_e kind);
} // namespace robv::decoder
