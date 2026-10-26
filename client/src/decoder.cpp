#include "robv/client/decoder.h"

#include "robv/common/log.h"

namespace robv::decoder
{
    namespace { constexpr u64 null_frame_period_ns = 1'000'000'000ull / 30; } // namespace

#if defined(ROBV_DECODER_MEDIACODEC)
    bool open_mediacodec(decoder_t& decoder);
    void close_mediacodec(decoder_t& decoder);
    bool submit_mediacodec(decoder_t& decoder, std::span<const u8> access_unit, u64 pts_ns);
    bool acquire_mediacodec(decoder_t& decoder, decoded_frame_t& out);
    void service_mediacodec(decoder_t& decoder);
    void release_mediacodec(void* image);
#endif
#if defined(ROBV_DECODER_FFMPEG)
    bool open_ffmpeg(decoder_t& decoder);
    void close_ffmpeg(decoder_t& decoder);
    bool submit_ffmpeg(decoder_t& decoder, std::span<const u8> access_unit, u64 pts_ns);
    bool acquire_ffmpeg(decoder_t& decoder, decoded_frame_t& out);
#endif

    bool open(decoder_t& decoder, const decoder_config_t& config)
    {
        decoder = {};
        decoder.config = config;

        if (!is_compiled_in(config.kind))
        {
            ROBV_LOG_ERROR("DECODE", "the {} decoder is not available in this build", kind_name(config.kind));
            return false;
        }
#if defined(ROBV_DECODER_FFMPEG)
        if (config.kind == decoder_kind_e::ffmpeg && !open_ffmpeg(decoder))
        {
            close(decoder);
            return false;
        }
#endif
#if defined(ROBV_DECODER_MEDIACODEC)
        if (config.kind == decoder_kind_e::mediacodec && !open_mediacodec(decoder))
        {
            close(decoder);
            return false;
        }
#endif

        ROBV_LOG_INFO("DECODE", "{} decoder, {} {}x{}", kind_name(config.kind), video::codec_name(config.codec),
                      config.width, config.height);
        return true;
    }

    void close(decoder_t& decoder)
    {
#if defined(ROBV_DECODER_FFMPEG)
        if (decoder.config.kind == decoder_kind_e::ffmpeg && decoder.backend) { close_ffmpeg(decoder); }
#endif
#if defined(ROBV_DECODER_MEDIACODEC)
        if (decoder.config.kind == decoder_kind_e::mediacodec && decoder.backend) { close_mediacodec(decoder); }
#endif
        decoder.backend = nullptr;
    }

    bool reset(decoder_t& decoder)
    {
        const decoder_config_t config = decoder.config;
        close(decoder);
        return open(decoder, config);
    }

    bool submit(decoder_t& decoder, std::span<const u8> access_unit, u64 pts_ns)
    {
        if (access_unit.empty()) { return false; }
        ++decoder.units_in;
        bool ok = true;
#if defined(ROBV_DECODER_FFMPEG)
        if (decoder.config.kind == decoder_kind_e::ffmpeg) { ok = submit_ffmpeg(decoder, access_unit, pts_ns); }
#endif
#if defined(ROBV_DECODER_MEDIACODEC)
        if (decoder.config.kind == decoder_kind_e::mediacodec) { ok = submit_mediacodec(decoder, access_unit, pts_ns); }
#endif
        (void)pts_ns;
        if (!ok) { ++decoder.errors; }
        return ok;
    }

    void release(decoder_t& decoder, void* image)
    {
#if defined(ROBV_DECODER_MEDIACODEC)
        if (decoder.config.kind == decoder_kind_e::mediacodec && image) { release_mediacodec(image); }
#endif
        (void)decoder;
        (void)image;
    }

    void service(decoder_t& decoder)
    {
#if defined(ROBV_DECODER_MEDIACODEC)
        if (decoder.config.kind == decoder_kind_e::mediacodec && decoder.backend) { service_mediacodec(decoder); }
#endif
        (void)decoder;
    }

    bool acquire(decoder_t& decoder, u64 now_ns, decoded_frame_t& out)
    {
#if defined(ROBV_DECODER_FFMPEG)
        if (decoder.config.kind == decoder_kind_e::ffmpeg)
        {
            const bool got = acquire_ffmpeg(decoder, out);
            if (got) { ++decoder.frames_out; }
            return got;
        }
#endif
#if defined(ROBV_DECODER_MEDIACODEC)
        if (decoder.config.kind == decoder_kind_e::mediacodec)
        {
            const bool got = acquire_mediacodec(decoder, out);
            if (got) { ++decoder.frames_out; }
            return got;
        }
#endif
        if (now_ns < decoder.next_frame_ns) { return false; }

        decoder.next_frame_ns += null_frame_period_ns;
        if (decoder.next_frame_ns <= now_ns) { decoder.next_frame_ns = now_ns + null_frame_period_ns; }

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

    // null runs on a clock, the stream decoders will wake the app with an event instead
    u64 next_frame_ns(const decoder_t& decoder)
    { return decoder.config.kind == decoder_kind_e::null ? decoder.next_frame_ns : UINT64_MAX; }

    bool is_compiled_in(decoder_kind_e kind)
    {
        switch (kind)
        {
        case decoder_kind_e::null: return true;
        case decoder_kind_e::mediacodec:
#if defined(ROBV_DECODER_MEDIACODEC)
            return true;
#else
            return false;
#endif
        case decoder_kind_e::ffmpeg:
#if defined(ROBV_DECODER_FFMPEG)
            return true;
#else
            return false;
#endif
        }
        return false;
    }

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
