/* The cover laid by facing and still ice on the CPU (RFC 0020, since 0.18.0): btbh_gpu_cover.h, the reference the
 * shaders follow, and the public structs' layout. No GPU.
 * - BTBHGPUSceneCoverLayer: 64 bytes, BTBHGPUSceneCover 136, with RFC 0020's offsets; the flags2 bits, the variant's
 *   number, the enums.
 * - Each check of btbh_gpu_window_scene_set_cover, with its message naming the layer and the field (phase 2's fields
 *   refused unless 0); a NULL window refused, a NULL cover taken.
 * - The block: the zero values resolved (slope 60, edge 8, noise 1, each pattern's roughness), COARSE drops the relief,
 *   BTBH_DEBUG_COVER's flag; a batch's mode: its layer only when that layer is on, the see-through bit.
 * - The facing: c 1 well within the reach, 0 past the edge, the patterns' edges at known points; no cover at amount 0
 *   (to the bit); amount and slope move the reach; the noise moves it by edge_noise_degrees at most.
 * - The noise: within -1..1, its mean near 0, its gradient the finite difference of its value, its octaves faded by the
 *   footprint (the edge goes clean where both fade); COARSE the first octave alone.
 * - The relief: a flat normal without bump; a tilted one with it, still unit and toward the face.
 * - MUD's puddles: rougher dry than wet, never below the puddle's roughness.
 * - Still ice: the parameters' refusals, the zero values, the plates' border distance (0 on the border, never
 *   negative), a crack 1 on its line and fading below a pixel, the frost's share, the roughness between clear and
 *   frosted. */
#undef NDEBUG

#include "btbh_gpu/btbh_gpu.h"
#include "btbh_gpu_cover.h"

#include <assert.h>
#include <math.h>
#include <stddef.h>
#include <stdio.h>
#include <string.h>

_Static_assert(sizeof(BTBHGPUSceneCoverLayer) == 64u, "a cover layer is 64 bytes");
_Static_assert(offsetof(BTBHGPUSceneCoverLayer, color) == 0u, "color");
_Static_assert(offsetof(BTBHGPUSceneCoverLayer, amount) == 12u, "amount");
_Static_assert(offsetof(BTBHGPUSceneCoverLayer, slope_degrees) == 16u, "slope_degrees");
_Static_assert(offsetof(BTBHGPUSceneCoverLayer, edge_degrees) == 20u, "edge_degrees");
_Static_assert(offsetof(BTBHGPUSceneCoverLayer, edge_noise_degrees) == 24u, "edge_noise_degrees");
_Static_assert(offsetof(BTBHGPUSceneCoverLayer, noise_scale) == 28u, "noise_scale");
_Static_assert(offsetof(BTBHGPUSceneCoverLayer, roughness) == 32u, "roughness");
_Static_assert(offsetof(BTBHGPUSceneCoverLayer, bump) == 36u, "bump");
_Static_assert(offsetof(BTBHGPUSceneCoverLayer, normal_influence) == 40u, "normal_influence");
_Static_assert(offsetof(BTBHGPUSceneCoverLayer, base_height) == 44u, "base_height");
_Static_assert(offsetof(BTBHGPUSceneCoverLayer, base_fade) == 48u, "base_fade");
_Static_assert(offsetof(BTBHGPUSceneCoverLayer, wind) == 52u, "wind");
_Static_assert(offsetof(BTBHGPUSceneCoverLayer, pattern) == 60u, "pattern");
_Static_assert(offsetof(BTBHGPUSceneCoverLayer, flags) == 61u, "flags");
_Static_assert(offsetof(BTBHGPUSceneCoverLayer, reserved) == 62u, "reserved");
_Static_assert(sizeof(BTBHGPUSceneCover) == 136u, "the cover is 136 bytes");
_Static_assert(offsetof(BTBHGPUSceneCover, layers) == 0u, "layers");
_Static_assert(offsetof(BTBHGPUSceneCover, flags) == 128u, "flags");
_Static_assert(offsetof(BTBHGPUSceneCover, reserved) == 132u, "reserved");
_Static_assert(BTBH_GPU_SCENE_BATCH_FLAGS2_COVER_0 == 4u && BTBH_GPU_SCENE_BATCH_FLAGS2_COVER_1 == 8u,
    "flags2 bits 2 and 3 (bit 0 KEEP_CASCADES_LOOK, bit 1 RFC 0019's)");
_Static_assert(BTBH_GPU_SCENE_SURFACE_ICE == 12 && BTBH_GPU_SCENE_SURFACE_COUNT == 13, "ICE is variant 12");
_Static_assert(BTBH_GPU_SCENE_COVER_SNOW == 0 && BTBH_GPU_SCENE_COVER_SAND == 1 && BTBH_GPU_SCENE_COVER_MUD == 2 &&
    BTBH_GPU_SCENE_COVER_PATTERN_COUNT == 3, "the patterns");
_Static_assert(BTBH_GPU_SCENE_COVER_LAYER_COARSE == 1u, "COARSE is layer flag bit 0");
_Static_assert(BTBH_GPU_SCENE_COVER_LAYER_COUNT == 2u, "two layers");
_Static_assert(sizeof(BTBHGPUCoverUniforms) == 128u, "the block is 128 bytes");
_Static_assert(offsetof(BTBHGPUCoverUniforms, facing) == 32u && offsetof(BTBHGPUCoverUniforms, relief) == 64u &&
    offsetof(BTBHGPUCoverUniforms, pattern) == 96u, "the block's float4 arrays");

