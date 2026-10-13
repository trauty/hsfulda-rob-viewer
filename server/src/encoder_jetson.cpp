// V4L2 M2M via the Jetson Multimedia API
// currently only opens the device and checks it is the encoder, not tested on actual hardware
// still to do: NvBufSurface input queue, bitstream capture queue, dmabuf handoff from Vulkan
#include "robv/common/log.h"
#include "robv/server/encoder.h"

#include <cerrno>
#include <cstring>
#include <fcntl.h>
#include <linux/videodev2.h>
#include <sys/ioctl.h>
#include <unistd.h>

namespace robv::encoder
{
    namespace
    {
        constexpr const char* device_path = "/dev/v4l2-nvenc";

        struct jetson_t
        { int fd = -1; };
    } // namespace

    bool open_jetson(encoder_t& encoder)
    {
        jetson_t* j = new jetson_t;
        encoder.backend = j;

        j->fd = ::open(device_path, O_RDWR | O_NONBLOCK | O_CLOEXEC);
        if (j->fd < 0)
        {
            ROBV_LOG_ERROR("ENCODE", "{}: {}. Is this an Orin NX with JetPack installed?", device_path,
                           std::strerror(errno));
            return false;
        }

        v4l2_capability caps{};
        if (ioctl(j->fd, VIDIOC_QUERYCAP, &caps) != 0)
        {
            ROBV_LOG_ERROR("ENCODE", "VIDIOC_QUERYCAP on {}: {}", device_path, std::strerror(errno));
            return false;
        }
        const u32 device_caps = caps.capabilities & V4L2_CAP_DEVICE_CAPS ? caps.device_caps : caps.capabilities;
        if (!(device_caps & V4L2_CAP_VIDEO_M2M_MPLANE))
        {
            ROBV_LOG_ERROR("ENCODE", "{} ({}) is not a multi planar mem2mem device", device_path,
                           reinterpret_cast<const char*>(caps.card));
            return false;
        }

        ROBV_LOG_INFO("ENCODE", "{}: {} / {}", device_path, reinterpret_cast<const char*>(caps.driver),
                      reinterpret_cast<const char*>(caps.card));
        ROBV_LOG_ERROR("ENCODE", "the jetson encoder is not implemented yet, run with --encoder null");
        return false;
    }

    void close_jetson(encoder_t& encoder)
    {
        jetson_t* j = static_cast<jetson_t*>(encoder.backend);
        if (j->fd >= 0) { ::close(j->fd); }
        delete j;
        encoder.backend = nullptr;
    }

    bool encode_jetson(encoder_t&, const planar_frame_t&, std::vector<encoded_packet_t>&) { return false; }
} // namespace robv::encoder
