#pragma once

#include "robv/common/types.h"

#include <memory>
#include <span>
#include <string>
#include <vector>

namespace robv
{
    // sensor_msgs PointField values
    enum class field_type_e : u8
    {
        int8 = 1,
        uint8 = 2,
        int16 = 3,
        uint16 = 4,
        int32 = 5,
        uint32 = 6,
        float32 = 7,
        float64 = 8
    };

    struct point_field_t
    {
        std::string name;
        u32 offset = 0;
        field_type_e type = field_type_e::float32;
        u32 count = 1;

        bool operator==(const point_field_t&) const = default;
    };

    // ROS free mirror of PointCloud2
    // data is the raw sensor blob, unpacked by the vertex shader via cloud_gpu_layout_t
    struct point_cloud_frame_t
    {
        u64 stamp_ns = 0;
        std::string frame_id;
        u32 width = 0;
        u32 height = 1;
        u32 point_stride = 0;
        u32 row_stride = 0;
        bool is_dense = true;
        bool little_endian = true;
        std::vector<point_field_t> fields;
        std::shared_ptr<const std::vector<std::byte>> data;
    };

    // indices into fields, minus one when absent
    struct cloud_layout_t
    {
        i32 x = -1;
        i32 y = -1;
        i32 z = -1;
        i32 intensity = -1;
        i32 rgb = -1;

        bool has_xyz() const { return x >= 0 && y >= 0 && z >= 0; }
    };

    // point vertex shader push constants, keep in sync with the Slang struct
    struct cloud_gpu_layout_t
    {
        u32 stride = 0;
        u32 offset_xyz = 0;
        u32 offset_intensity = 0;
        u32 offset_rgb = 0;
        u32 type_intensity = 0; // field_type_e, 0 if none
        u32 flags = 0;

        bool operator==(const cloud_gpu_layout_t&) const = default;
    };

    enum cloud_gpu_flags_e : u32
    {
        CLOUD_HAS_INTENSITY = 1u << 0,
        CLOUD_HAS_RGB = 1u << 1,
    };
} // namespace robv

namespace robv::point_cloud
{
    u32 field_type_size(field_type_e type);
    bool is_valid_field_type(u8 value);

    // well known fields by name: x y z, intensity or reflectivity, rgb or rgba
    cloud_layout_t resolve_layout(std::span<const point_field_t> fields);

    bool make_gpu_layout(std::span<const point_field_t> fields, const cloud_layout_t& layout, u32 point_stride,
                         cloud_gpu_layout_t& out);

    u64 point_count(const point_cloud_frame_t& frame);
} // namespace robv::point_cloud