static const float up[3] = {0.0f, 1.0f, 0.0f};

static BTBHGPUSceneCoverLayer layer_valid(void) {
    BTBHGPUSceneCoverLayer layer;
    memset(&layer, 0, sizeof(layer));
    layer.color[0] = 0.8f;
    layer.color[1] = 0.82f;
    layer.color[2] = 0.86f;
    layer.amount = 0.75f;
    layer.slope_degrees = 60.0f;
    layer.edge_degrees = 30.0f;
    layer.edge_noise_degrees = 12.0f;
    layer.noise_scale = 1.5f;
    layer.bump = 0.4f;
    return layer;
}

/* The check's message for a cover with layer `index` changed by `change`, or "" when it is taken. */
static void expect_refused(const BTBHGPUSceneCover *cover, const char *needle) {
    char message[256];
    message[0] = '\0';
    const bool taken = btbh_cover_check(cover, message, sizeof(message));
    if (taken || strstr(message, needle) == NULL) {
        fprintf(stderr, "expected a refusal naming \"%s\", got %s \"%s\"\n", needle, taken ? "taken" : "refused",
            message);
    }
    assert(!taken && strstr(message, needle) != NULL);
}

static void check_refusals(void) {
    char message[256];
    assert(btbh_cover_check(NULL, message, sizeof(message)));
    BTBHGPUSceneCover cover;
    memset(&cover, 0, sizeof(cover));
    assert(btbh_cover_check(&cover, message, sizeof(message)));
    cover.layers[0] = layer_valid();
    cover.layers[1] = layer_valid();
    cover.layers[1].pattern = BTBH_GPU_SCENE_COVER_MUD;
    cover.layers[1].flags = BTBH_GPU_SCENE_COVER_LAYER_COARSE;
    assert(btbh_cover_check(&cover, message, sizeof(message)));
    /* Each float: not finite, then out of range. */
    const struct {
        size_t offset;
        const char *name;
        float bad;
    } cases[] = {
        {offsetof(BTBHGPUSceneCoverLayer, color[0]), "color[0]", 1.5f},
        {offsetof(BTBHGPUSceneCoverLayer, color[1]), "color[1]", -0.1f},
        {offsetof(BTBHGPUSceneCoverLayer, color[2]), "color[2]", 2.0f},
        {offsetof(BTBHGPUSceneCoverLayer, amount), "amount", 1.01f},
        {offsetof(BTBHGPUSceneCoverLayer, slope_degrees), "slope_degrees", 91.0f},
        {offsetof(BTBHGPUSceneCoverLayer, edge_degrees), "edge_degrees", -1.0f},
        {offsetof(BTBHGPUSceneCoverLayer, edge_noise_degrees), "edge_noise_degrees", 95.0f},
        {offsetof(BTBHGPUSceneCoverLayer, noise_scale), "noise_scale", -0.5f},
        {offsetof(BTBHGPUSceneCoverLayer, roughness), "roughness", 1.2f},
        {offsetof(BTBHGPUSceneCoverLayer, bump), "bump", -0.2f},
        {offsetof(BTBHGPUSceneCoverLayer, normal_influence), "normal_influence", 1.5f},
        {offsetof(BTBHGPUSceneCoverLayer, base_height), "base_height", 0.5f},
        {offsetof(BTBHGPUSceneCoverLayer, base_fade), "base_fade", 0.5f},
        {offsetof(BTBHGPUSceneCoverLayer, wind[0]), "wind[0]", 0.5f},
        {offsetof(BTBHGPUSceneCoverLayer, wind[1]), "wind[1]", -0.5f},
    };
    for (uint32_t layer = 0u; layer < 2u; ++layer) {
        for (size_t index = 0u; index < sizeof(cases) / sizeof(cases[0]); ++index) {
            const float values[2] = {NAN, cases[index].bad};
            for (int which = 0; which < 2; ++which) {
                BTBHGPUSceneCover changed = cover;
                memcpy((uint8_t *)&changed.layers[layer] + cases[index].offset, &values[which], sizeof(float));
                char needle[96];
                (void)snprintf(needle, sizeof(needle), "layer %u: %s", layer, cases[index].name);
                expect_refused(&changed, needle);
            }
        }
        BTBHGPUSceneCover changed = cover;
        changed.layers[layer].wind[0] = INFINITY;
        expect_refused(&changed, "not finite");
        changed = cover;
        changed.layers[layer].base_height = 0.1f;
        expect_refused(&changed, "phase 2");
        changed = cover;
        changed.layers[layer].pattern = (uint8_t)BTBH_GPU_SCENE_COVER_PATTERN_COUNT;
        expect_refused(&changed, "pattern 3 is unknown");
        changed = cover;
        changed.layers[layer].flags = 2u;
        expect_refused(&changed, "flags has an unknown bit");
        changed = cover;
        changed.layers[layer].reserved[1] = 1u;
        expect_refused(&changed, "reserved is not 0");
    }
    BTBHGPUSceneCover changed = cover;
    changed.flags = 1u;
    expect_refused(&changed, "Cover: flags");
    changed = cover;
    changed.reserved = 1u;
    expect_refused(&changed, "Cover: reserved");
    /* The edges of the ranges are taken. */
    changed = cover;
    changed.layers[0].amount = 1.0f;
    changed.layers[0].slope_degrees = 90.0f;
    changed.layers[0].edge_degrees = 90.0f;
    changed.layers[0].edge_noise_degrees = 90.0f;
    changed.layers[0].roughness = 1.0f;
    changed.layers[0].bump = 1.0f;
    changed.layers[0].normal_influence = 1.0f;
    changed.layers[0].color[0] = 0.0f;
    changed.layers[0].color[1] = 1.0f;
    changed.layers[0].noise_scale = 1000.0f;
    assert(btbh_cover_check(&changed, message, sizeof(message)));
    printf("refusals: every field, NaN and out of range, both layers; phase 2's fields; the cover's flags and "
           "reserved\n");
}

