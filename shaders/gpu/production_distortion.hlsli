// RFC 0018 (screen-space distortion, since 0.18.0): one source's displacement at a point of its plane, as the CPU
// reference has it (src/gpu/btbh_gpu_distortion.h: btbh_distortion_displacement and its noise). Shared by
// production_distortion.frag.hlsl.
#ifndef BTBH_PRODUCTION_DISTORTION_HLSLI
#define BTBH_PRODUCTION_DISTORTION_HLSLI

#define DISTORTION_RING 0u
#define DISTORTION_DISC 1u
#define DISTORTION_STRIP 2u
#define DISTORTION_PUSH 0u
#define DISTORTION_DISC_PEAK 3.4938562f

// The haze's lattice: an integer hash of a cell (its coordinates' low 8 bits, so it repeats every 256 cells) and a seed.
uint distortion_hash(int x, int y, uint seed)
{
    uint h = (asuint(x) & 255u) * 0x8DA6B343u ^ (asuint(y) & 255u) * 0xD8163841u ^ seed * 0xCB1AB31Fu;
    h ^= h >> 16;
    h *= 0x7FEB352Du;
    h ^= h >> 15;
    h *= 0x846CA68Bu;
    h ^= h >> 16;
    return h;
}

float distortion_lattice(int x, int y, uint seed)
{
    return (float)(distortion_hash(x, y, seed) & 0xFFFFu) * (2.0f / 65535.0f) - 1.0f;
}

float distortion_value_noise(float2 p, uint seed)
{
    const float2 f = floor(p);
    const int ix = (int)f.x;
    const int iy = (int)f.y;
    const float2 t = p - f;
    const float2 w = t * t * (3.0f - 2.0f * t);
    const float a = distortion_lattice(ix, iy, seed);
    const float b = distortion_lattice(ix + 1, iy, seed);
    const float c = distortion_lattice(ix, iy + 1, seed);
    const float d = distortion_lattice(ix + 1, iy + 1, seed);
    const float top = a + (b - a) * w.x;
    const float bottom = c + (d - c) * w.x;
    return top + (bottom - top) * w.y;
}

// The first octave drifts `drift` cells along the shape's v (a strip: its u); the second, at twice the frequency and
// turned by half a radian, three cells for each of the first's.
float2 distortion_haze_noise(float2 h, float drift, bool along_u)
{
    const float c = 0.87758256f;
    const float s = 0.47942554f;
    const float2 p1 = along_u ? float2(h.x - drift, h.y) : float2(h.x, h.y - drift);
    const float2 r = 2.0f * float2(c * h.x - s * h.y, s * h.x + c * h.y);
    const float2 p2 = along_u ? float2(r.x - 3.0f * drift, r.y) : float2(r.x, r.y - 3.0f * drift);
    return float2(
        (distortion_value_noise(p1, 0u) + 0.5f * distortion_value_noise(p2, 1u)) / 1.5f,
        (distortion_value_noise(p1, 2u) + 0.5f * distortion_value_noise(p2, 3u)) / 1.5f
    );
}

// shape: radius, span (a ring's width, a strip's length), strength (render pixels), kind (shape + 3 * mode);
// extra: chroma, haze scale, haze drift. local: the point (world units along the shape's axes); dl_dx, dl_dy: its
// change across one field pixel. Returns the displacement in render pixels (x right, y down), its length times chroma
// and its length.
float4 distortion_displacement(float4 shape, float4 extra, float2 local, float2 dl_dx, float2 dl_dy)
{
    const uint kind = (uint)(shape.w + 0.5f);
    const uint form = kind % 3u;
    const uint mode = kind / 3u;
    float2 q = local;
    if (form == DISTORTION_STRIP)
    {
        q.x -= clamp(local.x, 0.0f, shape.y);
    }
    const float r = sqrt(q.x * q.x + q.y * q.y);
    float push = 0.0f;
    float envelope = 0.0f;
    if (form == DISTORTION_RING)
    {
        const float half_width = 0.5f * shape.y;
        const float x = (r - shape.x) / half_width;
        if (!(abs(x) < 1.0f))
        {
            return float4(0.0f, 0.0f, 0.0f, 0.0f);
        }
        const float band = (1.0f - x * x) * (1.0f - x * x);
        push = band * min(r / half_width, 1.0f);
        envelope = push;
    }
    else
    {
        const float u = r / shape.x;
        if (!(u < 1.0f))
        {
            return float4(0.0f, 0.0f, 0.0f, 0.0f);
        }
        const float edge = (1.0f - u * u) * (1.0f - u * u);
        push = DISTORTION_DISC_PEAK * u * edge;
        envelope = edge;
    }
    float2 d;
    if (mode == DISTORTION_PUSH)
    {
        if (!(r > 1.0e-12f))
        {
            return float4(0.0f, 0.0f, 0.0f, 0.0f);
        }
        const float2 g = float2(dot(q, dl_dx), dot(q, dl_dy)) / r;
        const float g_length = sqrt(g.x * g.x + g.y * g.y);
        if (!(g_length > 1.0e-20f))
        {
            return float4(0.0f, 0.0f, 0.0f, 0.0f);
        }
        d = g * (shape.z * push / g_length);
    }
    else
    {
        const float2 noise = distortion_haze_noise(local * extra.y, extra.z, form == DISTORTION_STRIP);
        const float magnitude = shape.z * envelope;
        d = float2(magnitude * noise.x, -magnitude * noise.y);
    }
    const float d_length = sqrt(d.x * d.x + d.y * d.y);
    return float4(d, extra.x * d_length, d_length);
}

#endif
