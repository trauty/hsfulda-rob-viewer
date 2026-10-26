// libavcodec H.264: NVENC on an NVIDIA desktop, x264 otherwise
// the desktop stand in for the Jetson's hardware encoder, same NV12 in and Annex B out
#include "robv/common/log.h"
#include "robv/server/encoder.h"

#include <cstring>
#include <iterator>
#include <string>

extern "C"
{
#include <libavcodec/avcodec.h>
#include <libavutil/dict.h>
#include <libavutil/error.h>
}

namespace robv::encoder
{
    namespace
    {
        struct ffmpeg_encoder_t
        {
            AVCodecContext* ctx = nullptr;
            AVFrame* frame = nullptr;
            AVPacket* packet = nullptr;
            i64 next_pts = 0;
            // pts_ns of recent frames by pts, a packet can come out a frame or two after its frame went in
            u64 pts_ns[16] = {};
        };

        std::string av_error(int code)
        {
            char text[AV_ERROR_MAX_STRING_SIZE] = {};
            av_strerror(code, text, sizeof(text));
            return text;
        }

        // low latency for both: no B frames, no lookahead, rate capped with a two frame buffer, IDR on request
        void set_low_latency(const std::string& name, AVDictionary*& options)
        {
            if (name == "h264_nvenc")
            {
                av_dict_set(&options, "preset", "p2", 0);
                av_dict_set(&options, "tune", "ull", 0);
                // VBR capped at the target: NVENC's CBR pads with filler NALs up to the full rate, wasted Wi-Fi airtime
                av_dict_set(&options, "rc", "vbr", 0);
                av_dict_set(&options, "zerolatency", "1", 0);
                av_dict_set(&options, "delay", "0", 0);
                av_dict_set(&options, "forced-idr", "1", 0);
            }
            else
            {
                av_dict_set(&options, "preset", "veryfast", 0);
                av_dict_set(&options, "tune", "zerolatency", 0);
                av_dict_set(&options, "forced-idr", "1", 0);
            }
        }

        AVCodecContext* open_codec(const std::string& name, const encoder_config_t& config)
        {
            const AVCodec* codec = avcodec_find_encoder_by_name(name.c_str());
            if (!codec) { return nullptr; }

            AVCodecContext* ctx = avcodec_alloc_context3(codec);
            ctx->width = static_cast<int>(config.width);
            ctx->height = static_cast<int>(config.height);
            ctx->time_base = {1, static_cast<int>(config.fps)};
            ctx->framerate = {static_cast<int>(config.fps), 1};
            ctx->pix_fmt = AV_PIX_FMT_NV12;
            ctx->bit_rate = static_cast<i64>(config.bitrate_kbps) * 1000;
            ctx->rc_max_rate = ctx->bit_rate;
            ctx->rc_buffer_size = static_cast<int>(ctx->bit_rate / config.fps * 2);
            ctx->max_b_frames = 0;
            // 0: keyframes only when a client asks, plus one a minute in case nobody does
            ctx->gop_size = static_cast<int>(config.idr_interval > 0 ? config.idr_interval : config.fps * 60);
            ctx->color_range = AVCOL_RANGE_MPEG;
            ctx->colorspace = AVCOL_SPC_BT709;
            ctx->color_primaries = AVCOL_PRI_BT709;
            ctx->color_trc = AVCOL_TRC_BT709;

            AVDictionary* options = nullptr;
            set_low_latency(name, options);
            const int opened = avcodec_open2(ctx, codec, &options);
            av_dict_free(&options);
            if (opened < 0)
            {
                ROBV_LOG_INFO("ENCODE", "{} unavailable: {}", name, av_error(opened));
                avcodec_free_context(&ctx);
                return nullptr;
            }
            return ctx;
        }
    } // namespace