static void check_block(void) {
    BTBHGPUSceneCover cover;
    memset(&cover, 0, sizeof(cover));
    cover.layers[0].amount = 0.5f;
    cover.layers[1].amount = 1.0f;
    cover.layers[1].pattern = BTBH_GPU_SCENE_COVER_SAND;
    cover.layers[1].flags = BTBH_GPU_SCENE_COVER_LAYER_COARSE;
    cover.layers[1].bump = 0.7f;
    cover.layers[1].roughness = 0.3f;
    BTBHGPUCoverUniforms block;
    btbh_cover_pack(&cover, false, &block);
    const float radians = BTBH_COVER_PI / 180.0f;
    assert(fabsf(block.facing[0][0] - 0.5f * 60.0f * radians) < 1.0e-6f);
    assert(fabsf(block.facing[0][1] - 1.0f / (8.0f * radians)) < 1.0e-4f);
    assert(block.facing[0][2] == 0.0f && block.facing[0][3] == 1.0f);
    assert(block.color_roughness[0][3] == BTBH_COVER_SNOW_ROUGHNESS);
    assert(block.color_roughness[1][3] == 0.3f);
    assert(block.relief[1][0] == 0.0f && block.pattern[1][0] == 1u && block.pattern[1][1] == BTBH_COVER_FLAG_COARSE);
    cover.layers[1].roughness = 0.0f;
    btbh_cover_pack(&cover, true, &block);
    assert(block.color_roughness[1][3] == BTBH_COVER_SAND_ROUGHNESS);
    assert(block.pattern[0][1] == BTBH_COVER_FLAG_DEBUG && block.pattern[1][1] == (BTBH_COVER_FLAG_COARSE |
        BTBH_COVER_FLAG_DEBUG));
    cover.layers[0].pattern = BTBH_GPU_SCENE_COVER_MUD;
    btbh_cover_pack(&cover, false, &block);
    assert(block.color_roughness[0][3] == BTBH_COVER_MUD_ROUGHNESS);
    btbh_cover_pack(NULL, true, &block);
    const uint8_t *bytes = (const uint8_t *)&block;
    for (size_t index = 0u; index < sizeof(block); ++index) {
        assert(bytes[index] == 0u);
    }
    /* A batch's mode. */
    BTBHGPUSceneGeometryBatch batch;
    memset(&batch, 0, sizeof(batch));
    assert(btbh_cover_batch_mode(&cover, &batch, false) == 0u);
    batch.flags2 = BTBH_GPU_SCENE_BATCH_FLAGS2_COVER_0 | BTBH_GPU_SCENE_BATCH_FLAGS2_KEEP_CASCADES_LOOK;
    assert(btbh_cover_batch_mode(&cover, &batch, false) == 1u);
    assert(btbh_cover_batch_mode(&cover, &batch, true) == (1u | BTBH_COVER_MODE_SEE_THROUGH));
    assert(btbh_cover_batch_mode(NULL, &batch, true) == 0u);
    batch.flags2 = BTBH_GPU_SCENE_BATCH_FLAGS2_COVER_1;
    assert(btbh_cover_batch_mode(&cover, &batch, false) == 2u);
    cover.layers[1].amount = 0.0f;
    assert(btbh_cover_batch_mode(&cover, &batch, true) == 0u);
    printf("block: the zero values, COARSE, the debug flag, NULL all zero; a batch's mode by its bits and its layer\n");
}

/* The reference's c at a slope (degrees) with no noise, through a packed block. */
static float coverage_at(const BTBHGPUSceneCoverLayer *layer, float slope_degrees) {
    BTBHGPUSceneCover cover;
    memset(&cover, 0, sizeof(cover));
    cover.layers[0] = *layer;
    BTBHGPUCoverUniforms block;
    btbh_cover_pack(&cover, false, &block);
    const float angle = slope_degrees * BTBH_COVER_PI / 180.0f;
    const float normal[3] = {0.0f, cosf(angle), sinf(angle)};
    const float position[3] = {3.3f, 1.0f, -7.1f};
    const float albedo[3] = {0.1f, 0.2f, 0.3f};
    const BTBHCoverSample sample = btbh_cover_sample(&block, 0u, position, 0.01f, normal, normal, albedo, 0.5f, 0.2f,
        0.4f, 0.0f);
    return sample.c;
}

