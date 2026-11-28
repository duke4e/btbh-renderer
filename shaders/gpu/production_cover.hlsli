// RFC 0020 (since 0.18.0): the cover laid by facing (btbh_gpu_window_scene_set_cover) and still ice
// (BTBH_GPU_SCENE_SURFACE_ICE), both worked out from the world position alone: no texture. The CPU reference,
// src/gpu/btbh_gpu_cover.h, does the same arithmetic in the same order for the tests; keep the two in step.
// Needs production_surface.hlsli first (btbh_detail_hash's integer hash, stable at a city's coordinates).
#ifndef BTBH_COVER_HLSLI
#define BTBH_COVER_HLSLI

static const uint btbh_cover_pattern_snow = 0u;
static const uint btbh_cover_pattern_sand = 1u;
static const uint btbh_cover_pattern_mud = 2u;
// cover_pattern[layer].y bits: 1 COARSE (one octave, no relief); 2 BTBH_DEBUG_COVER (c shown in place of the colour).
static const uint btbh_cover_flag_coarse = 1u;
static const uint btbh_cover_flag_debug = 2u;
// The second octave: its frequency over the first's and its offset (so the two don't line up).
static const float btbh_cover_octave_scale = 2.03f;
static const float btbh_cover_octave_offset = 17.31f;
// Each octave's share of N as a box of the same variance: value noise over independent uniform corners, smoothstep
// interpolated, has a standard deviation of (26/35) / sqrt(12); times 4/3 (and 1/2 for the second octave) and sqrt(3)
// for the box's half-width, 52/105 and 26/105.
static const float btbh_cover_octave1_spread = 52.0f / 105.0f;
static const float btbh_cover_octave2_spread = 26.0f / 105.0f;

// The integer hash of production_surface.hlsli's btbh_detail_hash, whole: 32 bits (still ice's seeds take two 16-bit
// halves of one).
uint btbh_cover_hash(float2 cell)
{
    const int2 integer_cell = (int2)cell;
    uint hash = (uint)integer_cell.x * 0x8da6b343u ^ (uint)integer_cell.y * 0xd8163841u;
    hash ^= hash >> 15;
    hash *= 0x2c1b3c6du;
    hash ^= hash >> 12;
    hash *= 0x297a2d39u;
    hash ^= hash >> 15;
    return hash;
}

float btbh_cover_hash01(float2 cell)
{
    return (float)(btbh_cover_hash(cell) & 0x00ffffffu) * (1.0f / 16777216.0f);
}

// Value noise in 0..1 with its gradient (per noise cell), smoothstep-interpolated between the corners' hashes.
float btbh_cover_value_noise(float2 value, out float2 gradient)
{
    const float2 cell = floor(value);
    const float2 f = value - cell;
    const float2 u = f * f * (3.0f - 2.0f * f);
    const float2 du = 6.0f * f * (1.0f - f);
    const float a = btbh_cover_hash01(cell);
    const float b = btbh_cover_hash01(cell + float2(1.0f, 0.0f));
    const float c = btbh_cover_hash01(cell + float2(0.0f, 1.0f));
    const float d = btbh_cover_hash01(cell + float2(1.0f, 1.0f));
    const float k1 = b - a;
    const float k2 = c - a;
    const float k3 = a - b - c + d;
    gradient = du * float2(k1 + k3 * u.y, k2 + k3 * u.x);
    return a + k1 * u.x + k2 * u.y + k3 * u.x * u.y;
}

// The cover's noise N in -1..1 over the flat world (x, z): two octaves (the second at 2.03x the frequency, half as
// strong), each fading out once its cells shrink under about two pixels (footprint: the first octave's cells per
// pixel), scaled by 4/3 so the sum spans -1..1. COARSE: the first octave alone. gradient1 and gradient2 are each
// octave's gradient (per its own cell, already faded) for the relief. spread: the half-width of a box with the
// variance the fading took out (what a pixel averages over there; 0 where nothing has faded), at most 1 - |N| so that
// N + spread stays within the reach.
float btbh_cover_noise(
    float2 position,
    float footprint,
    bool coarse,
    out float2 gradient1,
    out float2 gradient2,
    out float spread
)
{
    float2 g1;
    const float n1 = btbh_cover_value_noise(position, g1);
    const float fade1 = saturate((0.6f - footprint) * 2.5f);
    gradient1 = g1 * fade1;
    float sum = (n1 - 0.5f) * fade1;
    float lost = (1.0f - fade1 * fade1) * (btbh_cover_octave1_spread * btbh_cover_octave1_spread);
    gradient2 = float2(0.0f, 0.0f);
    if (!coarse)
    {
        float2 g2;
        const float n2 = btbh_cover_value_noise(
            position * btbh_cover_octave_scale + btbh_cover_octave_offset,
            g2
        );
        const float fade2 = saturate((0.6f - footprint * btbh_cover_octave_scale) * 2.5f);
        gradient2 = g2 * fade2;
        sum += 0.5f * (n2 - 0.5f) * fade2;
        lost += (1.0f - fade2 * fade2) * (btbh_cover_octave2_spread * btbh_cover_octave2_spread);
    }
    const float noise = sum * (4.0f / 3.0f);
    spread = min(sqrt(lost), 1.0f - abs(noise));
    return noise;
}

