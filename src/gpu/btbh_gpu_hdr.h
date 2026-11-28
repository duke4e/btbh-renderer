#ifndef BTBH_GPU_HDR_H
#define BTBH_GPU_HDR_H

/* HDR output's curves on the CPU (RFC 0017, since 0.17.0): the reference the shaders (production_hdr.hlsli,
   production_hdr_encode.frag, production_hdr_fold.frag) follow, used by the renderer (the knee, the encoded clear
   colour, the read-backs) and by the tests. Header only, no SDL. */

#include <math.h>
#include <stdint.h>
#include <string.h>

/* The display's decode of sRGB-encoded SDR values: what a typical SDR TV or monitor does (RFC 0017, decision 6). */
#define BTBH_HDR_DISPLAY_GAMMA 2.2f
/* The knee: where the bare SDR curve (on grey, exposure 1, no grading) reaches this output. PBR Neutral's is its own
   shoulder start (production_tonemap.hlsli: 0.8 - 0.04). */
#define BTBH_HDR_KNEE_OUTPUT 0.6f
#define BTBH_HDR_KNEE_OUTPUT_PBR_NEUTRAL 0.76f
/* The shoulder's width over the knee: tau = BTBH_HDR_SHOULDER_SCALE * m_k. */
#define BTBH_HDR_SHOULDER_SCALE 4.0f
/* The fold of an HDR capture into an SDR target: display light above it is compressed under 1. */
#define BTBH_HDR_FOLD_KNEE 0.6f
/* BT.2100 PQ. */
#define BTBH_HDR_PQ_M1 0.1593017578125f
#define BTBH_HDR_PQ_M2 78.84375f
#define BTBH_HDR_PQ_C1 0.8359375f
#define BTBH_HDR_PQ_C2 18.8515625f
#define BTBH_HDR_PQ_C3 18.6875f
/* scRGB's 1.0. */
#define BTBH_HDR_SCRGB_NITS 80.0f

/* The tone mappers' numbers (BTBHGPUScenePost.tonemap_mode). */
#define BTBH_HDR_TONEMAP_REINHARD 0u
#define BTBH_HDR_TONEMAP_PBR_NEUTRAL 1u
#define BTBH_HDR_TONEMAP_ACES 2u

static inline float btbh_hdr_saturate(float value)
{
    return value < 0.0f ? 0.0f : (value > 1.0f ? 1.0f : value);
}

/* The bare SDR curve on grey x (exposure applied, no grading), as production_tonemap.hlsli computes it, saturated as
   btbh_apply_color_adjustment saturates it. */
static inline float btbh_hdr_tone_curve(uint32_t tonemap_mode, float white_point, float x)
{
    x = x > 0.0f ? x : 0.0f;
    if (tonemap_mode == BTBH_HDR_TONEMAP_PBR_NEUTRAL) {
        const float start_compression = 0.8f - 0.04f;
        const float offset = x < 0.08f ? x - 6.25f * x * x : 0.04f;
        const float peak = x - offset;
        if (peak < start_compression) {
            return btbh_hdr_saturate(peak);
        }
        const float shoulder = 1.0f - start_compression;
        return btbh_hdr_saturate(1.0f - shoulder * shoulder / (peak + shoulder - start_compression));
    }
    if (tonemap_mode == BTBH_HDR_TONEMAP_ACES) {
        return btbh_hdr_saturate((x * (2.51f * x + 0.03f)) / (x * (2.43f * x + 0.59f) + 0.14f));
    }
    const float white_squared = white_point * white_point;
    if (!(white_squared > 0.0f)) {
        return btbh_hdr_saturate(x / (1.0f + x));
    }
    return btbh_hdr_saturate((white_squared * x + x * x) / (white_squared * x + white_squared));
}

/* The knee's output k for a tone mapper (knee_override > 0 replaces it: BTBH_GPU_HDR_KNEE). */
static inline float btbh_hdr_knee_output(uint32_t tonemap_mode, float knee_override)
{
    if (knee_override > 0.0f && knee_override < 1.0f) {
        return knee_override;
    }
    return tonemap_mode == BTBH_HDR_TONEMAP_PBR_NEUTRAL ? BTBH_HDR_KNEE_OUTPUT_PBR_NEUTRAL : BTBH_HDR_KNEE_OUTPUT;
}

/* m_k: the scene value (exposed, grey) where the bare SDR curve reaches k, by bisection. */
static inline float btbh_hdr_knee_input(uint32_t tonemap_mode, float white_point, float knee_output)
{
    float low = 0.0f;
    float high = 1024.0f;
    if (btbh_hdr_tone_curve(tonemap_mode, white_point, high) < knee_output) {
        return high;
    }
    for (int step = 0; step < 64; ++step) {
        const float middle = 0.5f * (low + high);
        if (btbh_hdr_tone_curve(tonemap_mode, white_point, middle) < knee_output) {
            low = middle;
        } else {
            high = middle;
        }
    }
    return high;
}

