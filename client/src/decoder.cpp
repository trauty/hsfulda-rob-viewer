#include "robv/client/decoder.h"

#include "robv/common/log.h"

namespace robv::decoder
{
    namespace { constexpr u64 null_frame_period_ns = 1'000'000'000ull / 30; } // namespace

    bool open(decoder_t& decoder, const decoder_config_t& config)
    {
        decoder = {};
        decoder.config = config;

        if (!is_compiled_in(config.kind))
        {
            ROBV_LOG_ERROR("DECODE", "the {} decoder is not implemented yet, use null", kind_name(config.kind));
            return false;
        }

        ROBV_LOG_INFO("DECODE", "{} decoder, {} {}x{}", kind_name(config.kind), video::codec_name(config.codec),
                      config.width, config.height);
        return true;
    }

    void close(decoder_t& decoder) { decoder.backend = nullptr; }

    bool submit(decoder_t& decoder, std::span<const u8> access_unit, u64)
    {
        if (access_unit.empty()) { return false; }
        ++decoder.units_in;
        return true;
    }

    // null: test pattern at 30 Hz regardless of input
    bool acquire(decoder_t& decoder, u64 now_ns, decoded_frame_t& out)
    {
        if (now_ns < decoder.next_frame_ns) { return false; }
        decoder.next_frame_ns = now_ns + null_frame_period_ns;

        if (out.cpu.width != decoder.config.width || out.cpu.height != decoder.config.height)
        {
            video::allocate_nv12(out.cpu, decoder.config.width, decoder.config.height);
        }
        const f32 phase = static_cast<f32>(decoder.frames_out % 300) / 300.0f;
        video::fill_test_pattern(out.cpu, phase, 0.6f);

        out.pts_ns = now_ns;
        out.width = out.cpu.width;
        out.height = out.cpu.height;
        ++decoder.frames_out;
        return true;
    }

    bool is_compiled_in(decoder_kind_e kind) { return kind == decoder_kind_e::null; }

    bool parse_kind(std::string_view text, decoder_kind_e& out)
    {
        if (text == "null") { out = decoder_kind_e::null; }
        else if (text == "mediacodec") { out = decoder_kind_e::mediacodec; }
        else if (text == "ffmpeg") { out = decoder_kind_e::ffmpeg; }
        else
        {
            return false;
        }
        return true;
    }

    const char* kind_name(decoder_kind_e kind)
    {
        switch (kind)
        {
        case decoder_kind_e::null: return "null";
        case decoder_kind_e::mediacodec: return "mediacodec";
        case decoder_kind_e::ffmpeg: return "ffmpeg";
        }
        return "unknown";
    }
} // namespace robv::decoder