// A ramp from lo to hi (smoothed: smoothstep; else linear) averaged over a box of half-width `spread` about x: the
// ramp's integral, within lo..hi and past hi, at the box's two ends. spread under 0.001: the ramp itself.
float btbh_cover_ramp_integral(float lo, float hi, bool smoothed, float x)
{
    const float t = saturate((x - lo) / (hi - lo));
    return (hi - lo) * (smoothed ? t * t * t * (1.0f - 0.5f * t) : 0.5f * t * t) + max(x - hi, 0.0f);
}

float btbh_cover_filtered_ramp(float lo, float hi, bool smoothed, float x, float spread)
{
    if (spread < 1.0e-3f)
    {
        return smoothed ? smoothstep(lo, hi, x) : saturate((x - lo) / (hi - lo));
    }
    if (x + spread <= lo)
    {
        return 0.0f;
    }
    if (x - spread >= hi)
    {
        return 1.0f;
    }
    return (btbh_cover_ramp_integral(lo, hi, smoothed, x + spread) -
        btbh_cover_ramp_integral(lo, hi, smoothed, x - spread)) / (2.0f * spread);
}

// The pattern's edge at x, how far the face is inside the reach in edge widths (saturated, the raw coverage c),
// averaged over a box of half-width `spread` (the same units): where the noise is finer than a pixel, the coverage it
// averages to there.
float btbh_cover_edge(uint pattern, float x, float spread)
{
    if (pattern == btbh_cover_pattern_snow)
    {
        return btbh_cover_filtered_ramp(0.3f, 0.7f, true, x, spread);
    }
    if (pattern == btbh_cover_pattern_mud)
    {
        return btbh_cover_filtered_ramp(0.15f, 0.55f, true, x, spread);
    }
    return btbh_cover_filtered_ramp(0.0f, 1.0f, false, x, spread);
}

// MUD's puddles at N (spread: as for the edge, in N): where N is low, averaged where it is finer than a pixel.
float btbh_cover_puddle(float noise, float spread)
{
    return 1.0f - btbh_cover_filtered_ramp(-0.4f, 0.2f, true, noise, spread);
}

float btbh_cover_grain(uint pattern)
{
    return pattern == btbh_cover_pattern_snow ? 0.04f : (pattern == btbh_cover_pattern_sand ? 0.10f : 0.15f);
}

// ---------------------------------------------------------------------------------------------------- still ice

// A Voronoi cell's seed (jitter 0.85, the integer hash's two 16-bit halves).
float2 btbh_ice_seed(float2 cell)
{
    const uint hash = btbh_cover_hash(cell);
    const float2 jitter = float2(
        (float)(hash & 0xffffu) * (1.0f / 65536.0f),
        (float)(hash >> 16) * (1.0f / 65536.0f)
    );
    return cell + 0.5f + (jitter - 0.5f) * 0.85f;
}

// One Voronoi grid over p (in cells): the distance from p to the nearest seed's cell's border (cells), the direction
// across that border (from the nearest seed toward the neighbour it parts it from), and the nearest seed's cell. Two
// passes: the nearest seed over the 3 x 3 cells round p, then the nearest of its borders, with the seeds of the 4 x 4
// cells from its cell toward p (each border's distance along the line between the two seeds), so the distance doesn't
// jump near a corner where three plates meet.
float btbh_ice_voronoi(float2 p, out float2 across, out float2 nearest_cell)
{
    const float2 base = floor(p);
    float best = 1.0e9f;
    float2 best_seed = float2(0.0f, 0.0f);
    nearest_cell = base;
    [unroll]
    for (int y = -1; y <= 1; ++y)
    {
        [unroll]
        for (int x = -1; x <= 1; ++x)
        {
            const float2 cell = base + float2((float)x, (float)y);
            const float2 seed = btbh_ice_seed(cell);
            const float2 delta = seed - p;
            const float distance2 = dot(delta, delta);
            if (distance2 < best)
            {
                best = distance2;
                best_seed = seed;
                nearest_cell = cell;
            }
        }
    }
    const float2 toward = float2(p.x >= best_seed.x ? 1.0f : -1.0f, p.y >= best_seed.y ? 1.0f : -1.0f);
    float border = 1.0e9f;
    across = float2(1.0f, 0.0f);
    [unroll]
    for (int y = -1; y <= 2; ++y)
    {
        [unroll]
        for (int x = -1; x <= 2; ++x)
        {
            if (x != 0 || y != 0)
            {
                const float2 seed = btbh_ice_seed(nearest_cell + float2((float)x, (float)y) * toward);
                const float2 link = seed - best_seed;
                const float2 direction = link / max(length(link), 1.0e-6f);
                const float distance = dot(0.5f * (best_seed + seed) - p, direction);
                if (distance < border)
                {
                    border = distance;
                    across = direction;
                }
            }
        }
    }
    return max(border, 0.0f);
}

