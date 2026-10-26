struct constants_t
{
    float4x4 view_projection;
    float2 grid_center;
};

[[vk::push_constant]]
ConstantBuffer<constants_t> constants;

// keep this sync with renderer.cpp, reflection would be overkill
static const int GRID_HALF_LINES = 20;
static const uint GRID_LINES_PER_AXIS = uint(GRID_HALF_LINES * 2 + 1);
static const uint GRID_VERTICES = GRID_LINES_PER_AXIS * 4u;

struct vertex_out_t
{
    float4 position : SV_Position;
    float3 color : COLOR0;
};

[shader("vertex")]
vertex_out_t vertex_main(uint id: SV_VulkanVertexID)
{
    float3 p;
    float3 color;
    if (id < GRID_VERTICES)
    {
        const uint line = id / 2u;
        const float along = (id & 1u) == 0u ? -float(GRID_HALF_LINES) : float(GRID_HALF_LINES);
        const float across = float(int(line % GRID_LINES_PER_AXIS) - GRID_HALF_LINES);
        // first half runs along x, second along y
        p = line < GRID_LINES_PER_AXIS ? float3(along, across, 0.0) : float3(across, along, 0.0);
        p.xy += constants.grid_center;
        color = float3(0.22, 0.24, 0.27);
    }
    else
    {
        const uint axis = (id - GRID_VERTICES) / 2u;
        const float length = (id & 1u) == 0u ? 0.0 : 1.0;
        color = float3(axis == 0u ? 1.0 : 0.0, axis == 1u ? 1.0 : 0.0, axis == 2u ? 1.0 : 0.0);
        p = color * length;
    }

    vertex_out_t result;
    result.position = mul(constants.view_projection, float4(p, 1.0));
    result.color = color;
    return result;
}

[shader("fragment")]
float4 fragment_main(vertex_out_t input) : SV_Target { return float4(input.color, 1.0); }