static void check_facing(void) {
    BTBHGPUSceneCoverLayer layer = layer_valid();
    layer.edge_noise_degrees = 0.0f;
    /* Reach 45 degrees, fading over 30: raw c = (45 - slope) / 30. */
    for (uint32_t pattern = 0u; pattern < 3u; ++pattern) {
        layer.pattern = (uint8_t)pattern;
        assert(coverage_at(&layer, 0.0f) == 1.0f);
        assert(coverage_at(&layer, 14.0f) == 1.0f);
        assert(coverage_at(&layer, 45.5f) == 0.0f);
        assert(coverage_at(&layer, 60.0f) == 0.0f);
        assert(coverage_at(&layer, 120.0f) == 0.0f);
        float previous = 1.0f;
        for (float slope = 0.0f; slope <= 90.0f; slope += 0.5f) {
            const float c = coverage_at(&layer, slope);
            assert(c <= previous + 1.0e-6f && c >= 0.0f && c <= 1.0f);
            previous = c;
        }
    }
    /* At slope 30 the raw c is 0.5: SNOW's smoothstep(0.3, 0.7) gives 0.5, SAND 0.5, MUD smoothstep(0.15, 0.55,
       0.5). */
    layer.pattern = BTBH_GPU_SCENE_COVER_SNOW;
    assert(fabsf(coverage_at(&layer, 30.0f) - 0.5f) < 1.0e-4f);
    layer.pattern = BTBH_GPU_SCENE_COVER_SAND;
    assert(fabsf(coverage_at(&layer, 30.0f) - 0.5f) < 1.0e-4f);
    layer.pattern = BTBH_GPU_SCENE_COVER_MUD;
    const float t = (0.5f - 0.15f) / 0.4f;
    assert(fabsf(coverage_at(&layer, 30.0f) - t * t * (3.0f - 2.0f * t)) < 1.0e-4f);
    /* SNOW's crisp edge: raw 0.3 and below is bare, 0.7 and above full. */
    layer.pattern = BTBH_GPU_SCENE_COVER_SNOW;
    assert(coverage_at(&layer, 36.5f) == 0.0f);
    assert(coverage_at(&layer, 23.5f) == 1.0f);
    /* Amount 0: nothing, to the bit, even on level faces with noise. */
    layer = layer_valid();
    layer.amount = 0.0f;
    for (uint32_t pattern = 0u; pattern < 3u; ++pattern) {
        layer.pattern = (uint8_t)pattern;
        assert(coverage_at(&layer, 0.0f) == 0.0f);
    }
    /* The default slope and edge: reach 60 x amount. */
    memset(&layer, 0, sizeof(layer));
    layer.amount = 1.0f;
    layer.pattern = BTBH_GPU_SCENE_COVER_SAND;
    assert(coverage_at(&layer, 51.5f) == 1.0f);
    assert(fabsf(coverage_at(&layer, 56.0f) - 0.5f) < 1.0e-3f);
    assert(coverage_at(&layer, 60.5f) == 0.0f);
    /* Past the farthest it reaches (amount x slope + the noise's reach: 45 + 12 x min(1, 4 x 0.75) = 57 degrees) the
       surface is left as it was, wherever it is; short of it (SAND, where the noise passes 0.3) the noise lays some. */
    layer = layer_valid();
    layer.pattern = BTBH_GPU_SCENE_COVER_SAND;
    BTBHGPUSceneCover cover;
    memset(&cover, 0, sizeof(cover));
    cover.layers[0] = layer;
    BTBHGPUCoverUniforms block;
    btbh_cover_pack(&cover, false, &block);
    const float reach = block.facing[0][0] + block.facing[0][2];
    assert(fabsf(reach - 57.0f * BTBH_COVER_PI / 180.0f) < 1.0e-5f);
    const float albedo[3] = {0.1f, 0.2f, 0.3f};
    float most_short = 0.0f;
    for (int at = 0; at < 2000; ++at) {
        const float position[3] = {-20.0f + 0.0371f * (float)at, 1.0f, 13.0f - 0.0293f * (float)at};
        for (int side = 0; side < 2; ++side) {
            const float angle = side == 0 ? reach + 1.0e-4f : reach - 0.7f * block.facing[0][2];
            const float normal[3] = {0.0f, cosf(angle), sinf(angle)};
            const BTBHCoverSample sample = btbh_cover_sample(&block, 0u, position, 0.01f, normal, normal, albedo, 0.5f,
                0.2f, 0.4f, 1.0f);
            if (side == 0) {
                assert(sample.c == 0.0f && sample.noise == 0.0f);
                assert(sample.albedo[0] == albedo[0] && sample.albedo[1] == albedo[1] && sample.albedo[2] == albedo[2]);
                assert(sample.roughness == 0.5f && sample.metalness == 0.2f && sample.occlusion == 0.4f);
                assert(sample.normal[0] == normal[0] && sample.normal[1] == normal[1] && sample.normal[2] == normal[2]);
            } else {
                most_short = sample.c > most_short ? sample.c : most_short;
            }
        }
    }
    assert(most_short > 0.0f);
    printf("facing: the reach and the edge, monotonic in slope, the patterns' edges, amount 0 lays nothing; past the "
           "reach (%.1f degrees) the surface as it was (short of it, c up to %.3f)\n",
        (double)(reach * 180.0f / BTBH_COVER_PI), (double)most_short);
}

