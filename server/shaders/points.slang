// point cloud straight from the sensor blob
// the vertex shader finds each point's fields through the layout in the push constants (cloud_gpu_layout_t),
// so XYZ, XYZI, XYZRGB and driver specific layouts need no CPU conversion

struct constants_t
{
    float4x4 view_projection;
    // cloud_gpu_layout_t
    uint stride;
    uint offset_xyz;
    uint offset_intensity;
    uint offset_rgb;
    uint type_intensity;
    uint flags;
    // rows of an organised cloud can be padded
    uint width;
    uint row_stride;
    // view_state render settings
    uint color_mode;
    float point_size;
    float intensity_min;
    float intensity_max;
    // z range of the cloud for the height colours
    float height_min;
    float height_max;
};

[[vk::push_constant]]
ConstantBuffer<constants_t> constants;

[[vk::binding(0, 0)]]
ByteAddressBuffer points;

static const uint CLOUD_HAS_INTENSITY = 1;
static const uint CLOUD_HAS_RGB = 2;

static const uint COLOR_INTENSITY = 0;
static const uint COLOR_HEIGHT = 1;
static const uint COLOR_RGB = 2;
static const uint COLOR_FLAT = 3;

struct vertex_out_t
{
    float4 position : SV_Position;
    float point_size : SV_PointSize;
    float3 color : COLOR0;
};

// turbo color map, polynomial fit by Google (Apache 2.0)
float3 turbo(float t)
{
    const float4 red4 = float4(0.13572138, 4.61539260, -42.66032258, 132.13108234);
    const float4 green4 = float4(0.09140261, 2.19418839, 4.84296658, -14.18503333);
    const float4 blue4 = float4(0.10667330, 12.64194608, -60.58204836, 110.36276771);
    const float2 red2 = float2(-152.94239396, 59.28637943);
    const float2 green2 = float2(4.27729857, 2.82956604);
    const float2 blue2 = float2(-89.90310912, 27.34824973);

    t = saturate(t);
    const float4 v4 = float4(1.0, t, t * t, t * t * t);
    const float2 v2 = v4.zw * v4.z;
    return saturate(
        float3(dot(v4, red4) + dot(v2, red2), dot(v4, green4) + dot(v2, green2), dot(v4, blue4) + dot(v2, blue2)));
}

// sensor_msgs PointField types, the buffer is read in aligned 32 bit words
float read_scalar(uint offset, uint type)
{
    const uint word = points.Load(offset & ~3u);
    const uint shift = (offset & 3u) * 8u;
    switch (type)
    {
    case 1: return float(int(word << (24u - shift)) >> 24);
    case 2: return float((word >> shift) & 0xffu);
    case 3: return float(int(word << (16u - shift)) >> 16);
    case 4: return float((word >> shift) & 0xffffu);
    case 5: return float(int(word));
    case 6: return float(word);
    case 7: return asfloat(word);
    case 8:
    {
        // float64 without the Float64 capability: the high word holds sign, exponent and 20 mantissa bits
        const uint high = points.Load(offset + 4u);
        const int exponent = int((high >> 20u) & 0x7ffu) - 1023;
        if (exponent == -1023) { return 0.0; }
        const float mantissa = 1.0 + float(high & 0xfffffu) / 1048576.0;
        const float value = ldexp(mantissa, float(exponent));
        return (high & 0x80000000u) != 0u ? -value : value;
    }
    default: return 0.0;
    }
}

float range_fraction(float value, float low, float high) { return (value - low) / max(high - low, 1e-6); }

[shader("vertex")]
vertex_out_t vertex_main(uint id: SV_VulkanVertexID)
{
    const uint row = id / constants.width;
    const uint base = row * constants.row_stride + (id - row * constants.width) * constants.stride;

    const uint xyz = base + constants.offset_xyz;
    const float3 p = float3(asfloat(points.Load(xyz)), asfloat(points.Load(xyz + 4u)), asfloat(points.Load(xyz + 8u)));

    const bool has_intensity = (constants.flags & CLOUD_HAS_INTENSITY) != 0u;
    const bool has_rgb = (constants.flags & CLOUD_HAS_RGB) != 0u;

    // intensity or rgb on a cloud without that field falls back to height
    float3 color = float3(0.85);
    if (constants.color_mode == COLOR_RGB && has_rgb)
    {
        // PCL packs bytes b g r a into one word
        const uint packed = points.Load(base + constants.offset_rgb);
        color = float3(float((packed >> 16u) & 0xffu), float((packed >> 8u) & 0xffu), float(packed & 0xffu)) / 255.0;
    }
    else if (constants.color_mode == COLOR_INTENSITY && has_intensity)
    {
        const float intensity = read_scalar(base + constants.offset_intensity, constants.type_intensity);
        color = turbo(range_fraction(intensity, constants.intensity_min, constants.intensity_max));
    }
    else if (constants.color_mode != COLOR_FLAT)
    {
        color = turbo(range_fraction(p.z, constants.height_min, constants.height_max));
    }

    vertex_out_t result;
    // non dense clouds mark missing returns with NaN, those go outside the clip volume
    result.position = any(isnan(p)) ? float4(2.0, 2.0, 2.0, 1.0) : mul(constants.view_projection, float4(p, 1.0));
    result.point_size = constants.point_size;
    result.color = color;
    return result;
}

[shader("fragment")]
float4 fragment_main(vertex_out_t input) : SV_Target { return float4(input.color, 1.0); }