// A crack of half-width `width` (world units) at `distance` from its line, with a pixel `footprint` (world units): it
// widens to half a pixel and loses strength in step, so a crack thinner than a pixel fades instead of aliasing.
float btbh_ice_crack(float distance, float width, float footprint)
{
    const float wide = max(width, 0.5f * footprint);
    return (1.0f - smoothstep(0.0f, wide, distance)) * (width / max(wide, 1.0e-6f));
}

struct BTBHIce
{
    float lift_share;     // 0..1: how far the albedo moves toward its lifted colour (the cracks and the frost)
    float roughness;
    float2 gradient;      // the surface gradient (world x, z) the plate's tilt and the cracks' bevels add
};

// Still ice at the flat world (x, z), a pixel `footprint` world units wide. p0 and p1: surface_parameters0 and 1 with
// their zero values resolved (production_model.frag's apply_surface passes them through btbh_ice_defaults).
BTBHIce btbh_ice(float2 position, float footprint, float4 p0, float4 p1)
{
    const float plate = p0.x;
    const float width = p0.y;
    const float tilt = p0.w;
    BTBHIce ice;
    float2 across;
    float2 cell;
    const float distance = btbh_ice_voronoi(position / plate, across, cell) * plate;
    const float crack = btbh_ice_crack(distance, width, footprint);
    // The plate's own tilt, from its cell's hash (another stream than its seed's).
    const uint plate_hash = btbh_cover_hash(cell + float2(113.0f, 71.0f));
    const float2 plate_tilt = tilt * (float2(
        (float)(plate_hash & 0xffffu) * (1.0f / 65536.0f),
        (float)(plate_hash >> 16) * (1.0f / 65536.0f)
    ) * 2.0f - 1.0f);
    // The bevel: within twice the crack's width the surface leans up to 0.35 toward the crack's line (a V groove),
    // faded as the crack is.
    const float bevel = 0.35f * (1.0f - saturate(distance / (2.0f * max(width, 0.5f * footprint)))) *
        (width / max(width, 0.5f * footprint));
    ice.gradient = plate_tilt - bevel * across;
    float share = crack;
    // The hairline craze: a second grid at plate / 2.6, half the width, 0.6 as strong; it fades out as a pixel grows
    // from its width to twice it, and is skipped past that (no derivative is taken in it).
    const float fine_width = 0.5f * width;
    const float fine_fade = saturate(2.0f - footprint / fine_width);
    [branch]
    if (fine_fade > 0.0f)
    {
        float2 fine_across;
        float2 fine_cell;
        const float fine_plate = plate / 2.6f;
        const float fine_distance =
            btbh_ice_voronoi(position / fine_plate + float2(7.1f, 3.3f), fine_across, fine_cell) * fine_plate;
        share = max(share, 0.6f * fine_fade * btbh_ice_crack(fine_distance, fine_width, footprint));
    }
    // Frost in patches: two octaves of value noise over (x, z) / its patch size, its share of the surface p1.x.
    const float frost = p1.x;
    if (frost > 0.0f)
    {
        float2 g1;
        float2 g2;
        const float2 frost_position = position / p1.y;
        const float n1 = btbh_cover_value_noise(frost_position, g1);
        const float n2 = btbh_cover_value_noise(frost_position * 2.03f + 5.7f, g2);
        const float noise = n1 * (2.0f / 3.0f) + n2 * (1.0f / 3.0f);
        share = max(share, smoothstep(1.0f - frost - 0.1f, 1.0f - frost + 0.1f, noise));
    }
    ice.lift_share = share;
    ice.roughness = lerp(p1.z, p1.w, share);
    return ice;
}

// surface_parameters0 and 1 with their zero values resolved (BTBH_GPU_SCENE_SURFACE_ICE's defaults).
void btbh_ice_defaults(inout float4 p0, inout float4 p1)
{
    p0.x = p0.x > 0.0f ? p0.x : 1.2f;
    p0.y = p0.y > 0.0f ? p0.y : 0.025f;
    p0.z = p0.z > 0.0f ? p0.z : 0.5f;
    p0.w = p0.w > 0.0f ? p0.w : 0.035f;
    p1.y = p1.y > 0.0f ? p1.y : 3.0f;
    p1.z = p1.z > 0.0f ? p1.z : 0.06f;
    p1.w = p1.w > 0.0f ? p1.w : 0.6f;
}

#endif
