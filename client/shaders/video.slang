// decoded NV12 video

struct vertex_out_t
{
    float4 position : SV_Position;
    float2 uv : TEXCOORD0;
};

[[vk::binding(0, 0)]]
Sampler2D video;

[shader("vertex")]
vertex_out_t vertex_main(uint id: SV_VulkanVertexID)
{
    const float2 uv = float2((id << 1) & 2, id & 2);
    vertex_out_t result;
    result.position = float4(uv * 2.0 - 1.0, 0.0, 1.0);
    result.uv = uv;
    return result;
}

[shader("fragment")]
float4 fragment_main(vertex_out_t input) : SV_Target { return float4(video.Sample(input.uv).rgb, 1.0); }