/* The highlight gain: 1 at and below the knee m_k (and C1 there), rising monotonically toward ratio (peak over paper
   white). heat: exposure * the scene's largest channel. */
static inline float btbh_hdr_gain(float heat, float ratio, float knee_input, float shoulder)
{
    const float over = heat - knee_input;
    const float t = over > 0.0f ? over / (shoulder > 1.0e-6f ? shoulder : 1.0e-6f) : 0.0f;
    const float t2 = t * t;
    return 1.0f + (ratio - 1.0f) * (t2 / (1.0f + t2));
}

/* The SDR output pass's encoding (production_post.frag.hlsl's linear_to_srgb: a pure 1/2.4 power, no linear toe). */
static inline float btbh_hdr_linear_to_srgb(float value)
{
    const float encoded = 1.055f * powf(value > 0.0f ? value : 0.0f, 0.416666667f) - 0.055f;
    return encoded > 0.0f ? encoded : 0.0f;
}

/* An encoded value in display light: gamma 2.2, extended above 1. */
static inline float btbh_hdr_display_light(float encoded)
{
    return powf(encoded > 0.0f ? encoded : 0.0f, BTBH_HDR_DISPLAY_GAMMA);
}

static inline float btbh_hdr_display_encode(float light)
{
    return powf(light > 0.0f ? light : 0.0f, 1.0f / BTBH_HDR_DISPLAY_GAMMA);
}

/* PQ (BT.2100) of a luminance as a fraction of 10000 nits, and its inverse. */
static inline float btbh_hdr_pq_encode(float y)
{
    y = y > 0.0f ? (y < 1.0f ? y : 1.0f) : 0.0f;
    const float p = powf(y, BTBH_HDR_PQ_M1);
    return powf((BTBH_HDR_PQ_C1 + BTBH_HDR_PQ_C2 * p) / (1.0f + BTBH_HDR_PQ_C3 * p), BTBH_HDR_PQ_M2);
}

static inline float btbh_hdr_pq_decode(float e)
{
    e = e > 0.0f ? (e < 1.0f ? e : 1.0f) : 0.0f;
    const float p = powf(e, 1.0f / BTBH_HDR_PQ_M2);
    const float numerator = p - BTBH_HDR_PQ_C1;
    const float denominator = BTBH_HDR_PQ_C2 - BTBH_HDR_PQ_C3 * p;
    return powf((numerator > 0.0f ? numerator : 0.0f) / denominator, 1.0f / BTBH_HDR_PQ_M1);
}

/* BT.709 to BT.2020 primaries (linear light), rows. */
static inline void btbh_hdr_709_to_2020(const float rgb[3], float out[3])
{
    out[0] = 0.6274040f * rgb[0] + 0.3292820f * rgb[1] + 0.0433136f * rgb[2];
    out[1] = 0.0690970f * rgb[0] + 0.9195400f * rgb[1] + 0.0113612f * rgb[2];
    out[2] = 0.0163916f * rgb[0] + 0.0880132f * rgb[1] + 0.8955950f * rgb[2];
}

/* What the encode pass writes for a frame value v (sRGB-encoded, 1.0 = paper white), without its dither.
   composition: 2 HDR10 (PQ, BT.2020), 3 extended linear. linear_scale: paper white / 80 (Windows, gamescope), 1 on
   Apple. ratio: the clamp at the peak, over paper white. */
static inline void btbh_hdr_encode_pixel(
    const float v[3],
    uint32_t composition,
    float paper_white_nits,
    float ratio,
    float linear_scale,
    float out[3]
)
{
    float light[3];
    float largest = 0.0f;
    for (int channel = 0; channel < 3; ++channel) {
        light[channel] = btbh_hdr_display_light(v[channel]);
        largest = light[channel] > largest ? light[channel] : largest;
    }
    if (largest > ratio && largest > 0.0f) {
        const float scale = ratio / largest;
        for (int channel = 0; channel < 3; ++channel) {
            light[channel] *= scale;
        }
    }
    if (composition == 2u) {
        float nits[3];
        float wide[3];
        for (int channel = 0; channel < 3; ++channel) {
            nits[channel] = light[channel] * paper_white_nits;
        }
        btbh_hdr_709_to_2020(nits, wide);
        for (int channel = 0; channel < 3; ++channel) {
            out[channel] = btbh_hdr_pq_encode(wide[channel] / 10000.0f);
        }
        return;
    }
    for (int channel = 0; channel < 3; ++channel) {
        out[channel] = light[channel] * linear_scale;
    }
}

