#pragma once

#include "robv/common/types.h"

#include <vector>

namespace robv
{
    enum class video_codec_e : u8
    {
        h264 = 1,
        hevc = 2
    };

    enum class pixel_format_e : u8
    {
        nv12 = 1
    };

    struct planar_frame_t
    {
        u32 width = 0;
        u32 height = 0;
        pixel_format_e format = pixel_format_e::nv12;
        u32 y_stride = 0;
        u32 uv_stride = 0;
        u64 pts_ns = 0;
        u64 tick = 0;
        std::vector<u8> data;
    };
} // namespace robv

namespace robv::video
{
    // nv12 subsamples 2x2
    void allocate_nv12(planar_frame_t& frame, u32 width, u32 height);

    u8* y_plane(planar_frame_t& frame);
    u8* uv_plane(planar_frame_t& frame);

    // colour bars, phase scrolls and hue tints, both 0 to 1
    void fill_test_pattern(planar_frame_t& frame, f32 phase, f32 hue);

    const char* codec_name(video_codec_e codec);
} // namespace robv::video
