#pragma once

#include "robv/common/types.h"

#include <span>
#include <vector>

// H.264 over RTP: single NAL unit packets, FU-A for NALs larger than one packet
// the server packetizes each access unit once and sends the same packets to every client
//
// a client punches the server's RTP port from its own UDP socket, the server sends video back to that address
//   punch   u32 magic "RBVP"  u32 client_id      little endian like the control channel
namespace robv
{
    inline constexpr u8 rtp_payload_type = 96;
    inline constexpr u32 rtp_max_payload = 1200;
    inline constexpr u32 rtp_header_size = 12;
    inline constexpr u32 rtp_punch_magic = 0x50564252;
    inline constexpr u32 rtp_punch_size = 8;

    struct rtp_packetizer_t
    {
        u32 ssrc = 0;
        u16 seq = 0;
    };

    // one buffer, packet i is bytes[ends[i - 1] .. ends[i]), so a frame costs no allocations once warm
    struct rtp_packets_t
    {
        std::vector<u8> bytes;
        std::vector<u32> ends;
    };

    // one Annex B access unit, start codes in front of every NAL
    struct rtp_unit_t
    {
        std::vector<u8> data;
        u32 timestamp = 0;
        bool keyframe = false;
        // false: packets were lost, the decoder conceals the rest and the caller asks for an IDR
        bool complete = true;
    };

    struct rtp_depacketizer_t
    {
        u32 ssrc = 0;
        bool have_seq = false;
        u16 next_seq = 0;

        rtp_unit_t unit;
        bool unit_broken = false;
        bool fu_open = false;
        // a decoder can only start at an intact IDR, so units are dropped until one arrives
        // set at the start and by a caller that threw units away (queue overflow), not by a loss
        bool waiting_for_idr = true;

        u64 packets = 0;
        u64 lost = 0;
        u64 units = 0;
        u64 dropped = 0;
    };
} // namespace robv

namespace robv::rtp
{
    // 90 kHz, wraps every 13 hours, receivers only compare
    u32 timestamp(u64 ns);

    // replaces out
    void packetize(rtp_packetizer_t& p, std::span<const u8> annex_b, u32 timestamp, rtp_packets_t& out);

    // feeds one datagram, appends the access units it finished: none, one, or two when a marker was lost
    // lost is set when a sequence gap was seen, the caller asks the server for an IDR
    void depacketize(rtp_depacketizer_t& d, std::span<const u8> datagram, std::vector<rtp_unit_t>& out, bool& lost);

    void write_punch(std::vector<u8>& out, u32 client_id);
    bool read_punch(std::span<const u8> datagram, u32& client_id);
} // namespace robv::rtp
