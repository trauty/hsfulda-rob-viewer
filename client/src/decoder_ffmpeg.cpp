// libavcodec H.264 in software, the desktop counterpart of MediaCodec
// decodes in submit, so the newest frame is ready for acquire on the same UI pass
#include "robv/client/decoder.h"
#include "robv/common/log.h"

#include <cstring>
#include <string>
#include <utility>

extern "C"
{
#include <libavcodec/avcodec.h>
#include <libavutil/error.h>
}

namespace robv::decoder
{
    namespace
    {
        struct ffmpeg_decoder_t
        {
            AVCodecContext* ctx = nullptr;
            AVPacket* packet = nullptr;
            AVFrame* frame = nullptr;
            // newest decoded picture as NV12, waiting for acquire
            planar_frame_t latest;
            bool fresh = false;
        };

        std::string av_error(int code)
        {
            char text[AV_ERROR_MAX_STRING_SIZE] = {};
            av_strerror(code, text, sizeof(text));
            return text;
        }

        // 4:2:0 planar to NV12: luma rows as they are, chroma interleaved
        bool to_nv12(const AVFrame& in, planar_frame_t& out)
        {
            if (in.format != AV_PIX_FMT_YUV420P && in.format != AV_PIX_FMT_YUVJ420P)
            {
                ROBV_LOG_ERROR("DECODE", "decoder output format {} is not 4:2:0 planar", in.format);
                return false;
            }
            const u32 width = static_cast<u32>(in.width);
            const u32 height = static_cast<u32>(in.height);
            if (out.width != width || out.height != height || out.data.empty())
            {
                video::allocate_nv12(out, width, height);
            }

            u8* y = video::y_plane(out);
            u8* uv = video::uv_plane(out);
            for (u32 row = 0; row < height; ++row)
            {
                std::memcpy(y + static_cast<size_t>(row) * out.y_stride,
                            in.data[0] + static_cast<size_t>(row) * in.linesize[0], width);
            }
            for (u32 row = 0; row < height / 2; ++row)
            {
                const u8* u = in.data[1] + static_cast<size_t>(row) * in.linesize[1];
                const u8* v = in.data[2] + static_cast<size_t>(row) * in.linesize[2];
                u8* dst = uv + static_cast<size_t>(row) * out.uv_stride;
                for (u32 x = 0; x < width / 2; ++x)
                {
                    dst[2 * x] = u[x];
                    dst[2 * x + 1] = v[x];
                }
            }
            return true;
        }
    } // namespace

    bool open_ffmpeg(decoder_t& decoder)
    {
        if (decoder.config.codec != video_codec_e::h264)
        {
            ROBV_LOG_ERROR("DECODE", "the ffmpeg decoder takes h264 only");
            return false;
        }
        const AVCodec* codec = avcodec_find_decoder(AV_CODEC_ID_H264);
        if (!codec)
        {
            ROBV_LOG_ERROR("DECODE", "this libavcodec has no h264 decoder");
            return false;
        }

        ffmpeg_decoder_t* f = new ffmpeg_decoder_t;
        decoder.backend = f;
        f->ctx = avcodec_alloc_context3(codec);
        // frame threads hold a frame each, slices decode one frame in parallel without delay
        f->ctx->thread_type = FF_THREAD_SLICE;
        f->ctx->thread_count = 0;
        f->ctx->flags |= AV_CODEC_FLAG_LOW_DELAY;
        // after a loss h264 withholds frames until the next keyframe, this shows them concealed instead
        f->ctx->flags2 |= AV_CODEC_FLAG2_SHOW_ALL;
        if (const int result = avcodec_open2(f->ctx, codec, nullptr); result < 0)
        {
            ROBV_LOG_ERROR("DECODE", "avcodec_open2: {}", av_error(result));
            return false;
        }
        f->packet = av_packet_alloc();
        f->frame = av_frame_alloc();
        return true;
    }

    void close_ffmpeg(decoder_t& decoder)
    {
        ffmpeg_decoder_t* f = static_cast<ffmpeg_decoder_t*>(decoder.backend);
        av_frame_free(&f->frame);
        av_packet_free(&f->packet);
        avcodec_free_context(&f->ctx);
        delete f;
    }

    bool submit_ffmpeg(decoder_t& decoder, std::span<const u8> access_unit, u64 pts_ns)
    {
        ffmpeg_decoder_t* f = static_cast<ffmpeg_decoder_t*>(decoder.backend);
        // not refcounted: libavcodec copies what it keeps
        f->packet->data = const_cast<u8*>(access_unit.data());
        f->packet->size = static_cast<int>(access_unit.size());
        f->packet->pts = static_cast<i64>(pts_ns);
        const int sent = avcodec_send_packet(f->ctx, f->packet);
        f->packet->data = nullptr;
        f->packet->size = 0;
        if (sent < 0)
        {
            ROBV_LOG_DEBUG("DECODE", "avcodec_send_packet: {}", av_error(sent));
            return false;
        }

        for (;;)
        {
            const int received = avcodec_receive_frame(f->ctx, f->frame);
            if (received == AVERROR(EAGAIN) || received == AVERROR_EOF) { break; }
            if (received < 0)
            {
                ROBV_LOG_DEBUG("DECODE", "avcodec_receive_frame: {}", av_error(received));
                return false;
            }
            // concealed after a loss: shown anyway, a short smear beats a frozen view, false asks for an IDR
            const bool corrupt = (f->frame->flags & AV_FRAME_FLAG_CORRUPT) != 0;
            ++decoder.decoded;
            if (to_nv12(*f->frame, f->latest))
            {
                f->latest.pts_ns = static_cast<u64>(f->frame->pts);
                f->fresh = true;
            }
            av_frame_unref(f->frame);
            if (corrupt) { return false; }
        }
        return true;
    }

    bool acquire_ffmpeg(decoder_t& decoder, decoded_frame_t& out)
    {
        ffmpeg_decoder_t* f = static_cast<ffmpeg_decoder_t*>(decoder.backend);
        if (!f->fresh) { return false; }
        f->fresh = false;
        // swapped, so out's old buffer becomes the next decode target
        std::swap(out.cpu, f->latest);
        out.pts_ns = out.cpu.pts_ns;
        out.width = out.cpu.width;
        out.height = out.cpu.height;
        return true;
    }
} // namespace robv::decoder
