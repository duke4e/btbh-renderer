struct InstanceData
{
    float4x4 model;
    float4 color;
};

StructuredBuffer<InstanceData> instances : register(t0, space0);

cbuffer SceneUniforms : register(b0, space1)
{
    float4x4 view_projection;
    float4 camera_right;
    float4 camera_up;
    uint bone_offset;
    uint billboard_mode;
    uint2 scene_padding;
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

VertexOutput main(VertexInput input, uint instance_id : SV_InstanceID)
{
    const InstanceData instance = instances[instance_id];
    float4 world_position;
    if (billboard_mode == 0)
    {
        world_position = mul(
            instance.model,
            float4(input.position, 1.0f)
        );
    }
    else
    {
        const float3 origin = mul(
            instance.model,
            float4(0.0f, 0.0f, 0.0f, 1.0f)
        ).xyz;
        const float3 local_offset = mul(
            (float3x3)instance.model,
            input.position
        );
        float3 right = camera_right.xyz;
        float3 up = camera_up.xyz;
        if (billboard_mode == 2)
        {
            right = normalize(float3(right.x, 0.0f, right.z));
            up = float3(0.0f, 1.0f, 0.0f);
        }
        const float3 forward = normalize(cross(right, up));
        world_position = float4(
            origin +
                right * local_offset.x +
                up * local_offset.y +
                forward * local_offset.z,
            1.0f
        );
    }
    VertexOutput output;
    output.position = mul(view_projection, world_position);
    output.color = input.color * instance.color;
    output.texture_coordinate = input.texture_coordinate;
    return output;
}
