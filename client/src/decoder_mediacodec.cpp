// AMediaCodec in async mode, rendering into an AImageReader of GPU buffers that video_view imports, zero copy
#include "robv/client/decoder.h"
#include "robv/common/log.h"

#include <SDL3/SDL.h>
#include <atomic>
#include <cstring>
#include <media/NdkImage.h>
#include <media/NdkImageReader.h>
#include <media/NdkMediaCodec.h>
#include <media/NdkMediaFormat.h>
#include <mutex>
#include <utility>
#include <vector>

namespace robv::decoder
{
    namespace
    {
        // pictures the reader can hand out: one shown, up to two still read by frames in flight, one arriving,
        // and room for acquireLatestImage to skip older ones
        constexpr i32 reader_images = 6;

        struct mediacodec_t
        {
            AMediaCodec* codec = nullptr;
            AImageReader* reader = nullptr;
            u32 wake_event = 0;

            // filled by the callbacks, taken on the app's thread
            std::mutex mutex;
            std::vector<i32> free_inputs;
            // decoded pictures to render into the reader, with whether they are pictures at all
            std::vector<std::pair<i32, bool>> ready_outputs;
            std::atomic<bool> wake_pending{false};
            std::atomic<u64> decoded{0};
            std::atomic<bool> failed{false};
        };

        // one event in the queue is enough, service handles everything waiting
        void wake(mediacodec_t* m)
        {
            if (m->wake_event == 0 || m->wake_pending.exchange(true)) { return; }
            SDL_Event event{};
            event.type = m->wake_event;
            SDL_PushEvent(&event);
        }

        // codec thread: input buffer free for submit
        void on_input(AMediaCodec*, void* data, i32 index)
        {
            mediacodec_t* m = static_cast<mediacodec_t*>(data);
            std::lock_guard lock(m->mutex);
            m->free_inputs.push_back(index);
        }

        // codec thread: a picture is done, service renders it into the reader
        void on_output(AMediaCodec*, void* data, i32 index, AMediaCodecBufferInfo* info)
        {
            mediacodec_t* m = static_cast<mediacodec_t*>(data);
            const u32 not_pictures = AMEDIACODEC_BUFFER_FLAG_END_OF_STREAM | AMEDIACODEC_BUFFER_FLAG_CODEC_CONFIG;
            const bool picture = (info->flags & not_pictures) == 0;
            {
                std::lock_guard lock(m->mutex);
                m->ready_outputs.emplace_back(index, picture);
            }
            wake(m);
        }

        void on_format(AMediaCodec*, void*, AMediaFormat* format)
        { ROBV_LOG_INFO("DECODE", "mediacodec output format {}", AMediaFormat_toString(format)); }

        void on_error(AMediaCodec*, void* data, media_status_t error, i32 code, const char* detail)
        {
            mediacodec_t* m = static_cast<mediacodec_t*>(data);
            ROBV_LOG_ERROR("DECODE", "mediacodec error {} ({}): {}", static_cast<i32>(error), code, detail);
            m->failed.store(true);
        }

        // reader thread: a rendered picture arrived, acquire takes it on the next frame
        void on_image(void* data, AImageReader*) { wake(static_cast<mediacodec_t*>(data)); }

    } // namespace

    bool open_mediacodec(decoder_t& decoder)
    {
        const decoder_config_t& config = decoder.config;
        if (config.codec != video_codec_e::h264)
        {
            ROBV_LOG_ERROR("DECODE", "the mediacodec decoder is set up for h264 only");
            return false;
        }

        mediacodec_t* m = new mediacodec_t;
        decoder.backend = m;
        m->wake_event = config.wake_event;

        const i32 width = static_cast<i32>(config.width);
        const i32 height = static_cast<i32>(config.height);
        if (AImageReader_newWithUsage(width, height, AIMAGE_FORMAT_PRIVATE, AHARDWAREBUFFER_USAGE_GPU_SAMPLED_IMAGE,
                                      reader_images, &m->reader) != AMEDIA_OK)
        {
            ROBV_LOG_ERROR("DECODE", "AImageReader_new {}x{} failed", width, height);
            return false;
        }
        AImageReader_ImageListener listener{.context = m, .onImageAvailable = on_image};
        AImageReader_setImageListener(m->reader, &listener);
        ANativeWindow* window = nullptr;
        if (AImageReader_getWindow(m->reader, &window) != AMEDIA_OK)
        {
            ROBV_LOG_ERROR("DECODE", "AImageReader_getWindow failed");
            return false;
        }

        m->codec = AMediaCodec_createDecoderByType("video/avc");
        if (!m->codec)
        {
            ROBV_LOG_ERROR("DECODE", "no video/avc decoder on this device");
            return false;
        }
        char* name = nullptr;
        if (AMediaCodec_getName(m->codec, &name) == AMEDIA_OK)
        {
            ROBV_LOG_INFO("DECODE", "mediacodec {}", name);
            AMediaCodec_releaseName(m->codec, name);
        }

        AMediaCodecOnAsyncNotifyCallback callbacks{
            .onAsyncInputAvailable = on_input,
            .onAsyncOutputAvailable = on_output,
            .onAsyncFormatChanged = on_format,
            .onAsyncError = on_error,
        };
        if (AMediaCodec_setAsyncNotifyCallback(m->codec, callbacks, m) != AMEDIA_OK)
        {
            ROBV_LOG_ERROR("DECODE", "AMediaCodec_setAsyncNotifyCallback failed");
            return false;
        }

        AMediaFormat* format = AMediaFormat_new();
        AMediaFormat_setString(format, AMEDIAFORMAT_KEY_MIME, "video/avc");
        AMediaFormat_setInt32(format, AMEDIAFORMAT_KEY_WIDTH, width);
        AMediaFormat_setInt32(format, AMEDIAFORMAT_KEY_HEIGHT, height);
        // Android 11 low latency mode, a decoder without it ignores the key
        AMediaFormat_setInt32(format, "low-latency", 1);
        // realtime priority
        AMediaFormat_setInt32(format, "priority", 0);
        const media_status_t configured = AMediaCodec_configure(m->codec, format, window, nullptr, 0);
        AMediaFormat_delete(format);
        if (configured != AMEDIA_OK || AMediaCodec_start(m->codec) != AMEDIA_OK)
        {
            ROBV_LOG_ERROR("DECODE", "mediacodec configure or start failed ({})", static_cast<i32>(configured));
            return false;
        }
        return true;
    }

