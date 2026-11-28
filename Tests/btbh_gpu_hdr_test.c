/* HDR output's curves on the CPU (RFC 0017, since 0.17.0): btbh_gpu_hdr.h, the reference the shaders follow, and the
 * public structs' layout. No GPU.
 * - The highlight gain: 1 at and below the knee, C1 there, monotonic, toward P; exactly 1 when P is 1.
 * - The knee for each tone mapper, against RFC 0017's table.
 * - PQ: BT.2100's values (203 and 1000 nits), its round trip; BT.709 to BT.2020 keeps white.
 * - The encode of paper white and of a highlight, in both compositions, and its clamp at the peak.
 * - The fold: the identity at and below its knee and for P = 1, continuous, the peak to 1.
 * - Halves both ways, the RGBA8 clamp.
 * - BTBHGPUWindowHDRSettings, BTBHGPUWindowHDRState and BTBHGPUPlatformSnapshot's sizes and offsets on 64-bit. */
#undef NDEBUG

#include "btbh_gpu/btbh_gpu.h"
#include "btbh_gpu_hdr.h"

#include <assert.h>
#include <math.h>
#include <stddef.h>
#include <stdio.h>

static int near(float a, float b, float tolerance) {
    return fabsf(a - b) <= tolerance;
}

static void check_gain(void) {
    const float ratio = 1000.0f / 203.0f;
    const float knee = 0.476f;
    const float shoulder = 4.0f * knee;
    assert(btbh_hdr_gain(0.0f, ratio, knee, shoulder) == 1.0f);
    assert(btbh_hdr_gain(0.18f, ratio, knee, shoulder) == 1.0f);
    assert(btbh_hdr_gain(knee, ratio, knee, shoulder) == 1.0f);
    /* C1 at the knee: the slope just above it is ~0. */
    const float step = 1.0e-3f;
    const float slope = (btbh_hdr_gain(knee + step, ratio, knee, shoulder) - 1.0f) / step;
    assert(slope >= 0.0f && slope < 0.01f);
    float previous = 1.0f;
    for (int index = 1; index <= 2000; ++index) {
        const float heat = knee + (float)index * 0.05f;
        const float gain = btbh_hdr_gain(heat, ratio, knee, shoulder);
        assert(gain >= previous);
        assert(gain <= ratio);
        previous = gain;
    }
    /* Within 1 % of P by m_k + 10 tau. */
    assert(btbh_hdr_gain(knee + 10.0f * shoulder, ratio, knee, shoulder) >= 0.99f * ratio);
    /* P = 1: no lift anywhere. */
    for (int index = 0; index < 100; ++index) {
        assert(btbh_hdr_gain((float)index, 1.0f, knee, shoulder) == 1.0f);
    }
    /* The RFC's table (ACES, P = 4.93): gain 1.28 at m = 1, 4.04 at 4, 4.87 at 16. */
    const float aces_knee = btbh_hdr_knee_input(BTBH_HDR_TONEMAP_ACES, 1.0f, BTBH_HDR_KNEE_OUTPUT);
    const float aces_shoulder = BTBH_HDR_SHOULDER_SCALE * aces_knee;
    assert(near(btbh_hdr_gain(1.0f, ratio, aces_knee, aces_shoulder), 1.276f, 0.01f));
    assert(near(btbh_hdr_gain(4.0f, ratio, aces_knee, aces_shoulder), 4.04f, 0.02f));
    assert(near(btbh_hdr_gain(16.0f, ratio, aces_knee, aces_shoulder), 4.87f, 0.02f));
}

static void check_knee(void) {
    const float aces = btbh_hdr_knee_input(BTBH_HDR_TONEMAP_ACES, 1.0f, BTBH_HDR_KNEE_OUTPUT);
    assert(near(aces, 0.4758f, 1.0e-3f));
    const float neutral = btbh_hdr_knee_input(
        BTBH_HDR_TONEMAP_PBR_NEUTRAL, 1.0f, btbh_hdr_knee_output(BTBH_HDR_TONEMAP_PBR_NEUTRAL, 0.0f));
    assert(near(neutral, 0.80f, 1.0e-3f));
    const float reinhard = btbh_hdr_knee_input(BTBH_HDR_TONEMAP_REINHARD, 4.0f, BTBH_HDR_KNEE_OUTPUT);
    assert(near(reinhard, 1.254f, 2.0e-3f));
    assert(btbh_hdr_knee_output(BTBH_HDR_TONEMAP_ACES, 0.0f) == BTBH_HDR_KNEE_OUTPUT);
    assert(btbh_hdr_knee_output(BTBH_HDR_TONEMAP_ACES, 0.7f) == 0.7f);
    assert(btbh_hdr_knee_output(BTBH_HDR_TONEMAP_ACES, 1.5f) == BTBH_HDR_KNEE_OUTPUT);
    /* The curve at its knee gives the knee's output. */
    assert(near(btbh_hdr_tone_curve(BTBH_HDR_TONEMAP_ACES, 1.0f, aces), 0.6f, 1.0e-4f));
}

