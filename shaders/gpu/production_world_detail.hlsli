#ifndef BTBH_WORLD_DETAIL_HLSLI
#define BTBH_WORLD_DETAIL_HLSLI

// RFC 0008: a batch's world detail (BTBHGPUSceneGeometryBatch.world_detail), a second texture set projected from
// the flat world position by the face's own normal. The projection, its frame, the seed's hash and the normal map's
// tilt live here, apart from the scene shader, so that their C copy (src/gpu/btbh_gpu_world_detail.h) can be
// tested natively: keep the two in step. RFC 0009's table (world_detail_table) adds a vertex's index and the
// entry it picks.

// world_detail_mode.x: the maps the batch samples (0: none, the whole path is skipped).
static const uint btbh_world_detail_map_albedo = 1u;
static const uint btbh_world_detail_map_normal = 2u;
static const uint btbh_world_detail_map_orm = 4u;
static const uint btbh_world_detail_map_albedo_in_orm_blue = 8u;
// RFC 0009: the batch draws its table (the WorldDetailTable block, the world_detail_maps array); y is then the
// table's seed component.
static const uint btbh_world_detail_map_table = 16u;
static const uint btbh_world_detail_table_max_entries = 8u;
// An entry's layer that is absent; the vertex index that means "no detail".
static const uint btbh_world_detail_no_layer = 255u;

// Where the face normal and the vertex normal are this close to perpendicular, the face normal is turned toward
// the camera instead of to the vertex normal's side.
static const float btbh_world_detail_camera_side = 0.05f;
// The seed is clamped to the floats that convert to a 32-bit int.
static const float btbh_world_detail_seed_limit = 2147483520.0f;

struct BTBHWorldDetailFrame
{
    float3 normal;     // the face normal N, on the vertex normal's side
    float3 tangent;    // T, the image's right
    float3 bitangent;  // B = cross(N, T), toward the image's top
    float2 uv;         // in repeats, before the flip to the image's rows and the seed's offset
};

float3 btbh_world_detail_normalize(float3 value, float3 fallback)
{
    const float length_squared = dot(value, value);
    return length_squared > 1.0e-30f ? value * rsqrt(length_squared) : fallback;
}

// face: the face's normal either way round (cross(ddx(P), ddy(P))); vertex_normal: which side is outside;
// to_camera: for a face whose vertex normal lies in its plane; inverse_tiles: 1 / level_tile (u, v),
// 1 / wall_tile (u, v).
// Level faces (|N.y| >= cos_level): u = x, v = -z (north), or v = +z on a face that looks down; T is +x made
// orthogonal to N. Other faces: T = normalize(cross(up, N)), B = cross(N, T), u = dot(P, T), v = dot(P, B): the
// height on vertical walls, the distance up the slope on pitched faces.
BTBHWorldDetailFrame btbh_world_detail_frame(
    float3 position,
    float3 face,
    float3 vertex_normal,
    float3 to_camera,
    float cos_level,
    float4 inverse_tiles)
{
    const float3 side_normal = btbh_world_detail_normalize(vertex_normal, float3(0.0f, 0.0f, 0.0f));
    float3 normal = btbh_world_detail_normalize(
        face,
        dot(side_normal, side_normal) > 0.0f ? side_normal : float3(0.0f, 1.0f, 0.0f));
    const float facing = dot(normal, side_normal);
    const float side = abs(facing) < btbh_world_detail_camera_side ? dot(normal, to_camera) : facing;
    if (side < 0.0f)
    {
        normal = -normal;
    }
    BTBHWorldDetailFrame frame;
    frame.normal = normal;
    const bool level = abs(normal.y) >= cos_level;
    if (level)
    {
        frame.tangent = btbh_world_detail_normalize(
            float3(1.0f - normal.x * normal.x, -normal.y * normal.x, -normal.z * normal.x),
            float3(1.0f, 0.0f, 0.0f));
    }
    else
    {
        frame.tangent = btbh_world_detail_normalize(float3(normal.z, 0.0f, -normal.x), float3(1.0f, 0.0f, 0.0f));
    }
    frame.bitangent = cross(normal, frame.tangent);
    if (level)
    {
        frame.uv = float2(
            position.x * inverse_tiles.x,
            (normal.y >= 0.0f ? -position.z : position.z) * inverse_tiles.y);
    }
    else
    {
        frame.uv = float2(dot(position, frame.tangent) * inverse_tiles.z, dot(position, frame.bitangent) * inverse_tiles.w);
    }
    return frame;
}

// PCG (Jarzynski and Olano, "Hash Functions for GPU Rendering", 2020).
uint btbh_world_detail_hash(uint value)
{
    const uint state = value * 747796405u + 2891336453u;
    const uint word = ((state >> ((state >> 28u) + 4u)) ^ state) * 277803737u;
    return (word >> 22u) ^ word;
}

// The seed's offset in repeats along u and v, each in [0, 1): the instance's custom float `component` (1..4: x..w)
// rounded to a whole number (it reaches the fragment interpolated, so it may be off by an ulp), hashed, its two
// 16-bit halves. 0 for component 0.
float2 btbh_world_detail_offset(float4 custom, uint component)
{
    if (component == 0u)
    {
        return float2(0.0f, 0.0f);
    }
    const float raw = component == 1u ? custom.x : (component == 2u ? custom.y : (component == 3u ? custom.z : custom.w));
    const float seed = clamp(round(raw), -btbh_world_detail_seed_limit, btbh_world_detail_seed_limit);
    const uint hash = btbh_world_detail_hash((uint)(int)seed);
    return float2((float)(hash & 0xffffu), (float)(hash >> 16u)) * (1.0f / 65536.0f);
}

// The texture coordinate: (u, -v) plus the offset (the image's top lies toward +v, as an OpenGL normal map's +Y).
float2 btbh_world_detail_coordinate(BTBHWorldDetailFrame frame, float2 offset)
{
    return float2(frame.uv.x + offset.x, -frame.uv.y + offset.y);
}

// The surface gradient a tangent-space normal adds (normal_xy: decoded, times normal_scale); the shading normal
// becomes normalize(normal - gradient), as with the procedural detail's gradient.
float3 btbh_world_detail_gradient(BTBHWorldDetailFrame frame, float2 normal_xy)
{
    const float z = sqrt(1.0f - saturate(dot(normal_xy, normal_xy)));
    return -(normal_xy.x * frame.tangent + normal_xy.y * frame.bitangent) / max(z, 1.0e-3f);
}

// RFC 0009: a vertex's table index from the first float of its joint slot: rounded to the nearest whole number,
// 0 below 0, 255 above 255, and 255 (none) for NaN (neither comparison holds).
uint btbh_world_detail_table_index(float joint)
{
    const float rounded = round(joint);
    return rounded >= 0.0f ? (uint)min(rounded, 255.0f) : (rounded < 0.0f ? 0u : btbh_world_detail_no_layer);
}

// The entry whose block an index reads: min(index, 7). Past the count it is read and thrown away.
uint btbh_world_detail_table_slot(uint index)
{
    return min(index, btbh_world_detail_table_max_entries - 1u);
}

// Whether a map of the entry an index picks changes the pixel: the index is below the count and the map's layer
// isn't 255.
bool btbh_world_detail_table_applies(uint index, uint count, uint layer)
{
    return index < count && layer != btbh_world_detail_no_layer;
}

#endif
