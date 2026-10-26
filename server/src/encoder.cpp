#include "robv/server/encoder.h"

#include "robv/common/log.h"

namespace robv::encoder
{
#if defined(ROBV_ENCODER_JETSON)
    bool open_jetson(encoder_t& encoder);
    void close_jetson(encoder_t& encoder);
    bool encode_jetson(encoder_t& encoder, const planar_frame_t& frame, std::vector<encoded_packet_t>& out);
#endif
#if defined(ROBV_ENCODER_FFMPEG)
    bool open_ffmpeg(encoder_t& encoder);
    void close_ffmpeg(encoder_t& encoder);
    bool encode_ffmpeg(encoder_t& encoder, const planar_frame_t& frame, std::vector<encoded_packet_t>& out);
#endif

    bool open(encoder_t& encoder, const encoder_config_t& config)
    {
        encoder = {};
        encoder.config = config;

        if (!is_compiled_in(config.kind))
        {
            if (config.kind == encoder_kind_e::ffmpeg)
            {
                ROBV_LOG_ERROR("ENCODE", "the ffmpeg encoder is not compiled in, configure with -DROBV_WITH_FFMPEG=ON");
            }
            else
            {
                ROBV_LOG_ERROR("ENCODE", "the {} encoder is not compiled in, configure with -DROBV_ENCODER={}",
                               kind_name(config.kind), kind_name(config.kind));
            }
            return false;
        }

        bool ok = false;
        switch (config.kind)
        {
        case encoder_kind_e::null: ok = true; break;
        case encoder_kind_e::jetson:
#if defined(ROBV_ENCODER_JETSON)
            ok = open_jetson(encoder);
#endif
            break;
        case encoder_kind_e::ffmpeg:
#if defined(ROBV_ENCODER_FFMPEG)
            ok = open_ffmpeg(encoder);
#endif
            break;
        }
        if (!ok)
        {
            close(encoder);
            return false;
        }

        ROBV_LOG_INFO("ENCODE", "{} encoder, {} {}x{} @ {} fps, {} kbit/s", kind_name(config.kind),
                      video::codec_name(config.codec), config.width, config.height, config.fps, config.bitrate_kbps);
        return true;
    }

    void close(encoder_t& encoder)
    {
#if defined(ROBV_ENCODER_JETSON)
        if (encoder.config.kind == encoder_kind_e::jetson && encoder.backend) { close_jetson(encoder); }
#endif
#if defined(ROBV_ENCODER_FFMPEG)
        if (encoder.config.kind == encoder_kind_e::ffmpeg && encoder.backend) { close_ffmpeg(encoder); }
#endif
        encoder.backend = nullptr;
    }

    bool encode(encoder_t& encoder, const planar_frame_t& frame, std::vector<encoded_packet_t>& out)
    {
        if (frame.format != input_format(encoder.config.kind))
        {
            ROBV_LOG_ERROR("ENCODE", "frame is not in the encoder's input format");
            return false;
        }

        encoder.frames_in++;
        const size_t before = out.size();

        bool ok = true;
        switch (encoder.config.kind)
        {
        case encoder_kind_e::null: break;
        case encoder_kind_e::jetson:
#if defined(ROBV_ENCODER_JETSON)
            ok = encode_jetson(encoder, frame, out);
#else
            ok = false;
#endif
            break;
        case encoder_kind_e::ffmpeg:
#if defined(ROBV_ENCODER_FFMPEG)
            ok = encode_ffmpeg(encoder, frame, out);
#else
            ok = false;
#endif
            break;
        }

        encoder.keyframe_requested = false;
        for (size_t i = before; i < out.size(); ++i)
        {
            encoder.packets_out++;
            encoder.bytes_out += out[i].data.size();
        }
        return ok;
    }

    void request_keyframe(encoder_t& encoder) { encoder.keyframe_requested = true; }

    pixel_format_e input_format(encoder_kind_e) { return pixel_format_e::nv12; }

    bool is_compiled_in(encoder_kind_e kind)
    {
        switch (kind)
        {
        case encoder_kind_e::null: return true;
        case encoder_kind_e::jetson:
#if defined(ROBV_ENCODER_JETSON)
            return true;
#else
            return false;
#endif
        case encoder_kind_e::ffmpeg:
#if defined(ROBV_ENCODER_FFMPEG)
            return true;
#else
            return false;
#endif
        }
        return false;
    }

    bool parse_kind(std::string_view text, encoder_kind_e& out)
    {
        if (text == "null") { out = encoder_kind_e::null; }
        else if (text == "jetson") { out = encoder_kind_e::jetson; }
        else if (text == "ffmpeg") { out = encoder_kind_e::ffmpeg; }
        else
        {
            return false;
        }
        return true;
    }

    const char* kind_name(encoder_kind_e kind)
    {
        switch (kind)
        {
        case encoder_kind_e::null: return "null";
        case encoder_kind_e::jetson: return "jetson";
        case encoder_kind_e::ffmpeg: return "ffmpeg";
        }
        return "unknown";
    }
} // namespace robv::encoder