    void close_mediacodec(decoder_t& decoder)
    {
        mediacodec_t* m = static_cast<mediacodec_t*>(decoder.backend);
        // no callbacks run once stop returns
        if (m->codec)
        {
            AMediaCodec_stop(m->codec);
            AMediaCodec_delete(m->codec);
        }
        if (m->reader) { AImageReader_delete(m->reader); }
        delete m;
    }

    bool submit_mediacodec(decoder_t& decoder, std::span<const u8> access_unit, u64 pts_ns)
    {
        mediacodec_t* m = static_cast<mediacodec_t*>(decoder.backend);
        if (m->failed.load()) { return false; }

        i32 index = -1;
        {
            std::lock_guard lock(m->mutex);
            if (!m->free_inputs.empty())
            {
                index = m->free_inputs.front();
                m->free_inputs.erase(m->free_inputs.begin());
            }
        }
        // every input buffer queued: the decoder is behind, this unit is lost and the stream needs an IDR
        if (index < 0)
        {
            ROBV_LOG_DEBUG("DECODE", "no free mediacodec input buffer, unit dropped");
            return false;
        }

        size_t capacity = 0;
        u8* buffer = AMediaCodec_getInputBuffer(m->codec, static_cast<size_t>(index), &capacity);
        if (!buffer || capacity < access_unit.size())
        {
            ROBV_LOG_ERROR("DECODE", "unit of {} bytes does not fit the {} byte input buffer", access_unit.size(),
                           capacity);
            AMediaCodec_queueInputBuffer(m->codec, static_cast<size_t>(index), 0, 0, pts_ns / 1000, 0);
            return false;
        }
        std::memcpy(buffer, access_unit.data(), access_unit.size());
        // SPS and PPS travel in band in front of each IDR
        const media_status_t queued =
            AMediaCodec_queueInputBuffer(m->codec, static_cast<size_t>(index), 0, access_unit.size(), pts_ns / 1000, 0);
        return queued == AMEDIA_OK;
    }

    // renders finished pictures into the reader, they come back through on_image
    void service_mediacodec(decoder_t& decoder)
    {
        mediacodec_t* m = static_cast<mediacodec_t*>(decoder.backend);
        m->wake_pending.store(false);
        std::vector<std::pair<i32, bool>> ready;
        {
            std::lock_guard lock(m->mutex);
            ready.swap(m->ready_outputs);
        }
        for (const auto& [index, picture] : ready)
        {
            AMediaCodec_releaseOutputBuffer(m->codec, static_cast<size_t>(index), picture);
            if (picture) { m->decoded.fetch_add(1); }
        }
        decoder.decoded = m->decoded.load();
    }

    bool acquire_mediacodec(decoder_t& decoder, decoded_frame_t& out)
    {
        mediacodec_t* m = static_cast<mediacodec_t*>(decoder.backend);
        service_mediacodec(decoder);

        AImage* image = nullptr;
        if (AImageReader_acquireLatestImage(m->reader, &image) != AMEDIA_OK || !image) { return false; }
        AHardwareBuffer* buffer = nullptr;
        if (AImage_getHardwareBuffer(image, &buffer) != AMEDIA_OK || !buffer)
        {
            AImage_delete(image);
            return false;
        }

        // the crop is the picture, the buffer may be padded
        AImageCropRect crop{};
        i64 timestamp_ns = 0;
        AImage_getCropRect(image, &crop);
        AImage_getTimestamp(image, &timestamp_ns);
        out.pts_ns = static_cast<u64>(timestamp_ns);
        out.width = static_cast<u32>(crop.right - crop.left);
        out.height = static_cast<u32>(crop.bottom - crop.top);
        out.hardware_buffer = buffer;
        out.image = image;
        return true;
    }

    void release_mediacodec(void* image) { AImage_delete(static_cast<AImage*>(image)); }
} // namespace robv::decoder