static void check_noise(void) {
    double sum = 0.0;
    float low = 1.0f;
    float high = -1.0f;
    uint32_t count = 0u;
    for (int y = 0; y < 400; ++y) {
        for (int x = 0; x < 400; ++x) {
            float g1[2];
            float g2[2];
            float spread;
            const float noise = btbh_cover_noise(-37.3f + 0.137f * (float)x, 911.0f + 0.211f * (float)y, 0.01f, false,
                g1, g2, &spread);
            assert(spread == 0.0f);
            assert(noise >= -1.0f && noise <= 1.0f);
            low = noise < low ? noise : low;
            high = noise > high ? noise : high;
            sum += noise;
            count += 1u;
        }
    }
    printf("noise: %.3f..%.3f, mean %.4f over %u points\n", (double)low, (double)high, sum / count, count);
    assert(low < -0.5f && high > 0.5f && fabs(sum / count) < 0.05);
    /* The value noise's gradient is its finite difference. */
    for (int index = 0; index < 200; ++index) {
        const float x = -5.0f + 0.0731f * (float)index;
        const float y = 3.0f + 0.0517f * (float)index;
        float gradient[2];
        float unused[2];
        const float h = 1.0e-3f;
        (void)btbh_cover_value_noise(x, y, gradient);
        const float dx = (btbh_cover_value_noise(x + h, y, unused) - btbh_cover_value_noise(x - h, y, unused)) /
            (2.0f * h);
        const float dy = (btbh_cover_value_noise(x, y + h, unused) - btbh_cover_value_noise(x, y - h, unused)) /
            (2.0f * h);
        assert(fabsf(dx - gradient[0]) < 0.02f && fabsf(dy - gradient[1]) < 0.02f);
    }
    /* The footprint fades the octaves: at 0.4 cells a pixel the second is gone, at 0.6 both (N is 0). */
    float g1[2];
    float g2[2];
    float spread;
    const float coarse = btbh_cover_noise(12.7f, -3.1f, 0.01f, true, g1, g2, &spread);
    assert(g2[0] == 0.0f && g2[1] == 0.0f && spread == 0.0f);
    const float faded = btbh_cover_noise(12.7f, -3.1f, 0.4f, false, g1, g2, &spread);
    assert(fabsf(faded - coarse * 0.5f) < 1.0e-5f);
    assert(g2[0] == 0.0f && g2[1] == 0.0f);
    /* What faded out comes back as the spread: the first octave's three quarters, the second's whole. */
    assert(fabsf(spread - sqrtf(0.75f * BTBH_COVER_OCTAVE1_SPREAD * BTBH_COVER_OCTAVE1_SPREAD +
        BTBH_COVER_OCTAVE2_SPREAD * BTBH_COVER_OCTAVE2_SPREAD)) < 1.0e-5f);
    assert(btbh_cover_noise(12.7f, -3.1f, 0.6f, false, g1, g2, &spread) == 0.0f);
    assert(fabsf(spread - sqrtf(BTBH_COVER_OCTAVE1_SPREAD * BTBH_COVER_OCTAVE1_SPREAD +
        BTBH_COVER_OCTAVE2_SPREAD * BTBH_COVER_OCTAVE2_SPREAD)) < 1.0e-5f);
    (void)btbh_cover_noise(12.7f, -3.1f, 0.6f, true, g1, g2, &spread);
    assert(fabsf(spread - BTBH_COVER_OCTAVE1_SPREAD) < 1.0e-6f);
    /* The spreads are the octaves' standard deviations times sqrt(3), measured. */
    double sum1 = 0.0;
    double sum2 = 0.0;
    for (int index = 0; index < 160000; ++index) {
        float unused[2];
        const float at_x = -900.0f + 0.0137f * (float)(index % 400) + 3.1f * (float)(index / 400);
        const float at_y = 400.0f + 0.0211f * (float)(index / 400) + 1.7f * (float)(index % 400);
        const double a1 = (btbh_cover_value_noise(at_x, at_y, unused) - 0.5) * (4.0 / 3.0);
        const double a2 = 0.5 * (btbh_cover_value_noise(at_x * 2.03f + 17.31f, at_y * 2.03f + 17.31f, unused) - 0.5) *
            (4.0 / 3.0);
        sum1 += a1 * a1;
        sum2 += a2 * a2;
    }
    const double measured1 = sqrt(3.0 * sum1 / 160000.0);
    const double measured2 = sqrt(3.0 * sum2 / 160000.0);
    printf("noise: the octaves' box half-widths measured %.4f and %.4f (52/105 = %.4f, 26/105 = %.4f)\n", measured1,
        measured2, (double)BTBH_COVER_OCTAVE1_SPREAD, (double)BTBH_COVER_OCTAVE2_SPREAD);
    assert(fabs(measured1 - BTBH_COVER_OCTAVE1_SPREAD) < 0.01 && fabs(measured2 - BTBH_COVER_OCTAVE2_SPREAD) < 0.005);
    /* The hash is btbh_detail_hash's 24 bits. */
    assert(btbh_cover_hash01(0.0f, 0.0f) == (float)(btbh_cover_hash(0.0f, 0.0f) & 0x00ffffffu) / 16777216.0f);
    assert(btbh_cover_hash(-1.0f, 5.0f) != btbh_cover_hash(1.0f, 5.0f));
    printf("noise: within -1..1, its gradient its finite difference, faded by the footprint, COARSE one octave\n");
}

