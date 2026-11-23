struct RadianceEmitter
{
    float4 position_radius;
    float4 radiance_height;
    float4 direction_inner;
    float4 outer_shape;
};

StructuredBuffer<RadianceEmitter> emitters : register(t0, space0);
// RFC 0010: each emitter's top (position_y + top_above with source_tops 1, else 0), in the records' order.
StructuredBuffer<float> emitter_tops : register(t1, space0);

cbuffer RadianceSourceUniforms : register(b0, space1)
{
    float4 origin_cell;
    uint2 dimensions;
    // RFC 0010: BTBHGPUSceneRadianceCascades.source_tops. A spot then draws its top over its directional footprint.
    uint source_tops;
    uint padding;
};

struct VertexOutput
{
    float4 position : SV_Position;
    float2 local_position : TEXCOORD0;
    float3 radiance : TEXCOORD1;
    nointerpolation uint shape : TEXCOORD2;
    nointerpolation float4 spot_parameters : TEXCOORD3;
    // RFC 0010: the source's top, for the tops texture (SV_Target1).
    nointerpolation float top : TEXCOORD4;
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
    const uint shape = (uint)round(emitter.outer_shape.y);
    const float2 direction = dot(
        emitter.direction_inner.xz,
        emitter.direction_inner.xz
    ) > 0.000001f
        ? normalize(emitter.direction_inner.xz)
        : float2(1.0f, 0.0f);
    const float2 side = float2(-direction.y, direction.x);
    const float radius = max(emitter.position_radius.w, 0.0001f);
    // RFC 0010: with source_tops, a spot (which lights only through the directional-source atlas) draws its top as
    // that pass draws its light: a disk at least 1.5 texels round it. Otherwise the quad is as before.
    const bool spot_top =
        source_tops != 0u &&
        emitter.direction_inner.w > emitter.outer_shape.x &&
        emitter.outer_shape.x > 0.0001f &&
        emitter.direction_inner.w <= 1.0f;
    const float footprint_radius = spot_top
        ? max(emitter.position_radius.w, origin_cell.z * 1.5f)
        : radius;
    const float2 disk_offset = local * footprint_radius;
    const float segment_half_width = max(
        min(emitter.radiance_height.w * 0.5f, radius),
        radius * 0.08f
    );
    const float2 segment_offset =
        direction * (local.x * radius) +
        side * (local.y * segment_half_width);
    const float2 world_offset = shape == 2u && !spot_top
        ? segment_offset
        : disk_offset;
    const float2 world = emitter.position_radius.xz + world_offset;
    const float2 field_uv =
        ((world - origin_cell.xy) * origin_cell.w) /
        max(float2(dimensions), 1.0f);

    VertexOutput output;
    output.position = float4(
        field_uv.x * 2.0f - 1.0f,
        1.0f - field_uv.y * 2.0f,
        0.0f,
        1.0f
    );
    output.local_position = local;
    output.radiance = emitter.radiance_height.rgb;
    output.shape = shape;
    output.spot_parameters = float4(
        emitter.direction_inner.w,
        emitter.outer_shape.x,
        radius,
        emitter.radiance_height.w
    );
    output.top = emitter_tops[instance_id];
    return output;
}
