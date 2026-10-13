#pragma once

#include "robv/common/point_cloud.h"
#include "robv/common/uri.h"

#include <string>
#include <string_view>
#include <vector>

// point clouds by URI scheme
// synthetic://sphere?points=200000&rate=10     generated
// mcap:///data/run.mcap?topic=/livox/lidar     a recording
// socket://0.0.0.0:9500                        ROS 2 bridge or robot feed
namespace robv
{
    enum class source_kind_e : u8
    {
        synthetic,
        mcap,
        socket
    };

    struct source_caps_t
    {
        bool live = false;
        bool seekable = false;
        bool pausable = false;
        u64 begin_ns = 0;
        u64 end_ns = 0;
        std::vector<std::string> topics;
    };

    struct source_t
    {
        source_kind_e kind = source_kind_e::synthetic;
        uri_t uri;
        std::string uri_text;
        source_caps_t caps;
        bool paused = false;
        u64 frames = 0;
        // owned by the backend
        void* backend = nullptr;
    };
} // namespace robv

namespace robv::source
{
    bool open(source_t& source, std::string_view uri);
    void close(source_t& source);

    // newest frame since the last poll, never blocks
    bool poll(source_t& source, u64 now_ns, point_cloud_frame_t& out);

    bool seek(source_t& source, u64 stamp_ns);
    void set_paused(source_t& source, bool paused);

    const char* kind_name(source_kind_e kind);
} // namespace robv::source
