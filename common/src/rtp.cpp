#include "robv/common/rtp.h"

#include <algorithm>

namespace robv::rtp
{
    namespace
    {
        constexpr u8 nal_type_mask = 0x1f;
        constexpr u8 nal_idr = 5;
        constexpr u8 nal_stap_a = 24;
        constexpr u8 nal_fu_a = 28;
        constexpr u8 fu_start = 0x80;
        constexpr u8 fu_end = 0x40;
        constexpr u8 start_code[4] = {0, 0, 0, 1};

        void put_u16_be(u8* out, u16 value)
        {
            out[0] = static_cast<u8>(value >> 8);
            out[1] = static_cast<u8>(value);
        }

        void put_u32_be(u8* out, u32 value)
        {
            for (u32 i = 0; i < 4; ++i) { out[i] = static_cast<u8>(value >> (24 - 8 * i)); }
        }

        u16 get_u16_be(const u8* in) { return static_cast<u16>((in[0] << 8) | in[1]); }

        u32 get_u32_be(const u8* in)
        {
            return (static_cast<u32>(in[0]) << 24) | (static_cast<u32>(in[1]) << 16) | (static_cast<u32>(in[2]) << 8) |
                   in[3];
        }

        // appends one packet: header, then the caller's payload pieces
        void add_packet(rtp_packetizer_t& p, rtp_packets_t& out, u32 timestamp, bool marker, std::span<const u8> prefix,
                        std::span<const u8> payload)
        {
            const size_t start = out.bytes.size();
            out.bytes.resize(start + rtp_header_size);
            u8* header = out.bytes.data() + start;
            header[0] = 0x80; // version 2, no padding, no extension, no CSRC
            header[1] = static_cast<u8>((marker ? 0x80 : 0x00) | rtp_payload_type);
            put_u16_be(header + 2, p.seq++);
            put_u32_be(header + 4, timestamp);
            put_u32_be(header + 8, p.ssrc);
            out.bytes.insert(out.bytes.end(), prefix.begin(), prefix.end());
            out.bytes.insert(out.bytes.end(), payload.begin(), payload.end());
            out.ends.push_back(static_cast<u32>(out.bytes.size()));
        }

        // NAL units between Annex B start codes, without the codes
        // a 4 byte code's leading zero belongs to the code, not the NAL before it
        template <typename fn_t>
        void for_each_nal(std::span<const u8> bytes, fn_t&& fn)
        {
            const size_t size = bytes.size();
            size_t nal_start = SIZE_MAX;
            size_t i = 0;
            while (i + 2 < size)
            {
                if (bytes[i] == 0 && bytes[i + 1] == 0 && bytes[i + 2] == 1)
                {
                    if (nal_start != SIZE_MAX)
                    {
                        size_t end = i;
                        while (end > nal_start && bytes[end - 1] == 0) { --end; }
                        if (end > nal_start) { fn(bytes.subspan(nal_start, end - nal_start)); }
                    }
                    i += 3;
                    nal_start = i;
                    continue;
                }
                ++i;
            }
            if (nal_start != SIZE_MAX && nal_start < size) { fn(bytes.subspan(nal_start)); }
        }

        void append_nal(rtp_depacketizer_t& d, std::span<const u8> nal)
        {
            if (nal.empty()) { return; }
            d.unit.data.insert(d.unit.data.end(), start_code, start_code + 4);
            d.unit.data.insert(d.unit.data.end(), nal.begin(), nal.end());
            if ((nal[0] & nal_type_mask) == nal_idr) { d.unit.keyframe = true; }
        }

        void reset_unit(rtp_depacketizer_t& d)
        {
            d.unit.data.clear();
            d.unit.keyframe = false;
            d.unit_broken = false;
            d.fu_open = false;
        }

        // the unit is still handed out, incomplete: the decoder conceals what is missing
        // dropping it would leave a frame_num gap, after which libavcodec discards frames as out of order
        // until the next IDR, and a frozen view is worse than a brief smear while dragging
        // an FU-A that lost a fragment ends where the loss is, later fragments would land at the wrong offset
        void mark_loss(rtp_depacketizer_t& d, bool& lost)
        {
            d.unit_broken = true;
            d.fu_open = false;
            lost = true;
        }

        // hands the open unit out, false when nothing usable is in it or the decoder cannot start with it
        bool finish_unit(rtp_depacketizer_t& d, std::vector<rtp_unit_t>& out)
        {
            bool usable = !d.unit.data.empty();
            if (usable && d.waiting_for_idr && (d.unit_broken || !d.unit.keyframe)) { usable = false; }
            if (usable)
            {
                d.waiting_for_idr = false;
                ++d.units;
                rtp_unit_t& unit = out.emplace_back();
                unit.data.swap(d.unit.data);
                unit.timestamp = d.unit.timestamp;
                unit.keyframe = d.unit.keyframe;
                unit.complete = !d.unit_broken;
            }
            else
            {
                ++d.dropped;
            }
            reset_unit(d);
            return usable;
        }
    } // namespace

    u32 timestamp(u64 ns) { return static_cast<u32>(ns / 1000 * 9 / 100); }

