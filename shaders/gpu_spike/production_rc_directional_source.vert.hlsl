struct RadianceEmitter
{
    float4 position_radius;
    float4 radiance_height;
    float4 direction_inner;
    float4 outer_shape;
};

StructuredBuffer<RadianceEmitter> emitters : register(t0, space0);

cbuffer RadianceDirectionalSourceUniforms : register(b0, space1)
{
    float4 origin_cell;
    uint2 field_dimensions;
    uint2 direction_tile;
    uint direction_count;
    uint3 padding;
};

struct VertexOutput
{
    float4 position : SV_Position;
    float2 local_position : TEXCOORD0;
    float3 radiance : TEXCOORD1;
    nointerpolation float angular_coverage : TEXCOORD2;
    nointerpolation uint is_spot : TEXCOORD3;
};

static const float two_pi = 6.28318530718f;
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
    const uint emitter_index = instance_id / direction_count;
    const uint direction_index = instance_id % direction_count;
    const RadianceEmitter emitter = emitters[emitter_index];
    const float2 local = corners[vertex_id];
    // A physical emitter can be smaller than one visibility texel. Rasterize
    // a conservative footprint so sub-cell lights cannot disappear between
    // pixel centers; this changes source capture, not the transported range.
    const float radius = max(
        emitter.position_radius.w,
        origin_cell.z * 1.5f
    );
    const float2 world = emitter.position_radius.xz + local * radius;
    const float2 field_uv =
        ((world - origin_cell.xy) * origin_cell.w) /
        max(float2(field_dimensions), 1.0f);
    const uint2 tile = uint2(
        direction_index % direction_tile.x,
        direction_index / direction_tile.x
    );
    const float2 atlas_uv =
        (float2(tile) + field_uv) / float2(direction_tile);

    const float2 forward = dot(
        emitter.direction_inner.xz,
        emitter.direction_inner.xz
    ) > 0.000001f
        ? normalize(emitter.direction_inner.xz)
        : float2(1.0f, 0.0f);
    const float angle = two_pi *
        (float(direction_index) + 0.5f) / float(direction_count);
    const float2 receiver_to_source = float2(cos(angle), sin(angle));
    const float emission_cosine = dot(forward, -receiver_to_source);
    const float inner_cutoff = emitter.direction_inner.w;
    const float outer_cutoff = emitter.outer_shape.x;
    const bool valid_spot =
        inner_cutoff > outer_cutoff &&
        outer_cutoff > 0.0001f &&
        inner_cutoff <= 1.0f;

    VertexOutput output;
    output.position = float4(
        atlas_uv.x * 2.0f - 1.0f,
        1.0f - atlas_uv.y * 2.0f,
        0.0f,
        1.0f
    );
    output.local_position = local;
    output.radiance = emitter.radiance_height.rgb;
    output.angular_coverage = valid_spot
        ? smoothstep(outer_cutoff, inner_cutoff, emission_cosine)
        : 0.0f;
    output.is_spot = valid_spot ? 1u : 0u;
    return output;
}