static void check_relief_and_puddles(void) {
    BTBHGPUSceneCover cover;
    memset(&cover, 0, sizeof(cover));
    cover.layers[0] = layer_valid();
    cover.layers[0].amount = 1.0f;
    cover.layers[0].bump = 0.0f;
    BTBHGPUCoverUniforms block;
    btbh_cover_pack(&cover, false, &block);
    const float albedo[3] = {0.1f, 0.2f, 0.3f};
    const float position[3] = {1.3f, 0.0f, 2.9f};
    BTBHCoverSample flat = btbh_cover_sample(&block, 0u, position, 0.01f, up, up, albedo, 0.5f, 0.0f, 1.0f, 0.0f);
    assert(flat.c == 1.0f && flat.normal[1] == 1.0f);
    cover.layers[0].bump = 1.0f;
    btbh_cover_pack(&cover, false, &block);
    float tilted_sum = 0.0f;
    for (int index = 0; index < 64; ++index) {
        const float at[3] = {0.37f * (float)index, 0.0f, -0.29f * (float)index};
        const BTBHCoverSample sample = btbh_cover_sample(&block, 0u, at, 0.01f, up, up, albedo, 0.5f, 0.0f, 1.0f,
            0.0f);
        const float length = sqrtf(sample.normal[0] * sample.normal[0] + sample.normal[1] * sample.normal[1] +
            sample.normal[2] * sample.normal[2]);
        assert(fabsf(length - 1.0f) < 1.0e-4f && sample.normal[1] > 0.7f);
        tilted_sum += 1.0f - sample.normal[1];
    }
    assert(tilted_sum > 0.01f);
    /* MUD's puddles: wet ones glossier, none below the puddle's roughness, the dry layer's roughness elsewhere. */
    cover.layers[0].pattern = BTBH_GPU_SCENE_COVER_MUD;
    btbh_cover_pack(&cover, false, &block);
    float lowest_dry = 1.0f;
    float lowest_wet = 1.0f;
    float highest = 0.0f;
    for (int index = 0; index < 400; ++index) {
        const float at[3] = {0.113f * (float)index, 0.0f, 7.0f - 0.071f * (float)index};
        const BTBHCoverSample dry = btbh_cover_sample(&block, 0u, at, 0.01f, up, up, albedo, 0.5f, 0.0f, 1.0f, 0.0f);
        const BTBHCoverSample wet = btbh_cover_sample(&block, 0u, at, 0.01f, up, up, albedo, 0.5f, 0.0f, 1.0f, 1.0f);
        assert(wet.roughness <= dry.roughness + 1.0e-6f);
        assert(wet.roughness >= BTBH_COVER_PUDDLE_ROUGHNESS - 1.0e-6f);
        lowest_dry = dry.roughness < lowest_dry ? dry.roughness : lowest_dry;
        lowest_wet = wet.roughness < lowest_wet ? wet.roughness : lowest_wet;
        highest = dry.roughness > highest ? dry.roughness : highest;
    }
    printf("MUD's roughness: dry %.3f..%.3f, wet down to %.3f\n", (double)lowest_dry, (double)highest,
        (double)lowest_wet);
    assert(fabsf(highest - BTBH_COVER_MUD_ROUGHNESS) < 1.0e-4f && lowest_wet < lowest_dry && lowest_wet < 0.2f);
    /* Without noise (edge_noise_degrees 0) MUD has no puddles: the layer's roughness, wet or dry. */
    cover.layers[0].edge_noise_degrees = 0.0f;
    btbh_cover_pack(&cover, false, &block);
    for (int index = 0; index < 40; ++index) {
        const float at[3] = {0.113f * (float)index, 0.0f, 7.0f - 0.071f * (float)index};
        const BTBHCoverSample wet = btbh_cover_sample(&block, 0u, at, 0.01f, up, up, albedo, 0.5f, 0.0f, 1.0f, 1.0f);
        assert(wet.roughness == BTBH_COVER_MUD_ROUGHNESS);
    }
    printf("relief: flat without bump, tilted and unit with it; MUD's puddles glossier when wet, none without noise\n");
}

/* The mean over a grid of points of a layer's coverage (and, with `roughness`, of its roughness) at a slope (degrees)
   with a pixel footprint (world units). */
static double mean_cover(const BTBHGPUCoverUniforms *block, float slope_degrees, float pixel, bool roughness) {
    const float angle = slope_degrees * BTBH_COVER_PI / 180.0f;
    const float normal[3] = {0.0f, cosf(angle), sinf(angle)};
    const float albedo[3] = {0.1f, 0.2f, 0.3f};
    double sum = 0.0;
    for (int y = 0; y < 150; ++y) {
        for (int x = 0; x < 150; ++x) {
            const float at[3] = {-400.0f + 0.731f * (float)x + 0.0173f * (float)y, 2.0f,
                250.0f + 0.677f * (float)y + 0.0119f * (float)x};
            const BTBHCoverSample sample = btbh_cover_sample(block, 0u, at, pixel, normal, normal, albedo, 0.5f,
                0.0f, 1.0f, 1.0f);
            sum += roughness ? sample.roughness : sample.c;
        }
    }
    return sum / (150.0 * 150.0);
}

/* Where the noise's cells shrink under a pixel, a face keeps the coverage it averages to near by (a thin cover's far
   level faces as patchy as its near ones, not solid), and MUD's puddles their share of the gloss. */