    void packetize(rtp_packetizer_t& p, std::span<const u8> annex_b, u32 timestamp, rtp_packets_t& out)
    {
        out.bytes.clear();
        out.ends.clear();

        std::vector<std::span<const u8>> nals;
        for_each_nal(annex_b, [&](std::span<const u8> nal) { nals.push_back(nal); });

        for (size_t n = 0; n < nals.size(); ++n)
        {
            const std::span<const u8> nal = nals[n];
            const bool last_nal = n + 1 == nals.size();
            if (nal.size() <= rtp_max_payload)
            {
                add_packet(p, out, timestamp, last_nal, {}, nal);
                continue;
            }

            // FU-A: the NAL header moves into the indicator and the FU header, the body is split
            const u8 indicator = static_cast<u8>((nal[0] & 0xe0) | nal_fu_a);
            const u8 type = nal[0] & nal_type_mask;
            const std::span<const u8> body = nal.subspan(1);
            const size_t chunk = rtp_max_payload - 2;
            for (size_t offset = 0; offset < body.size(); offset += chunk)
            {
                const size_t length = std::min(chunk, body.size() - offset);
                const bool first = offset == 0;
                const bool last = offset + length == body.size();
                const u8 prefix[2] = {indicator, static_cast<u8>((first ? fu_start : 0) | (last ? fu_end : 0) | type)};
                add_packet(p, out, timestamp, last_nal && last, prefix, body.subspan(offset, length));
            }
        }
    }

    void depacketize(rtp_depacketizer_t& d, std::span<const u8> datagram, std::vector<rtp_unit_t>& out, bool& lost)
    {
        lost = false;
        if (datagram.size() < rtp_header_size || (datagram[0] >> 6) != 2) { return; }
        if (get_u32_be(datagram.data() + 8) != d.ssrc) { return; }

        // skip CSRCs and an extension, drop padding
        size_t offset = rtp_header_size + 4u * (datagram[0] & 0x0f);
        size_t size = datagram.size();
        if (datagram[0] & 0x20)
        {
            if (size == 0 || datagram[size - 1] > size) { return; }
            size -= datagram[size - 1];
        }
        if ((datagram[0] & 0x10) && offset + 4 <= size) { offset += 4 + 4u * get_u16_be(datagram.data() + offset + 2); }
        if (offset >= size) { return; }

        const bool marker = datagram[1] & 0x80;
        const u16 seq = get_u16_be(datagram.data() + 2);
        const u32 timestamp = get_u32_be(datagram.data() + 4);
        const std::span<const u8> payload = datagram.subspan(offset, size - offset);
        ++d.packets;

        if (d.have_seq && seq != d.next_seq)
        {
            const i16 ahead = static_cast<i16>(seq - d.next_seq);
            // late or duplicate, its slot was already counted as lost
            if (ahead < 0) { return; }
            d.lost += static_cast<u16>(ahead);
            mark_loss(d, lost);
        }
        d.have_seq = true;
        d.next_seq = static_cast<u16>(seq + 1);

        // a new timestamp with a unit still open means the old unit's marker packet was lost
        // the old unit goes out incomplete, the new one starts clean
        if (!d.unit.data.empty() && timestamp != d.unit.timestamp)
        {
            d.unit_broken = true;
            finish_unit(d, out);
            lost = true;
        }
        d.unit.timestamp = timestamp;

        const u8 type = payload[0] & nal_type_mask;
        if (type >= 1 && type <= 23) { append_nal(d, payload); }
        else if (type == nal_stap_a)
        {
            size_t at = 1;
            while (at + 2 <= payload.size())
            {
                const size_t length = get_u16_be(payload.data() + at);
                at += 2;
                if (at + length > payload.size()) { break; }
                append_nal(d, payload.subspan(at, length));
                at += length;
            }
        }
        else if (type == nal_fu_a && payload.size() >= 2)
        {
            const u8 header = payload[1];
            if (header & fu_start)
            {
                const u8 nal_header = static_cast<u8>((payload[0] & 0xe0) | (header & nal_type_mask));
                d.unit.data.insert(d.unit.data.end(), start_code, start_code + 4);
                d.unit.data.push_back(nal_header);
                if ((nal_header & nal_type_mask) == nal_idr) { d.unit.keyframe = true; }
                d.fu_open = true;
            }
            else if (!d.fu_open)
            {
                // the start fragment is missing, the sequence check usually catches this first
                d.unit_broken = true;
            }
            if (d.fu_open) { d.unit.data.insert(d.unit.data.end(), payload.begin() + 2, payload.end()); }
            if (header & fu_end) { d.fu_open = false; }
        }

        if (marker) { finish_unit(d, out); }
    }

    void write_punch(std::vector<u8>& out, u32 client_id)
    {
        out.clear();
        for (const u32 value : {rtp_punch_magic, client_id})
        {
            for (u32 i = 0; i < 4; ++i) { out.push_back(static_cast<u8>(value >> (8 * i))); }
        }
    }

    bool read_punch(std::span<const u8> datagram, u32& client_id)
    {
        if (datagram.size() != rtp_punch_size) { return false; }
        u32 values[2] = {};
        for (u32 v = 0; v < 2; ++v)
        {
            for (u32 i = 0; i < 4; ++i) { values[v] |= static_cast<u32>(datagram[4 * v + i]) << (8 * i); }
        }
        client_id = values[1];
        return values[0] == rtp_punch_magic;
    }
} // namespace robv::rtp