static void check_pq(void) {
    assert(near(btbh_hdr_pq_encode(203.0f / 10000.0f), 0.5807f, 1.0e-4f));
    assert(near(btbh_hdr_pq_encode(1000.0f / 10000.0f), 0.7519f, 1.0e-4f));
    assert(btbh_hdr_pq_encode(0.0f) < 1.0e-6f);
    assert(near(btbh_hdr_pq_encode(1.0f), 1.0f, 1.0e-6f));
    for (int index = 1; index <= 1000; ++index) {
        const float y = (float)index / 1000.0f;
        const float back = btbh_hdr_pq_decode(btbh_hdr_pq_encode(y));
        assert(fabsf(back - y) <= 1.0e-5f * (1.0f + y * 10.0f));
    }
    const float white[3] = { 1.0f, 1.0f, 1.0f };
    float wide[3];
    btbh_hdr_709_to_2020(white, wide);
    for (int channel = 0; channel < 3; ++channel) {
        assert(near(wide[channel], 1.0f, 1.0e-4f));
    }
}

static void check_encode(void) {
    const float paper = 203.0f;
    const float ratio = 1000.0f / 203.0f;
    const float one[3] = { 1.0f, 1.0f, 1.0f };
    float out[3];
    btbh_hdr_encode_pixel(one, 2u, paper, ratio, paper / 80.0f, out);
    for (int channel = 0; channel < 3; ++channel) {
        assert(near(out[channel], 0.5807f, 2.0e-4f));
    }
    btbh_hdr_encode_pixel(one, 3u, paper, ratio, paper / 80.0f, out);
    for (int channel = 0; channel < 3; ++channel) {
        assert(near(out[channel], paper / 80.0f, 1.0e-5f));
    }
    /* A highlight at the peak's encoding is the peak; above it, clamped to it. */
    const float peak_value = powf(ratio, 1.0f / 2.2f);
    const float at_peak[3] = { peak_value, peak_value, peak_value };
    btbh_hdr_encode_pixel(at_peak, 2u, paper, ratio, paper / 80.0f, out);
    assert(near(out[0], 0.7519f, 3.0e-4f));
    const float over[3] = { 3.0f * peak_value, 3.0f * peak_value, 0.0f };
    btbh_hdr_encode_pixel(over, 3u, paper, ratio, paper / 80.0f, out);
    assert(near(out[0], 1000.0f / 80.0f, 1.0e-2f));
    assert(out[2] == 0.0f);
    /* Apple's EDR: relative light. */
    btbh_hdr_encode_pixel(one, 3u, paper, ratio, 1.0f, out);
    assert(near(out[0], 1.0f, 1.0e-6f));
}

static void check_fold(void) {
    const float ratio = 1000.0f / 203.0f;
    for (int index = 0; index <= 60; ++index) {
        const float m = (float)index / 100.0f;
        assert(btbh_hdr_fold_largest(m, ratio) == m);
    }
    for (int index = 0; index <= 200; ++index) {
        const float m = (float)index / 40.0f;
        assert(btbh_hdr_fold_largest(m, 1.0f) == m);
    }
    assert(near(btbh_hdr_fold_largest(ratio, ratio), 1.0f, 1.0e-5f));
    float previous = BTBH_HDR_FOLD_KNEE;
    for (int index = 1; index <= 1000; ++index) {
        const float m = BTBH_HDR_FOLD_KNEE + (ratio - BTBH_HDR_FOLD_KNEE) * (float)index / 1000.0f;
        const float folded = btbh_hdr_fold_largest(m, ratio);
        assert(folded > previous && folded <= 1.0f + 1.0e-5f);
        previous = folded;
    }
    /* C1 at the knee. */
    const float slope =
        (btbh_hdr_fold_largest(BTBH_HDR_FOLD_KNEE + 1.0e-3f, ratio) - BTBH_HDR_FOLD_KNEE) / 1.0e-3f;
    assert(near(slope, 1.0f, 0.01f));
    const float sdr[3] = { 0.5f, 0.7f, 0.9f };
    float out[3];
    btbh_hdr_fold_pixel(sdr, 1.0f, out);
    assert(out[0] == sdr[0] && out[1] == sdr[1] && out[2] == sdr[2]);
}

