#pragma once

#include "robv/common/point_cloud.h"

#include <string>
#include <vector>

// bridge side of socket://, framed with protocol.h
// no ROS in here, builds without a ROS install
namespace robv
{
    struct socket_sender_t
    {
        std::string host;
        u16 port = 0;
        int fd = -1;
        u64 frames_sent = 0;
        u64 bytes_sent = 0;
        u64 frames_dropped = 0;
        std::vector<u8> scratch;
    };
} // namespace robv

namespace robv::socket_sender
{
    // "host:port", IPv4 only, does not connect
    bool init(socket_sender_t& sender, const std::string& server);
    void shutdown(socket_sender_t& sender);

    // false when the server is not there, caller retries later
    bool ensure_connected(socket_sender_t& sender);
    bool is_connected(const socket_sender_t& sender);

    // blocks until sent or failed
    // a failed frame is dropped, never queued
    bool send(socket_sender_t& sender, const point_cloud_frame_t& frame);
} // namespace robv::socket_sender
