// AMD FidelityFX Super Resolution 1.0, Edge Adaptive Spatial Upsampling (EASU): a port of FsrEasuF from
// ffx_fsr1.h (MIT licence, Copyright (c) 2021 Advanced Micro Devices, Inc.). Twelve taps around the output
// pixel; a Lanczos-2 kernel stretched along the local edge direction, clamped to the nearest four texels.
Texture2D<float4> source_texture : register(t0, space2);
SamplerState source_sampler : register(s0, space2);

cbuffer UpscaleUniforms : register(b0, space3)
{
    // 1 / source width, 1 / source height, source width, source height.
    float4 source_metrics;
};

struct FragmentInput
{
    float4 position : SV_Position;
    float2 texture_coordinate : TEXCOORD0;
};

float3 load_source(float2 texel)
{
    const int2 limit = int2(source_metrics.zw) - 1;
    return source_texture.Load(int3(clamp(int2(texel), int2(0, 0), limit), 0)).rgb;
}

// Luma times two, as EASU uses it.
float easu_luma(float3 color)
{
    return color.b * 0.5f + (color.r * 0.5f + color.g);
}

// One bilinear quadrant's share of the edge direction and length: center c, neighbours a (up), b (left),
// d (right), e (down), weighted by how close the output pixel sits to that quadrant's texel.
void easu_set(inout float2 direction, inout float edge_length, float weight,
              float la, float lb, float lc, float ld, float le)
{
    const float dc = ld - lc;
    const float cb = lc - lb;
    float length_x = max(abs(dc), abs(cb));
    length_x = length_x > 0.0f ? 1.0f / length_x : 0.0f;
    const float direction_x = ld - lb;
    direction.x += direction_x * weight;
    length_x = saturate(abs(direction_x) * length_x);
    length_x *= length_x;
    edge_length += length_x * weight;

    const float ec = le - lc;
    const float ca = lc - la;
    float length_y = max(abs(ec), abs(ca));
    length_y = length_y > 0.0f ? 1.0f / length_y : 0.0f;
    const float direction_y = le - la;
    direction.y += direction_y * weight;
    length_y = saturate(abs(direction_y) * length_y);
    length_y *= length_y;
    edge_length += length_y * weight;
}

void easu_tap(inout float3 accumulated, inout float accumulated_weight, float2 offset, float2 direction,
              float2 stretch_length, float lobe, float clip, float3 color)
{
    float2 rotated;
    rotated.x = offset.x * direction.x + offset.y * direction.y;
    rotated.y = offset.x * -direction.y + offset.y * direction.x;
    rotated *= stretch_length;
    float distance_squared = min(dot(rotated, rotated), clip);
    float window = 2.0f / 5.0f * distance_squared - 1.0f;
    float base = lobe * distance_squared - 1.0f;
    window *= window;
    base *= base;
    window = 25.0f / 16.0f * window - (25.0f / 16.0f - 1.0f);
    const float weight = window * base;
    accumulated += color * weight;
    accumulated_weight += weight;
}

