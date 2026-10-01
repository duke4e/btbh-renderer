struct RadianceEmitter
{
    float4 position_radius;
    float4 radiance_range;
    float4 direction_inner;
    float4 outer_shape;
};

StructuredBuffer<RadianceEmitter> emitters : register(t0, space0);

cbuffer RadianceDirectLightUniforms : register(b0, space1)
{
    float4 origin_cell;
    uint2 field_dimensions;
    uint2 padding;
};

struct VertexOutput
{
    float4 position : SV_Position;
    float2 field_uv : TEXCOORD0;
    nointerpolation float2 source_uv : TEXCOORD1;
    float3 radiance : TEXCOORD2;
    nointerpolation float2 forward : TEXCOORD3;
    nointerpolation float4 cone_parameters : TEXCOORD4;
    nointerpolation float3 field_size_cell : TEXCOORD5;
    nointerpolation uint is_spot : TEXCOORD6;
};

static const float2 corners[6] =
{
    float2(-1.0f, -1.0f),
    float2( 1.0f, -1.0f),
    float2( 1.0f,  1.0f),
    float2(-1.0f, -1.0f),
    float2( 1.0f,  1.0f),
    float2(-1.0f,  1.0f)
};

VertexOutput main(uint vertex_id : SV_VertexID, uint instance_id : SV_InstanceID)
{
    const RadianceEmitter emitter = emitters[instance_id];
    const float2 local = corners[vertex_id];
    const float range = max(emitter.radiance_range.w, origin_cell.z);
    const float2 source_world = emitter.position_radius.xz;
    const float2 world = source_world + local * range;
    const float2 field_uv =
        ((world - origin_cell.xy) * origin_cell.w) /
        max(float2(field_dimensions), 1.0f);
    const float2 source_uv =
        ((source_world - origin_cell.xy) * origin_cell.w) /
        max(float2(field_dimensions), 1.0f);
    const float2 forward = dot(
        emitter.direction_inner.xz,
        emitter.direction_inner.xz
    ) > 0.000001f
        ? normalize(emitter.direction_inner.xz)
        : float2(1.0f, 0.0f);
    const float inner_cutoff = emitter.direction_inner.w;
    const float outer_cutoff = emitter.outer_shape.x;
    const bool valid_spot =
        inner_cutoff > outer_cutoff &&
        outer_cutoff > 0.0001f &&
        inner_cutoff <= 1.0f;

    VertexOutput output;
    output.position = float4(
        field_uv.x * 2.0f - 1.0f,
        1.0f - field_uv.y * 2.0f,
        0.0f,
        1.0f
    );
    output.field_uv = field_uv;
    output.source_uv = source_uv;
    output.radiance =
        emitter.radiance_range.rgb * max(emitter.outer_shape.z, 0.0f);
    output.forward = forward;
    output.cone_parameters = float4(
        inner_cutoff,
        outer_cutoff,
        range * origin_cell.w,
        max(emitter.position_radius.y, 0.0f)
    );
    output.field_size_cell = float3(
        float2(field_dimensions),
        origin_cell.z
    );
    output.is_spot = valid_spot ? 1u : 0u;
    return output;
}