    bool open_ffmpeg(encoder_t& encoder)
    {
        const encoder_config_t& config = encoder.config;
        if (config.codec != video_codec_e::h264)
        {
            ROBV_LOG_ERROR("ENCODE", "the ffmpeg encoder streams h264 only, RTP for hevc is not implemented");
            return false;
        }

        ffmpeg_encoder_t* f = new ffmpeg_encoder_t;
        encoder.backend = f;

        for (const char* name : {"h264_nvenc", "libx264"})
        {
            f->ctx = open_codec(name, config);
            if (f->ctx)
            {
                ROBV_LOG_INFO("ENCODE", "ffmpeg encoder {}", name);
                break;
            }
        }
        if (!f->ctx)
        {
            ROBV_LOG_ERROR("ENCODE", "neither h264_nvenc nor libx264 could be opened");
            return false;
        }

        f->frame = av_frame_alloc();
        f->frame->format = AV_PIX_FMT_NV12;
        f->frame->width = f->ctx->width;
        f->frame->height = f->ctx->height;
        f->packet = av_packet_alloc();
        if (const int result = av_frame_get_buffer(f->frame, 0); result < 0)
        {
            ROBV_LOG_ERROR("ENCODE", "av_frame_get_buffer: {}", av_error(result));
            return false;
        }
        return true;
    }

    void close_ffmpeg(encoder_t& encoder)
    {
        ffmpeg_encoder_t* f = static_cast<ffmpeg_encoder_t*>(encoder.backend);
        av_packet_free(&f->packet);
        av_frame_free(&f->frame);
        avcodec_free_context(&f->ctx);
        delete f;
    }

    bool encode_ffmpeg(encoder_t& encoder, const planar_frame_t& frame, std::vector<encoded_packet_t>& out)
    {
        ffmpeg_encoder_t* f = static_cast<ffmpeg_encoder_t*>(encoder.backend);
        if (frame.width != static_cast<u32>(f->ctx->width) || frame.height != static_cast<u32>(f->ctx->height))
        {
            ROBV_LOG_ERROR("ENCODE", "frame is {}x{}, the encoder was opened for {}x{}", frame.width, frame.height,
                           f->ctx->width, f->ctx->height);
            return false;
        }

        // the encoder may still hold the last buffer, a writable one may be a new allocation
        if (const int result = av_frame_make_writable(f->frame); result < 0)
        {
            ROBV_LOG_ERROR("ENCODE", "av_frame_make_writable: {}", av_error(result));
            return false;
        }
        const u8* y = frame.data.data();
        const u8* uv = y + static_cast<size_t>(frame.y_stride) * frame.height;
        for (u32 row = 0; row < frame.height; ++row)
        {
            std::memcpy(f->frame->data[0] + static_cast<size_t>(row) * f->frame->linesize[0],
                        y + static_cast<size_t>(row) * frame.y_stride, frame.width);
        }
        for (u32 row = 0; row < frame.height / 2; ++row)
        {
            std::memcpy(f->frame->data[1] + static_cast<size_t>(row) * f->frame->linesize[1],
                        uv + static_cast<size_t>(row) * frame.uv_stride, frame.width);
        }

        f->frame->pts = f->next_pts++;
        f->pts_ns[f->frame->pts % std::size(f->pts_ns)] = frame.pts_ns;
        if (encoder.keyframe_requested)
        {
            f->frame->pict_type = AV_PICTURE_TYPE_I;
            f->frame->flags |= AV_FRAME_FLAG_KEY;
        }
        else
        {
            f->frame->pict_type = AV_PICTURE_TYPE_NONE;
            f->frame->flags &= ~AV_FRAME_FLAG_KEY;
        }

        if (const int result = avcodec_send_frame(f->ctx, f->frame); result < 0)
        {
            ROBV_LOG_ERROR("ENCODE", "avcodec_send_frame: {}", av_error(result));
            return false;
        }
        for (;;)
        {
            const int result = avcodec_receive_packet(f->ctx, f->packet);
            if (result == AVERROR(EAGAIN) || result == AVERROR_EOF) { break; }
            if (result < 0)
            {
                ROBV_LOG_ERROR("ENCODE", "avcodec_receive_packet: {}", av_error(result));
                return false;
            }
            encoded_packet_t& packet = out.emplace_back();
            packet.data.assign(f->packet->data, f->packet->data + f->packet->size);
            packet.pts_ns = f->pts_ns[f->packet->pts % std::size(f->pts_ns)];
            packet.keyframe = (f->packet->flags & AV_PKT_FLAG_KEY) != 0;
            av_packet_unref(f->packet);
        }
        return true;
    }
} // namespace robv::encoder
