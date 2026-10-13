#include "robv/common/point_cloud.h"

#include <string_view>

namespace robv::point_cloud
{
    namespace
    {
        i32 find_field(std::span<const point_field_t> fields, std::string_view name)
        {
            for (size_t i = 0; i < fields.size(); ++i)
            {
                if (fields[i].name == name) { return static_cast<i32>(i); }
            }
            return -1;
        }

        bool is_coordinate(const point_field_t& field)
        { return field.count == 1 && (field.type == field_type_e::float32 || field.type == field_type_e::float64); }

        bool fits(const point_field_t& field, u32 point_stride)
        { return field.offset + field_type_size(field.type) * field.count <= point_stride; }
    } // namespace

    u32 field_type_size(field_type_e type)
    {
        switch (type)
        {
        case field_type_e::int8:
        case field_type_e::uint8: return 1;
        case field_type_e::int16:
        case field_type_e::uint16: return 2;
        case field_type_e::int32:
        case field_type_e::uint32:
        case field_type_e::float32: return 4;
        case field_type_e::float64: return 8;
        }
        return 0;
    }

    bool is_valid_field_type(u8 value) { return value >= 1 && value <= 8; }

    cloud_layout_t resolve_layout(std::span<const point_field_t> fields)
    {
        cloud_layout_t layout;

        layout.x = find_field(fields, "x");
        layout.y = find_field(fields, "y");
        layout.z = find_field(fields, "z");
        if (layout.x >= 0 && !is_coordinate(fields[layout.x])) { layout.x = -1; }
        if (layout.y >= 0 && !is_coordinate(fields[layout.y])) { layout.y = -1; }
        if (layout.z >= 0 && !is_coordinate(fields[layout.z])) { layout.z = -1; }

        // Livox and Velodyne say intensity, Ouster has both and intensity wins
        layout.intensity = find_field(fields, "intensity");
        if (layout.intensity < 0) { layout.intensity = find_field(fields, "reflectivity"); }

        // PCL packs rgb into a float32 or uint32, bytes b g r a
        layout.rgb = find_field(fields, "rgb");
        if (layout.rgb < 0) { layout.rgb = find_field(fields, "rgba"); }
        if (layout.rgb >= 0 && field_type_size(fields[layout.rgb].type) != 4) { layout.rgb = -1; }

        return layout;
    }

    bool make_gpu_layout(std::span<const point_field_t> fields, const cloud_layout_t& layout, u32 point_stride,
                         cloud_gpu_layout_t& out)
    {
        out = {};
        if (!layout.has_xyz() || point_stride == 0) { return false; }

        const point_field_t& x = fields[layout.x];
        const point_field_t& y = fields[layout.y];
        const point_field_t& z = fields[layout.z];

        // float64 would need a conversion pass, sensors send float32
        const bool float32_xyz =
            x.type == field_type_e::float32 && y.type == field_type_e::float32 && z.type == field_type_e::float32;
        const bool packed_xyz = y.offset == x.offset + 4 && z.offset == x.offset + 8;
        if (!float32_xyz || !packed_xyz || !fits(z, point_stride)) { return false; }

        // the shader reads whole u32 words from a storage buffer
        if (point_stride % 4 != 0 || x.offset % 4 != 0) { return false; }

        out.stride = point_stride;
        out.offset_xyz = x.offset;

        if (layout.intensity >= 0 && fits(fields[layout.intensity], point_stride))
        {
            const point_field_t& intensity = fields[layout.intensity];
            const u32 size = field_type_size(intensity.type);
            if (intensity.offset % size == 0)
            {
                out.offset_intensity = intensity.offset;
                out.type_intensity = static_cast<u32>(intensity.type);
                out.flags |= CLOUD_HAS_INTENSITY;
            }
        }

        if (layout.rgb >= 0 && fits(fields[layout.rgb], point_stride) && fields[layout.rgb].offset % 4 == 0)
        {
            out.offset_rgb = fields[layout.rgb].offset;
            out.flags |= CLOUD_HAS_RGB;
        }
        return true;
    }

    u64 point_count(const point_cloud_frame_t& frame) { return static_cast<u64>(frame.width) * frame.height; }
} // namespace robv::point_cloud
