// rgb to nv12

struct constants_t
{
    // luma size in pixels
    uint2 extent;
};

[[vk::push_constant]] ConstantBuffer<constants_t> constants;

[[vk::binding(0, 0)]] [format("rgba8")] RWTexture2D<float4> rgb;
[[vk::binding(1, 0)]] [format("r8")] WTexture2D<float> y_plane;
[[vk::binding(2, 0)]] [format("rg8")] WTexture2D<float2> uv_plane;

static const float3 luma_weights = float3(0.2126, 0.7152, 0.0722);

float to_y(float y) { return (16.0 + 219.0 * y) / 255.0; }
float to_c(float c) { return (128.0 + 224.0 * c) / 255.0; }

[shader("compute")]
[numthreads(8, 8, 1)]
void main(uint3 id: SV_DispatchThreadID)
{
    const uint2 block = id.xy;
    const uint2 origin = block * 2;
    if (any(origin >= constants.extent)) { return; }

    float3 sum = float3(0.0);
    for (uint dy = 0; dy < 2; ++dy)
    {
        for (uint dx = 0; dx < 2; ++dx)
        {
            const uint2 p = origin + uint2(dx, dy);
            const float3 c = saturate(rgb[p].rgb);
            y_plane.Store(p, to_y(dot(c, luma_weights)));
            sum += c;
        }
    }

    const float3 c = sum * 0.25;
    const float y = dot(c, luma_weights);
    const float u = (c.b - y) / 1.8556;
    const float v = (c.r - y) / 1.5748;
    uv_plane.Store(block, float2(to_c(u), to_c(v)));
}