float4 main(FragmentInput input) : SV_Target0
{
    // The output pixel's position in source texels, relative to texel centres.
    const float2 position = input.texture_coordinate * source_metrics.zw - 0.5f;
    const float2 origin = floor(position);
    const float2 fraction = position - origin;

    //    b c
    //  e f g h
    //  i j k l
    //    n o
    const float3 b = load_source(origin + float2(0.0f, -1.0f));
    const float3 c = load_source(origin + float2(1.0f, -1.0f));
    const float3 e = load_source(origin + float2(-1.0f, 0.0f));
    const float3 f = load_source(origin);
    const float3 g = load_source(origin + float2(1.0f, 0.0f));
    const float3 h = load_source(origin + float2(2.0f, 0.0f));
    const float3 i = load_source(origin + float2(-1.0f, 1.0f));
    const float3 j = load_source(origin + float2(0.0f, 1.0f));
    const float3 k = load_source(origin + float2(1.0f, 1.0f));
    const float3 l = load_source(origin + float2(2.0f, 1.0f));
    const float3 n = load_source(origin + float2(0.0f, 2.0f));
    const float3 o = load_source(origin + float2(1.0f, 2.0f));

    const float bl = easu_luma(b), cl = easu_luma(c), el = easu_luma(e), fl = easu_luma(f);
    const float gl = easu_luma(g), hl = easu_luma(h), il = easu_luma(i), jl = easu_luma(j);
    const float kl = easu_luma(k), ll = easu_luma(l), nl = easu_luma(n), ol = easu_luma(o);

    float2 direction = float2(0.0f, 0.0f);
    float edge_length = 0.0f;
    easu_set(direction, edge_length, (1.0f - fraction.x) * (1.0f - fraction.y), bl, el, fl, gl, jl);
    easu_set(direction, edge_length, fraction.x * (1.0f - fraction.y), cl, fl, gl, hl, kl);
    easu_set(direction, edge_length, (1.0f - fraction.x) * fraction.y, fl, il, jl, kl, nl);
    easu_set(direction, edge_length, fraction.x * fraction.y, gl, jl, kl, ll, ol);

    // Normalise the direction; a flat area keeps the unrotated kernel.
    const float direction_squared = dot(direction, direction);
    const bool flat = direction_squared < 1.0f / 32768.0f;
    direction = flat ? float2(1.0f, 0.0f) : direction * rsqrt(direction_squared);

    // Edge length 0 (no edge) .. 1 (strong edge), shaped; it stretches the kernel along the edge and sharpens it.
    edge_length = edge_length * 0.5f;
    edge_length *= edge_length;
    const float stretch = dot(direction, direction) / max(abs(direction.x), abs(direction.y));
    const float2 stretch_length = float2(1.0f + (stretch - 1.0f) * edge_length, 1.0f - 0.5f * edge_length);
    const float lobe = 0.5f + ((1.0f / 4.0f - 0.04f) - 0.5f) * edge_length;
    const float clip = 1.0f / lobe;

    float3 accumulated = float3(0.0f, 0.0f, 0.0f);
    float accumulated_weight = 0.0f;
    easu_tap(accumulated, accumulated_weight, float2(0.0f, -1.0f) - fraction, direction, stretch_length, lobe, clip, b);
    easu_tap(accumulated, accumulated_weight, float2(1.0f, -1.0f) - fraction, direction, stretch_length, lobe, clip, c);
    easu_tap(accumulated, accumulated_weight, float2(-1.0f, 1.0f) - fraction, direction, stretch_length, lobe, clip, i);
    easu_tap(accumulated, accumulated_weight, float2(0.0f, 1.0f) - fraction, direction, stretch_length, lobe, clip, j);
    easu_tap(accumulated, accumulated_weight, float2(0.0f, 0.0f) - fraction, direction, stretch_length, lobe, clip, f);
    easu_tap(accumulated, accumulated_weight, float2(-1.0f, 0.0f) - fraction, direction, stretch_length, lobe, clip, e);
    easu_tap(accumulated, accumulated_weight, float2(1.0f, 1.0f) - fraction, direction, stretch_length, lobe, clip, k);
    easu_tap(accumulated, accumulated_weight, float2(2.0f, 1.0f) - fraction, direction, stretch_length, lobe, clip, l);
    easu_tap(accumulated, accumulated_weight, float2(2.0f, 0.0f) - fraction, direction, stretch_length, lobe, clip, h);
    easu_tap(accumulated, accumulated_weight, float2(1.0f, 0.0f) - fraction, direction, stretch_length, lobe, clip, g);
    easu_tap(accumulated, accumulated_weight, float2(1.0f, 2.0f) - fraction, direction, stretch_length, lobe, clip, o);
    easu_tap(accumulated, accumulated_weight, float2(0.0f, 2.0f) - fraction, direction, stretch_length, lobe, clip, n);

    // Deringing: no brighter or darker than the four texels around the output pixel.
    const float3 minimum = min(min(f, g), min(j, k));
    const float3 maximum = max(max(f, g), max(j, k));
    const float3 color = accumulated / max(accumulated_weight, 1e-5f);
    return float4(clamp(color, minimum, maximum), 1.0f);
}
