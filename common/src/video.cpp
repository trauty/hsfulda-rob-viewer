#include "robv/common/video.h"

#include <algorithm>
#include <cmath>
#include <cstring>

namespace robv::video
{
    namespace
    {
        struct yuv_t
        {
            u8 y;
            u8 u;
            u8 v;
        };

        // bt.709 limited range, what encoders signal by default
        yuv_t rgb_to_yuv(f32 r, f32 g, f32 b)
        {
            const f32 y = 0.2126f * r + 0.7152f * g + 0.0722f * b;
            const f32 u = (b - y) / 1.8556f;
            const f32 v = (r - y) / 1.5748f;
            return {
                static_cast<u8>(std::clamp(16.0f + 219.0f * y, 16.0f, 235.0f)),
                static_cast<u8>(std::clamp(128.0f + 224.0f * u, 16.0f, 240.0f)),
                static_cast<u8>(std::clamp(128.0f + 224.0f * v, 16.0f, 240.0f)),
            };
        }

        void hue_to_rgb(f32 hue, f32& r, f32& g, f32& b)
        {
            hue = hue - std::floor(hue);
            r = std::clamp(std::fabs(hue * 6.0f - 3.0f) - 1.0f, 0.0f, 1.0f);
            g = std::clamp(2.0f - std::fabs(hue * 6.0f - 2.0f), 0.0f, 1.0f);
            b = std::clamp(2.0f - std::fabs(hue * 6.0f - 4.0f), 0.0f, 1.0f);
        }
    } // namespace

    void allocate_nv12(planar_frame_t& frame, u32 width, u32 height)
    {
        width = (width + 1) & ~1u;
        height = (height + 1) & ~1u;

        frame.width = width;
        frame.height = height;
        frame.format = pixel_format_e::nv12;
        frame.y_stride = width;
        frame.uv_stride = width;
        frame.data.resize(static_cast<size_t>(width) * height * 3 / 2);
    }

    u8* y_plane(planar_frame_t& frame) { return frame.data.data(); }

    u8* uv_plane(planar_frame_t& frame)
    { return frame.data.data() + static_cast<size_t>(frame.y_stride) * frame.height; }

    void fill_test_pattern(planar_frame_t& frame, f32 phase, f32 hue)
    {
        constexpr u32 bars = 8;

        // one row of bars, copied down
        yuv_t colors[bars];
        for (u32 i = 0; i < bars; ++i)
        {
            f32 r, g, b;
            hue_to_rgb(hue + static_cast<f32>(i) / bars, r, g, b);
            colors[i] = rgb_to_yuv(r, g, b);
        }

        const u32 shift = static_cast<u32>((phase - std::floor(phase)) * static_cast<f32>(frame.width));

        u8* y = y_plane(frame);
        u8* uv = uv_plane(frame);

        for (u32 x = 0; x < frame.width; ++x)
        {
            const u32 bar = ((x + shift) % frame.width) * bars / frame.width;
            y[x] = colors[bar].y;
        }
        for (u32 x = 0; x + 1 < frame.width; x += 2)
        {
            const u32 bar = ((x + shift) % frame.width) * bars / frame.width;
            uv[x] = colors[bar].u;
            uv[x + 1] = colors[bar].v;
        }

        for (u32 row = 1; row < frame.height; ++row)
        {
            std::memcpy(y + static_cast<size_t>(row) * frame.y_stride, y, frame.width);
        }
        for (u32 row = 1; row < frame.height / 2; ++row)
        {
            std::memcpy(uv + static_cast<size_t>(row) * frame.uv_stride, uv, frame.width);
        }
    }

    const char* codec_name(video_codec_e codec)
    {
        switch (codec)
        {
        case video_codec_e::h264: return "h264";
        case video_codec_e::hevc: return "hevc";
        }
        return "unknown";
    }
} // namespace robv::video