static void check_far_coverage(void) {
    BTBHGPUSceneCover cover;
    memset(&cover, 0, sizeof(cover));
    cover.layers[0] = layer_valid();
    cover.layers[0].amount = 0.1f;
    cover.layers[0].slope_degrees = 60.0f;
    cover.layers[0].edge_degrees = 8.0f;
    cover.layers[0].edge_noise_degrees = 14.0f;
    cover.layers[0].noise_scale = 1.0f;
    cover.layers[0].bump = 0.0f;
    double worst = 0.0;
    double worst_old = 0.0;
    for (uint32_t pattern = 0u; pattern < 3u; ++pattern) {
        cover.layers[0].pattern = (uint8_t)pattern;
        for (int coarse = 0; coarse < 2; ++coarse) {
            cover.layers[0].flags = coarse != 0 ? (uint8_t)BTBH_GPU_SCENE_COVER_LAYER_COARSE : 0u;
            BTBHGPUCoverUniforms block;
            btbh_cover_pack(&cover, false, &block);
            for (float slope = 0.0f; slope <= 9.5f; slope += 3.0f) {
                const double near = mean_cover(&block, slope, 0.01f, false);
                /* Every footprint from resolved to faded out: within a few hundredths of the near mean. */
                for (float pixel = 0.1f; pixel <= 1.05f; pixel += 0.1f) {
                    const double far = mean_cover(&block, slope, pixel, false);
                    const double off = fabs(far - near);
                    worst = off > worst ? off : worst;
                }
                /* What the noise-free line would give (0.18.0's first build far away). */
                const float x = (block.facing[0][0] - slope * BTBH_COVER_PI / 180.0f) * block.facing[0][1];
                const double old = btbh_cover_edge(pattern, x, 0.0f);
                worst_old = fabs(old - near) > worst_old ? fabs(old - near) : worst_old;
            }
        }
    }
    printf("far coverage: within %.3f of the near mean at every footprint (the noise-free line: %.3f off)\n", worst,
        worst_old);
    assert(worst < 0.06 && worst_old > 0.2);
    /* MUD's puddles, wet, on level ground fully covered: the far roughness is the near mean's. */
    cover.layers[0] = layer_valid();
    cover.layers[0].amount = 1.0f;
    cover.layers[0].bump = 0.0f;
    cover.layers[0].pattern = BTBH_GPU_SCENE_COVER_MUD;
    BTBHGPUCoverUniforms block;
    btbh_cover_pack(&cover, false, &block);
    const double near = mean_cover(&block, 0.0f, 0.01f, true);
    double puddle_worst = 0.0;
    for (float pixel = 0.05f; pixel <= 1.0f; pixel += 0.05f) {
        const double off = fabs(mean_cover(&block, 0.0f, pixel, true) - near);
        puddle_worst = off > puddle_worst ? off : puddle_worst;
    }
    const double old = BTBH_COVER_MUD_ROUGHNESS + (BTBH_COVER_PUDDLE_ROUGHNESS - BTBH_COVER_MUD_ROUGHNESS) *
        (1.0 - btbh_cover_smoothstep(-0.4f, 0.2f, 0.0f));
    printf("far puddles: MUD's wet roughness %.3f near, within %.3f at every footprint (N = 0's: %.3f)\n", near,
        puddle_worst, old);
    assert(puddle_worst < 0.02 && fabs(old - near) > 0.03);
}

