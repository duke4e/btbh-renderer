// RFC 0013 phase 2 (0.14.0): a batch with BTBH_GPU_SCENE_BATCH_PARTICLE_SHADOW drawn into the particle transmittance
// map, in sun space (the sun map's matrix, the one the receivers read). A billboard faces the sun, so that a puff
// shadows with its whole disc: FRONT lays the quad's local x and y along the matrix's first two rows (the sun's right
// and up), Y_AXIS turns it round y toward the sun; other batches are drawn as they stand. The surface variants the map
// takes (NONE, SOFT_PARTICLE, LIT_PARTICLE) move no vertex, and the map is in the flat world, as the shadow maps are.
// production_particle_transmittance.frag turns the fragments' alpha into optical depth.

struct InstanceData
{
    float4x4 model;
    float4 color;
    float4 custom;
};

StructuredBuffer<InstanceData> instances : register(t0, space0);

cbuffer TransmittanceUniforms : register(b0, space1)
{
    float4x4 light_view_projection;
    float4 material_color;
    // the batch's uv_scale (xy) and uv_offset (zw)
    float4 uv_transform;
    // x the billboard mode (BTBHGPUSceneBillboardMode), y 1 with BTBH_GPU_SCENE_BATCH_INSTANCE_UV_RECT, zw 0
    uint4 transmittance_options;
};

struct VertexInput
{
    float3 position : TEXCOORD0;
    float4 color : TEXCOORD1;
    float2 texture_coordinate : TEXCOORD2;
};

struct VertexOutput
{
    float4 position : SV_Position;
    float4 color : TEXCOORD0;
    float2 texture_coordinate : TEXCOORD1;
};

float3 transmittance_normalize(float3 value, float3 fallback)
{
    const float length_squared = dot(value, value);
    return length_squared > 1.0e-30f
        ? value * rsqrt(length_squared)
        : fallback;
}

VertexOutput main(VertexInput input, uint instance_id : SV_InstanceID)
{
    const InstanceData instance = instances[instance_id];
    const uint billboard_mode = transmittance_options.x;
    float4 world_position;
    if (billboard_mode == 0u)
    {
        world_position = mul(instance.model, float4(input.position, 1.0f));
    }
    else
    {
        const float3 origin = mul(instance.model, float4(0.0f, 0.0f, 0.0f, 1.0f)).xyz;
        const float3 local_offset = mul((float3x3)instance.model, input.position);
        float3 right = transmittance_normalize(light_view_projection[0].xyz, float3(1.0f, 0.0f, 0.0f));
        float3 up = transmittance_normalize(light_view_projection[1].xyz, float3(0.0f, 1.0f, 0.0f));
        if (billboard_mode == 2u)
        {
            right = transmittance_normalize(float3(right.x, 0.0f, right.z), float3(1.0f, 0.0f, 0.0f));
            up = float3(0.0f, 1.0f, 0.0f);
        }
        const float3 forward = transmittance_normalize(cross(right, up), float3(0.0f, 0.0f, 1.0f));
        world_position = float4(
            origin + right * local_offset.x + up * local_offset.y + forward * local_offset.z,
            1.0f
        );
    }
    VertexOutput output;
    output.position = mul(light_view_projection, world_position);
    output.color = input.color * instance.color * material_color;
    output.texture_coordinate = input.texture_coordinate * uv_transform.xy + uv_transform.zw;
    if (transmittance_options.y != 0u)
    {
        output.texture_coordinate = output.texture_coordinate * instance.custom.xy + instance.custom.zw;
    }
    return output;
}
