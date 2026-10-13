#pragma once

#include <cstddef>
#include <cstdint>

using u8 = std::uint8_t;
using u16 = std::uint16_t;
using u32 = std::uint32_t;
using u64 = std::uint64_t;

using i8 = std::int8_t;
using i16 = std::int16_t;
using i32 = std::int32_t;
using i64 = std::int64_t;

using f32 = float;
using f64 = double;

static_assert(sizeof(f32) == 4 && sizeof(f64) == 8, "IEEE 754 single and double precision expected");

namespace robv
{
    struct vec3_t
    {
        f32 x = 0.0f;
        f32 y = 0.0f;
        f32 z = 0.0f;

        bool operator==(const vec3_t&) const = default;
    };
} // namespace robv