static void check_ice(void) {
    BTBHGPUSceneGeometryBatch batch;
    memset(&batch, 0, sizeof(batch));
    batch.surface_variant = BTBH_GPU_SCENE_SURFACE_ICE;
    assert(btbh_ice_parameters_problem(&batch) == NULL);
    batch.surface_parameters0[1] = -0.1f;
    assert(btbh_ice_parameters_problem(&batch) != NULL);
    batch.surface_parameters0[1] = NAN;
    assert(btbh_ice_parameters_problem(&batch) != NULL);
    batch.surface_parameters0[1] = 0.0f;
    batch.surface_parameters1[0] = 1.5f;
    assert(btbh_ice_parameters_problem(&batch) != NULL);
    batch.surface_parameters1[0] = 1.0f;
    batch.surface_parameters1[3] = 1.2f;
    assert(btbh_ice_parameters_problem(&batch) != NULL);
    batch.surface_parameters1[3] = 1.0f;
    batch.surface_parameters0[2] = 3.0f;
    batch.surface_parameters1[1] = 40.0f;
    assert(btbh_ice_parameters_problem(&batch) == NULL);
    const float zero[4] = {0.0f, 0.0f, 0.0f, 0.0f};
    float p0[4];
    float p1[4];
    btbh_ice_defaults(zero, zero, p0, p1);
    assert(p0[0] == 1.2f && p0[1] == 0.025f && p0[2] == 0.5f && p0[3] == 0.035f);
    assert(p1[0] == 0.0f && p1[1] == 3.0f && p1[2] == 0.06f && p1[3] == 0.6f);
    /* The border distance: never negative; near 0 somewhere on a dense line, up to about half a plate inside. */
    float lowest = 1.0e9f;
    float highest = 0.0f;
    uint32_t cracked = 0u;
    uint32_t samples = 0u;
    for (int index = 0; index < 4000; ++index) {
        const float x = -8.0f + 0.004f * (float)index;
        const float z = 2.0f + 0.0013f * (float)index;
        const BTBHIceSample ice = btbh_ice_sample(x, z, 0.01f, p0, p1);
        assert(ice.crack_distance >= 0.0f);
        assert(ice.lift_share >= 0.0f && ice.lift_share <= 1.0f);
        assert(ice.roughness >= p1[2] - 1.0e-6f && ice.roughness <= p1[3] + 1.0e-6f);
        lowest = ice.crack_distance < lowest ? ice.crack_distance : lowest;
        highest = ice.crack_distance > highest ? ice.crack_distance : highest;
        cracked += ice.lift_share > 0.5f ? 1u : 0u;
        samples += 1u;
    }
    printf("ice: border distance %.4f..%.3f world units, %.1f %% of a line cracked\n", (double)lowest,
        (double)highest, 100.0 * cracked / samples);
    assert(lowest < 0.01f && highest > 0.2f && highest < 1.2f && cracked > 0u && cracked < samples / 4u);
    /* The border distance is the nearest border's, near a corner where three plates meet too: against every border
       of the nearest seed with the seeds of the 7 x 7 cells round it, at points within 0.3 cells of one. */
    uint32_t near_border = 0u;
    float worst_border = 0.0f;
    for (int index = 0; index < 60000; ++index) {
        const float px = -300.0f + 0.01731f * (float)index;
        const float py = 120.0f + 0.00947f * (float)index + 0.37f * (float)(index % 97);
        float across[2];
        float cell[2];
        const float distance = btbh_ice_voronoi(px, py, across, cell);
        float nearest[2];
        btbh_ice_seed(cell[0], cell[1], nearest);
        float exact = 1.0e9f;
        for (int y = -3; y <= 3; ++y) {
            for (int x = -3; x <= 3; ++x) {
                if (x == 0 && y == 0) {
                    continue;
                }
                float seed[2];
                btbh_ice_seed(cell[0] + (float)x, cell[1] + (float)y, seed);
                const float lx = seed[0] - nearest[0];
                const float ly = seed[1] - nearest[1];
                const float length = sqrtf(lx * lx + ly * ly);
                const float border = ((0.5f * (nearest[0] + seed[0]) - px) * lx +
                    (0.5f * (nearest[1] + seed[1]) - py) * ly) / length;
                exact = border < exact ? border : exact;
            }
        }
        if (exact < 0.3f) {
            near_border += 1u;
            const float off = fabsf(distance - (exact > 0.0f ? exact : 0.0f));
            worst_border = off > worst_border ? off : worst_border;
        }
    }
    printf("ice: the border distance at %u points within 0.3 cells of a border, off the exact one by %.2g at most\n",
        near_border, (double)worst_border);
    assert(near_border > 10000u && worst_border < 1.0e-4f);
    /* The craze fades as a pixel grows from its width to twice it: no step at the cutoff. */
    float worst_step = 0.0f;
    float lifted_before = 0.0f;
    for (int index = 0; index < 3000; ++index) {
        const float x = 3.0f + 0.0071f * (float)index;
        const float z = -4.0f + 0.0043f * (float)index;
        float previous = -1.0f;
        const float fine_width = 0.5f * p0[1];
        for (int step = 0; step <= 240; ++step) {
            const float footprint = fine_width * (0.5f + 2.5f * (float)step / 240.0f);
            const BTBHIceSample ice = btbh_ice_sample(x, z, footprint, p0, p1);
            if (previous >= 0.0f) {
                const float jump = fabsf(ice.lift_share - previous);
                worst_step = jump > worst_step ? jump : worst_step;
            }
            if (step == 96) {
                lifted_before += ice.lift_share;
            }
            previous = ice.lift_share;
        }
    }
    printf("ice: the craze over footprints 0.5 to 3 times its width, the largest step %.4f (of 240)\n",
        (double)worst_step);
    assert(worst_step < 0.02f && lifted_before > 0.0f);
    /* A crack is 1 on its line while it is wider than half a pixel, and loses strength with its share of a pixel. */
    assert(btbh_ice_crack(0.0f, 0.05f, 0.01f) == 1.0f);
    assert(btbh_ice_crack(0.05f, 0.05f, 0.01f) == 0.0f);
    assert(fabsf(btbh_ice_crack(0.0f, 0.01f, 0.08f) - 0.25f) < 1.0e-6f);
    /* Frost covers about its share of the surface, no frost none of it. */
    p1[0] = 0.0f;
    uint32_t frosted_none = 0u;
    uint32_t frosted_half = 0u;
    uint32_t points = 0u;
    for (int y = 0; y < 120; ++y) {
        for (int x = 0; x < 120; ++x) {
            const float wx = 0.37f * (float)x - 11.0f;
            const float wz = 0.41f * (float)y + 5.0f;
            p1[0] = 0.0f;
            const BTBHIceSample bare = btbh_ice_sample(wx, wz, 0.01f, p0, p1);
            p1[0] = 0.5f;
            const BTBHIceSample frost = btbh_ice_sample(wx, wz, 0.01f, p0, p1);
            frosted_none += bare.lift_share > 0.5f && bare.crack_distance > p0[1] ? 1u : 0u;
            frosted_half += frost.lift_share > 0.5f ? 1u : 0u;
            assert(frost.lift_share >= bare.lift_share);
            points += 1u;
        }
    }
    /* (Without frost only the hairline craze lifts points off the main cracks.) */
    printf("ice: past half lifted off the main cracks: %u of %u points without frost (the craze), %u with frost 0.5\n",
        frosted_none, points, frosted_half);
    assert(frosted_none < points / 20u && frosted_half > frosted_none + points / 4u &&
        frosted_half < (points * 4u) / 5u);
    printf("ice: refusals, zero values, the plates' borders, the cracks' fade, the frost's share, the roughness\n");
}

int main(void) {
    (void)setvbuf(stdout, NULL, _IONBF, 0);
    check_refusals();
    check_block();
    check_facing();
    check_noise();
    check_relief_and_puddles();
    check_far_coverage();
    check_ice();
    printf("btbh_gpu_cover_test: ok\n");
    return 0;
}