/* The fold of an HDR value into SDR (production_hdr_fold.frag): in display light, the largest channel M above the
   knee k is brought into (k, 1]: F(M) = k + (M - k) / (1 + (P - 1)(M - k) / ((1 - k)(P - k))), the hue kept, then
   encoded back. F is C1 at k, maps the peak P to 1, and is the identity when P is 1 (an HDR frame at peak = paper white
   is the SDR picture, copied as it is). Light above P (sharpening's overshoot) lands just above 1 and clamps. */
static inline float btbh_hdr_fold_largest(float largest, float ratio)
{
    const float k = BTBH_HDR_FOLD_KNEE;
    if (largest <= k || ratio <= 1.0f) {
        return largest;
    }
    const float over = largest - k;
    return k + over / (1.0f + (ratio - 1.0f) * over / ((1.0f - k) * (ratio - k)));
}

static inline void btbh_hdr_fold_pixel(const float v[3], float ratio, float out[3])
{
    float light[3];
    float largest = 0.0f;
    for (int channel = 0; channel < 3; ++channel) {
        light[channel] = btbh_hdr_display_light(v[channel]);
        largest = light[channel] > largest ? light[channel] : largest;
    }
    if (largest <= BTBH_HDR_FOLD_KNEE || ratio <= 1.0f) {
        /* The value as it is (the shader returns it untouched). */
        for (int channel = 0; channel < 3; ++channel) {
            out[channel] = v[channel];
        }
        return;
    }
    const float scale = btbh_hdr_fold_largest(largest, ratio) / largest;
    for (int channel = 0; channel < 3; ++channel) {
        out[channel] = btbh_hdr_display_encode(light[channel] * scale);
    }
}

/* IEEE half to float and back (round to nearest even), for the read-backs and the tests. */
static inline float btbh_hdr_half_to_float(uint16_t half)
{
    const uint32_t sign = (uint32_t)(half & 0x8000u) << 16;
    const uint32_t exponent = (half >> 10) & 0x1fu;
    const uint32_t mantissa = half & 0x3ffu;
    uint32_t bits;
    if (exponent == 0u) {
        if (mantissa == 0u) {
            bits = sign;
        } else {
            const float value = (float)mantissa * (1.0f / 16777216.0f);
            return sign != 0u ? -value : value;
        }
    } else if (exponent == 31u) {
        bits = sign | 0x7f800000u | (mantissa << 13);
    } else {
        bits = sign | ((exponent + 112u) << 23) | (mantissa << 13);
    }
    float result;
    memcpy(&result, &bits, sizeof(result));
    return result;
}

static inline uint16_t btbh_hdr_float_to_half(float value)
{
    uint32_t bits;
    memcpy(&bits, &value, sizeof(bits));
    const uint16_t sign = (uint16_t)((bits >> 16) & 0x8000u);
    const uint32_t exponent = (bits >> 23) & 0xffu;
    uint32_t mantissa = bits & 0x7fffffu;
    if (exponent == 0xffu) {
        return (uint16_t)(sign | 0x7c00u | (mantissa != 0u ? 0x200u : 0u));
    }
    const int32_t half_exponent = (int32_t)exponent - 127 + 15;
    if (half_exponent >= 31) {
        return (uint16_t)(sign | 0x7c00u);
    }
    if (half_exponent <= 0) {
        if (half_exponent < -10) {
            return sign;
        }
        mantissa |= 0x800000u;
        const uint32_t shift = (uint32_t)(14 - half_exponent);
        uint32_t half_mantissa = mantissa >> shift;
        const uint32_t remainder = mantissa & ((1u << shift) - 1u);
        const uint32_t halfway = 1u << (shift - 1u);
        if (remainder > halfway || (remainder == halfway && (half_mantissa & 1u) != 0u)) {
            half_mantissa += 1u;
        }
        return (uint16_t)(sign | half_mantissa);
    }
    uint32_t half = ((uint32_t)half_exponent << 10) | (mantissa >> 13);
    const uint32_t remainder = mantissa & 0x1fffu;
    if (remainder > 0x1000u || (remainder == 0x1000u && (half & 1u) != 0u)) {
        half += 1u;
    }
    return (uint16_t)(sign | half);
}

/* An HDR frame value to the RGBA8 read-back: clamped to 0-1 and rounded. */
static inline uint8_t btbh_hdr_clamp_to_unorm8(float value)
{
    const float clamped = btbh_hdr_saturate(value);
    return (uint8_t)(clamped * 255.0f + 0.5f);
}

#endif