static void check_halves(void) {
    const float values[] = { 0.0f, 1.0f, 0.5f, 2.0f, 0.333f, 65504.0f, 1.0e-6f, -1.5f };
    for (size_t index = 0u; index < sizeof(values) / sizeof(values[0]); ++index) {
        const float back = btbh_hdr_half_to_float(btbh_hdr_float_to_half(values[index]));
        assert(fabsf(back - values[index]) <= fabsf(values[index]) * 1.0e-3f + 1.0e-7f);
    }
    for (uint32_t bits = 0u; bits < 0x7c00u; bits += 7u) {
        const uint16_t half = (uint16_t)bits;
        assert(btbh_hdr_float_to_half(btbh_hdr_half_to_float(half)) == half);
    }
    assert(btbh_hdr_clamp_to_unorm8(-1.0f) == 0u);
    assert(btbh_hdr_clamp_to_unorm8(2.0f) == 255u);
    for (int value = 0; value < 256; ++value) {
        assert(btbh_hdr_clamp_to_unorm8((float)value / 255.0f) == (uint8_t)value);
    }
}

static void check_layout(void) {
#if UINTPTR_MAX == 0xffffffffffffffffu
    assert(sizeof(BTBHGPUWindowHDRSettings) == 32u);
    assert(offsetof(BTBHGPUWindowHDRSettings, mode) == 0u);
    assert(offsetof(BTBHGPUWindowHDRSettings, composition) == 1u);
    assert(offsetof(BTBHGPUWindowHDRSettings, flags) == 2u);
    assert(offsetof(BTBHGPUWindowHDRSettings, paper_white_nits) == 4u);
    assert(offsetof(BTBHGPUWindowHDRSettings, peak_nits) == 8u);
    assert(offsetof(BTBHGPUWindowHDRSettings, reserved) == 12u);
    assert(sizeof(BTBHGPUWindowHDRState) == 224u);
    assert(offsetof(BTBHGPUWindowHDRState, serial) == 0u);
    assert(offsetof(BTBHGPUWindowHDRState, composition) == 4u);
    assert(offsetof(BTBHGPUWindowHDRState, swapchain_format) == 8u);
    assert(offsetof(BTBHGPUWindowHDRState, frame_format) == 12u);
    assert(offsetof(BTBHGPUWindowHDRState, target_format) == 16u);
    assert(offsetof(BTBHGPUWindowHDRState, mode) == 20u);
    assert(offsetof(BTBHGPUWindowHDRState, active) == 21u);
    assert(offsetof(BTBHGPUWindowHDRState, available) == 22u);
    assert(offsetof(BTBHGPUWindowHDRState, os_hdr_enabled) == 23u);
    assert(offsetof(BTBHGPUWindowHDRState, supported) == 24u);
    assert(offsetof(BTBHGPUWindowHDRState, under_wine) == 28u);
    assert(offsetof(BTBHGPUWindowHDRState, relative_luminance) == 29u);
    assert(offsetof(BTBHGPUWindowHDRState, metadata_set) == 30u);
    assert(offsetof(BTBHGPUWindowHDRState, fallback) == 31u);
    assert(offsetof(BTBHGPUWindowHDRState, sdr_white_level) == 32u);
    assert(offsetof(BTBHGPUWindowHDRState, hdr_headroom) == 36u);
    assert(offsetof(BTBHGPUWindowHDRState, display_sdr_white_nits) == 40u);
    assert(offsetof(BTBHGPUWindowHDRState, display_peak_nits) == 44u);
    assert(offsetof(BTBHGPUWindowHDRState, paper_white_nits) == 48u);
    assert(offsetof(BTBHGPUWindowHDRState, peak_nits) == 52u);
    assert(offsetof(BTBHGPUWindowHDRState, highlight_ratio) == 56u);
    assert(offsetof(BTBHGPUWindowHDRState, switch_count) == 60u);
    assert(offsetof(BTBHGPUWindowHDRState, last_switch_milliseconds) == 64u);
    assert(offsetof(BTBHGPUWindowHDRState, display_id) == 72u);
    assert(offsetof(BTBHGPUWindowHDRState, reserved) == 76u);
    assert(offsetof(BTBHGPUWindowHDRState, message) == 96u);
    assert(sizeof(BTBHGPUPlatformSnapshot) == 56u);
    assert(offsetof(BTBHGPUPlatformSnapshot, hdr_changed) == 47u);
    assert(offsetof(BTBHGPUPlatformSnapshot, reserved) == 48u);
#endif
    assert(BTBH_GPU_SWAPCHAIN_HDR10_ST2084 == 2);
    assert(BTBH_GPU_SWAPCHAIN_HDR_EXTENDED_LINEAR == 3);
    assert(BTBH_GPU_HDR_MODE_FORCE == 2);
    assert(BTBH_GPU_HDR_FALLBACK_PENDING == 5);
}

int main(void) {
    check_gain();
    check_knee();
    check_pq();
    check_encode();
    check_fold();
    check_halves();
    check_layout();
    printf("hdr test passed\n");
    return 0;
}
