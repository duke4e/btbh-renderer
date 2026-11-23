/* RFC 0013 phase 1 without a GPU: the C copies of the lit particle's light and fog (src/gpu/btbh_gpu_particle.h, kept in
 * step with shaders/gpu/production_particle_lit.frag.hlsl), the refusals of a lit particle and of the UV rect, and the
 * frame block's layout.
 *
 * - The phase (Henyey-Greenstein times 4 pi): 1 at every angle for g = 0; its mean over the sphere 1 for every g;
 *   forward (g > 0) brighter looking toward the light; g clamped to -0.99..0.99.
 * - The wrap: Lambert at 0, half lit at the terminator for w = 1, 0 at the back for every w up to 1.
 * - The transmission: x at thickness 0, x exp(-z) at thickness 1, none for x or z below 0.
 * - The fog's make-up: for random puffs, backgrounds, coverages and fog amounts (own above, at and below behind),
 *   the composite's lerp(pixel, fog, behind) of the made-up colour blended over the background gives the puff fogged
 *   at its own depth over the background fogged at its own; for stacks of up to six layers too.
 * - The fog's amount: the composite's formula, its cap and its edge fog.
 * - Every refusal, and a batch each rule lets through.
 * - The light factor's (BTBH_GPU_SCENE_BATCH_INSTANCE_LIGHT_FACTOR, 0.11.0) refusals, and what it lets through.
 * - RFC 0013 phase 2 (0.14.0): the particle shadow's refusals and what it lets through; particle_shadow's values; the
 *   map's size; the optical depth a fragment adds (its density, its cutoffs); a slab's transmittance before, inside
 *   and past it, for a single layer, a filtered column edge, and random columns of even layers against the share of
 *   their summed optical depth before the receiver; the empty texel. */
#undef NDEBUG

#include "btbh_gpu_particle.h"

#include <assert.h>
#include <math.h>
#include <stddef.h>
#include <stdio.h>
#include <string.h>

static int checks;

static void expect_near(double actual, double expected, double tolerance, const char *what) {
    if (!(fabs(actual - expected) <= tolerance)) {
        fprintf(stderr, "%s: %.9f, expected %.9f\n", what, actual, expected);
    }
    assert(fabs(actual - expected) <= tolerance);
    checks += 1;
}

static void expect_problem(const char *problem, const char *needle, const char *what) {
    if (problem == NULL || strstr(problem, needle) == NULL) {
        fprintf(stderr, "%s: \"%s\", expected one with \"%s\"\n", what, problem != NULL ? problem : "(none)", needle);
    }
    assert(problem != NULL && strstr(problem, needle) != NULL);
    checks += 1;
}

static uint32_t random_state = 12345u;

static float random_unit(void) {
    random_state = random_state * 1664525u + 1013904223u;
    return (float)(random_state >> 8u) / 16777216.0f;
}

/* The mean of the phase over the sphere: half its integral over cos theta from -1 to 1 (Simpson, 20000 steps). */
static double phase_mean(float g) {
    const int steps = 20000;
    double sum = 0.0;
    for (int i = 0; i <= steps; ++i) {
        const double mu = -1.0 + 2.0 * (double)i / (double)steps;
        const double weight = (i == 0 || i == steps) ? 1.0 : ((i % 2) != 0 ? 4.0 : 2.0);
        sum += weight * (double)btbh_gpu_particle_phase(g, (float)mu);
    }
    return 0.5 * sum * (2.0 / (double)steps) / 3.0;
}

static void check_phase(void) {
    for (int i = 0; i <= 20; ++i) {
        const float mu = -1.0f + 0.1f * (float)i;
        expect_near(btbh_gpu_particle_phase(0.0f, mu), 1.0, 1.0e-6, "phase at g = 0");
    }
    static const float gs[] = { -0.9f, -0.6f, -0.3f, 0.1f, 0.3f, 0.6f, 0.8f, 0.9f };
    for (size_t i = 0u; i < sizeof(gs) / sizeof(gs[0]); ++i) {
        expect_near(phase_mean(gs[i]), 1.0, 2.0e-3, "the phase's mean over the sphere");
        if (gs[i] > 0.0f) {
            assert(btbh_gpu_particle_phase(gs[i], 1.0f) > btbh_gpu_particle_phase(gs[i], -1.0f));
        } else {
            assert(btbh_gpu_particle_phase(gs[i], 1.0f) < btbh_gpu_particle_phase(gs[i], -1.0f));
        }
        checks += 1;
    }
    /* Forward at g = 0.6: (1 - g^2) / (1 - g)^3 = 0.64 / 0.064 = 10. */
    expect_near(btbh_gpu_particle_phase(0.6f, 1.0f), 10.0, 1.0e-4, "the forward peak at g = 0.6");
    expect_near(btbh_gpu_particle_phase(0.6f, -1.0f), 0.64 / (1.6 * 1.6 * 1.6), 1.0e-6, "backward at g = 0.6");
    expect_near(btbh_gpu_particle_phase(1.5f, 0.3f), btbh_gpu_particle_phase(0.99f, 0.3f), 0.0, "g clamped above");
    expect_near(btbh_gpu_particle_phase(-3.0f, 0.3f), btbh_gpu_particle_phase(-0.99f, 0.3f), 0.0, "g clamped below");
    assert(isfinite(btbh_gpu_particle_phase(0.99f, 1.0f)));
    checks += 1;
}

static void check_wrap(void) {
    for (int i = 0; i <= 20; ++i) {
        const float ndl = -1.0f + 0.1f * (float)i;
        expect_near(btbh_gpu_particle_wrap(ndl, 0.0f), ndl > 0.0f ? ndl : 0.0f, 1.0e-6, "the wrap at 0 is Lambert");
        expect_near(btbh_gpu_particle_wrap(ndl, -1.0f), ndl > 0.0f ? ndl : 0.0f, 1.0e-6, "a wrap below 0 is 0");
        expect_near(btbh_gpu_particle_wrap(ndl, 1.0f), (ndl + 1.0f) * 0.5f, 1.0e-6, "the wrap at 1");
    }
    expect_near(btbh_gpu_particle_wrap(0.0f, 0.5f), 1.0 / 3.0, 1.0e-6, "the terminator at w = 0.5");
    expect_near(btbh_gpu_particle_wrap(1.0f, 0.7f), 1.0, 1.0e-6, "facing the light");
    expect_near(btbh_gpu_particle_wrap(-1.0f, 0.7f), 0.0, 1.0e-6, "the back at w = 0.7");
    expect_near(btbh_gpu_particle_transmission(0.8f, 0.0f, 3.0f), 0.8, 1.0e-7, "transmission at thickness 0");
    expect_near(btbh_gpu_particle_transmission(0.8f, 1.0f, 3.0f), 0.8 * exp(-3.0), 1.0e-7, "at thickness 1");
    expect_near(btbh_gpu_particle_transmission(-0.8f, 0.5f, 3.0f), 0.0, 0.0, "a transmission below 0");
    expect_near(btbh_gpu_particle_transmission(0.8f, 0.5f, -3.0f), 0.8, 1.0e-7, "an optical depth below 0");
}

/* What the composite leaves: lerp(pixel, fog, behind). */
static double composite(double pixel, double fog, double behind) {
    return pixel + (fog - pixel) * behind;
}

static void check_fog_makeup(void) {
    for (int trial = 0; trial < 20000; ++trial) {
        const int layers = 1 + trial % 6;
        const float fog = random_unit();
        float behind = 0.98f * random_unit();
        if (trial % 7 == 0) {
            behind = 0.0f;   /* over the background: the composite adds none */
        }
        const float background = 3.0f * random_unit();
        /* What the pixel holds before the composite, and what it should come to. */
        double pixel = background;
        double expected = composite(background, fog, behind);
        for (int layer = 0; layer < layers; ++layer) {
            const float colour = 4.0f * random_unit();
            const float alpha = random_unit();
            float own = random_unit() * 0.98f;
            if (trial % 5 == 0) {
                own = behind;
            }
            const float written = btbh_gpu_particle_fog_makeup(colour, fog, own, behind);
            pixel = (double)written * alpha + pixel * (1.0 - alpha);
            expected = ((double)colour * (1.0 - own) + (double)fog * own) * alpha + expected * (1.0 - alpha);
        }
        expect_near(composite(pixel, fog, behind), expected, 2.0e-4 * (1.0 + fabs(expected)) / (1.0 - behind),
            "a stack of puffs made up for the composite's fog");
    }
    /* own == behind writes the colour as it is; over the background (behind 0) it is the colour fogged. */
    expect_near(btbh_gpu_particle_fog_makeup(0.7f, 0.2f, 0.3f, 0.3f), 0.7, 1.0e-6, "own as behind");
    expect_near(btbh_gpu_particle_fog_makeup(0.7f, 0.2f, 0.3f, 0.0f), 0.7 * 0.7 + 0.2 * 0.3, 1.0e-6, "over the sky");
    /* A dark puff over far, fogged ground goes below 0 before the composite. */
    assert(btbh_gpu_particle_fog_makeup(0.0f, 1.0f, 0.1f, 0.5f) < 0.0f);
    checks += 1;
}

static void check_fog_amount(void) {
    BTBHGPUScenePost post;
    memset(&post, 0, sizeof(post));
    post.fog_enabled = 1u;
    post.fog_start_distance = 30.0f;
    post.fog_density = 0.036f;
    post.fog_maximum_opacity = 0.5f;
    expect_near(btbh_gpu_particle_fog_amount(&post, 20.0f, 0.0f, 0.0f), 0.0, 0.0, "before the start");
    const double optical = (45.0 - 30.0) * 0.036;
    expect_near(btbh_gpu_particle_fog_amount(&post, 45.0f, 0.0f, 0.0f), 1.0 - exp(-optical * optical), 1.0e-6,
        "past the start");
    expect_near(btbh_gpu_particle_fog_amount(&post, 400.0f, 0.0f, 0.0f), 0.5, 1.0e-7, "capped");
    post.fog_edge_enabled = 1u;
    post.fog_edge_bounds[0] = -50.0f;
    post.fog_edge_bounds[1] = -50.0f;
    post.fog_edge_bounds[2] = 50.0f;
    post.fog_edge_bounds[3] = 50.0f;
    post.fog_edge_fade_distance = 10.0f;
    post.fog_edge_clear_distance = 5.0f;
    expect_near(btbh_gpu_particle_fog_amount(&post, 45.0f, 0.0f, 0.0f), 1.0 - exp(-optical * optical), 1.0e-6,
        "inside the edge");
    expect_near(btbh_gpu_particle_fog_amount(&post, 45.0f, 55.0f, 0.0f), 1.0, 1.0e-6, "past the edge");
    expect_near(btbh_gpu_particle_fog_amount(&post, 45.0f, 45.0f, 0.0f), 0.5, 1.0e-6, "half way in the edge");
    expect_near(btbh_gpu_particle_fog_amount(&post, 2.0f, 55.0f, 0.0f), 0.0, 1.0e-6, "clear near the camera");
}

static BTBHGPUSceneGeometryBatch lit_batch(void) {
    BTBHGPUSceneGeometryBatch batch;
    memset(&batch, 0, sizeof(batch));
    batch.surface_variant = BTBH_GPU_SCENE_SURFACE_LIT_PARTICLE;
    batch.transparency_enabled = 1u;
    batch.blend_mode = BTBH_GPU_SCENE_BLEND_ALPHA;
    return batch;
}

static void check_refusals(void) {
    BTBHGPUSceneGeometryBatch batch = lit_batch();
    assert(btbh_gpu_particle_lit(&batch) && btbh_gpu_particle_lit_problem(&batch) == NULL);
    checks += 1;
    batch.transparency_enabled = 0u;
    expect_problem(btbh_gpu_particle_lit_problem(&batch), "transparency_enabled", "opaque");
    batch = lit_batch();
    batch.blend_mode = BTBH_GPU_SCENE_BLEND_PREMULTIPLIED_ALPHA;
    expect_problem(btbh_gpu_particle_lit_problem(&batch), "BTBH_GPU_SCENE_BLEND_ALPHA", "premultiplied");
    batch.blend_mode = BTBH_GPU_SCENE_BLEND_ADDITIVE;
    expect_problem(btbh_gpu_particle_lit_problem(&batch), "BTBH_GPU_SCENE_BLEND_ALPHA", "additive");
    batch = lit_batch();
    batch.unlit_enabled = 3u;
    expect_problem(btbh_gpu_particle_lit_problem(&batch), "unlit_enabled", "unlit");
    batch = lit_batch();
    static const int stand_in = 0;
    batch.animation_pose = (BTBHGPUAnimationPose *)(uintptr_t)&stand_in;
    expect_problem(btbh_gpu_particle_lit_problem(&batch), "static", "posed");
    batch = lit_batch();
    batch.animation_library = (BTBHGPUAnimationLibrary *)(uintptr_t)&stand_in;
    expect_problem(btbh_gpu_particle_lit_problem(&batch), "static", "animated");
    batch = lit_batch();
    batch.dug_by = (const BTBHGPUSdfTerrain *)(const void *)&stand_in;
    expect_problem(btbh_gpu_particle_lit_problem(&batch), "dug", "dug");
    BTBHGPUSceneWorldDetail detail;
    memset(&detail, 0, sizeof(detail));
    batch = lit_batch();
    batch.world_detail = &detail;
    assert(btbh_gpu_particle_lit_problem(&batch) == NULL);   /* a detail without textures draws as none */
    detail.orm_texture = (BTBHGPUTexture *)(uintptr_t)&stand_in;
    expect_problem(btbh_gpu_particle_lit_problem(&batch), "world_detail", "a world detail");
    BTBHGPUSceneWorldDetailTable table;
    memset(&table, 0, sizeof(table));
    batch = lit_batch();
    batch.world_detail_table = &table;
    assert(btbh_gpu_particle_lit_problem(&batch) == NULL);
    table.map_layers = (BTBHGPUTexture *)(uintptr_t)&stand_in;
    expect_problem(btbh_gpu_particle_lit_problem(&batch), "map_layers", "a table");
    BTBHGPUSceneHeightLift lift;
    memset(&lift, 0, sizeof(lift));
    batch = lit_batch();
    batch.height_lift = &lift;
    assert(btbh_gpu_particle_lit_problem(&batch) == NULL);
    lift.heightfield = (BTBHGPUTexture *)(uintptr_t)&stand_in;
    expect_problem(btbh_gpu_particle_lit_problem(&batch), "lifted", "a lift");
    for (int component = 0; component < 4; ++component) {
        batch = lit_batch();
        batch.detail_parameters[component] = 1.0f;
        expect_problem(btbh_gpu_particle_lit_problem(&batch), "detail_parameters", "a procedural detail");
    }
    /* surface_parameters1[3] is the particle shadow's density since 0.14.0 (refused unless 0 before): 0 or more. */
    batch = lit_batch();
    batch.surface_parameters1[3] = 0.5f;
    assert(btbh_gpu_particle_lit_problem(&batch) == NULL);
    checks += 1;
    batch.surface_parameters1[3] = -0.5f;
    expect_problem(btbh_gpu_particle_lit_problem(&batch), "surface_parameters1[3]", "a negative density");
    /* The others are free: any fades, wrap, transmission, asymmetry, optical depth, a billboard, the UV rect. */
    batch = lit_batch();
    batch.surface_parameters0[0] = 0.4f;
    batch.surface_parameters0[1] = 0.5f;
    batch.surface_parameters0[2] = 1.0f;
    batch.surface_parameters0[3] = 0.5f;
    batch.surface_parameters1[0] = 0.8f;
    batch.surface_parameters1[1] = -2.0f;
    batch.surface_parameters1[2] = 9.0f;
    batch.billboard_mode = BTBH_GPU_SCENE_BILLBOARD_FRONT;
    batch.flags = BTBH_GPU_SCENE_BATCH_INSTANCE_UV_RECT;
    assert(btbh_gpu_particle_lit_problem(&batch) == NULL && btbh_gpu_particle_uv_rect_problem(&batch) == NULL);
    checks += 1;

    /* The UV rect. */
    BTBHGPUSceneGeometryBatch rect;
    memset(&rect, 0, sizeof(rect));
    rect.flags = BTBH_GPU_SCENE_BATCH_INSTANCE_UV_RECT;
    assert(btbh_gpu_particle_uv_rect(&rect) && btbh_gpu_particle_uv_rect_problem(&rect) == NULL);
    checks += 1;
    rect.animation_library = (BTBHGPUAnimationLibrary *)(uintptr_t)&stand_in;
    expect_problem(btbh_gpu_particle_uv_rect_problem(&rect), "static", "an animated rect");
    rect.animation_library = NULL;
    static const BTBHGPUSceneSurfaceVariant readers[3] = {
        BTBH_GPU_SCENE_SURFACE_DISSOLVE, BTBH_GPU_SCENE_SURFACE_SHATTER, BTBH_GPU_SCENE_SURFACE_GLITTER,
    };
    for (int reader = 0; reader < 3; ++reader) {
        rect.surface_variant = readers[reader];
        expect_problem(btbh_gpu_particle_uv_rect_problem(&rect), "custom floats", "a variant reading custom");
    }
    static const BTBHGPUSceneSurfaceVariant others[] = {
        BTBH_GPU_SCENE_SURFACE_NONE, BTBH_GPU_SCENE_SURFACE_GLITTER_STATIC, BTBH_GPU_SCENE_SURFACE_WATER,
        BTBH_GPU_SCENE_SURFACE_SOFT_PARTICLE, BTBH_GPU_SCENE_SURFACE_RIM, BTBH_GPU_SCENE_SURFACE_GLINTS,
        BTBH_GPU_SCENE_SURFACE_PIXEL_WATER, BTBH_GPU_SCENE_SURFACE_REFRACT, BTBH_GPU_SCENE_SURFACE_LIT_PARTICLE,
    };
    for (size_t other = 0u; other < sizeof(others) / sizeof(others[0]); ++other) {
        rect.surface_variant = others[other];
        assert(btbh_gpu_particle_uv_rect_problem(&rect) == NULL);
        checks += 1;
    }
    rect.surface_variant = BTBH_GPU_SCENE_SURFACE_NONE;
    memset(&detail, 0, sizeof(detail));
    rect.world_detail = &detail;
    assert(btbh_gpu_particle_uv_rect_problem(&rect) == NULL);
    detail.seed_component = 2u;
    expect_problem(btbh_gpu_particle_uv_rect_problem(&rect), "world_detail's seed_component", "a seeded detail");
    rect.world_detail = NULL;
    memset(&table, 0, sizeof(table));
    rect.world_detail_table = &table;
    assert(btbh_gpu_particle_uv_rect_problem(&rect) == NULL);
    table.seed_component = 4u;
    expect_problem(btbh_gpu_particle_uv_rect_problem(&rect), "world_detail_table's seed_component", "a seeded table");

    /* The light factor (0.11.0): custom x. Not with the rect (which reports nothing of it: the factor's check does),
       a lit particle, a refracting batch, DISSOLVE or GLITTER, a detail or table seeded by x. Skinned batches, SHATTER
       (y, z, w), the other variants and seeds 2..4 (v2's world detail seed is custom w) are free. */
    BTBHGPUSceneGeometryBatch light;
    memset(&light, 0, sizeof(light));
    assert(!btbh_gpu_scene_light_factor(&light));
    light.flags = BTBH_GPU_SCENE_BATCH_INSTANCE_LIGHT_FACTOR;
    assert(btbh_gpu_scene_light_factor(&light) && btbh_gpu_scene_light_factor_problem(&light) == NULL);
    checks += 1;
    light.flags |= BTBH_GPU_SCENE_BATCH_INSTANCE_UV_RECT;
    expect_problem(btbh_gpu_scene_light_factor_problem(&light), "INSTANCE_UV_RECT", "the light factor and the rect");
    assert(btbh_gpu_particle_uv_rect_problem(&light) == NULL);
    checks += 1;
    light.flags = BTBH_GPU_SCENE_BATCH_INSTANCE_LIGHT_FACTOR;
    light.surface_variant = BTBH_GPU_SCENE_SURFACE_LIT_PARTICLE;
    expect_problem(btbh_gpu_scene_light_factor_problem(&light), "lit particle", "a lit particle's light factor");
    light.surface_variant = BTBH_GPU_SCENE_SURFACE_REFRACT;
    expect_problem(btbh_gpu_scene_light_factor_problem(&light), "refracting", "a refracting batch's light factor");
    light.surface_variant = BTBH_GPU_SCENE_SURFACE_DISSOLVE;
    expect_problem(btbh_gpu_scene_light_factor_problem(&light), "custom x", "DISSOLVE's light factor");
    light.surface_variant = BTBH_GPU_SCENE_SURFACE_GLITTER;
    expect_problem(btbh_gpu_scene_light_factor_problem(&light), "custom x", "GLITTER's light factor");
    static const BTBHGPUSceneSurfaceVariant lit_by_factor[] = {
        BTBH_GPU_SCENE_SURFACE_NONE, BTBH_GPU_SCENE_SURFACE_GLITTER_STATIC, BTBH_GPU_SCENE_SURFACE_WATER,
        BTBH_GPU_SCENE_SURFACE_SHATTER, BTBH_GPU_SCENE_SURFACE_SOFT_PARTICLE, BTBH_GPU_SCENE_SURFACE_RIM,
        BTBH_GPU_SCENE_SURFACE_GLINTS, BTBH_GPU_SCENE_SURFACE_PIXEL_WATER,
    };
    for (size_t variant = 0u; variant < sizeof(lit_by_factor) / sizeof(lit_by_factor[0]); ++variant) {
        light.surface_variant = lit_by_factor[variant];
        assert(btbh_gpu_scene_light_factor_problem(&light) == NULL);
        checks += 1;
    }
    light.surface_variant = BTBH_GPU_SCENE_SURFACE_NONE;
    light.animation_library = (BTBHGPUAnimationLibrary *)(uintptr_t)&stand_in;
    assert(btbh_gpu_scene_light_factor_problem(&light) == NULL);
    checks += 1;
    light.animation_library = NULL;
    memset(&detail, 0, sizeof(detail));
    light.world_detail = &detail;
    for (uint8_t seed = 0u; seed <= 4u; ++seed) {
        detail.seed_component = seed;
        if (seed == 1u) {
            expect_problem(btbh_gpu_scene_light_factor_problem(&light), "world_detail's seed_component 1",
                "a detail seeded by custom x");
        } else {
            assert(btbh_gpu_scene_light_factor_problem(&light) == NULL);
            checks += 1;
        }
    }
    light.world_detail = NULL;
    memset(&table, 0, sizeof(table));
    light.world_detail_table = &table;
    for (uint8_t seed = 0u; seed <= 4u; ++seed) {
        table.seed_component = seed;
        if (seed == 1u) {
            expect_problem(btbh_gpu_scene_light_factor_problem(&light), "world_detail_table's seed_component 1",
                "a table seeded by custom x");
        } else {
            assert(btbh_gpu_scene_light_factor_problem(&light) == NULL);
            checks += 1;
        }
    }
}

/* RFC 0013 phase 2: what may draw into the particle transmittance map, the lighting's byte and the map's size. */
static void check_particle_shadow_rules(void) {
    static const int stand_in = 0;
    BTBHGPUSceneGeometryBatch batch = lit_batch();
    batch.flags = BTBH_GPU_SCENE_BATCH_PARTICLE_SHADOW;
    assert(btbh_gpu_particle_shadow(&batch) && btbh_gpu_particle_shadow_problem(&batch) == NULL);
    checks += 1;
    /* NONE and SOFT_PARTICLE, see-through by transparency_enabled or by their blend, may too. */
    static const BTBHGPUSceneSurfaceVariant allowed[3] = {
        BTBH_GPU_SCENE_SURFACE_NONE, BTBH_GPU_SCENE_SURFACE_SOFT_PARTICLE, BTBH_GPU_SCENE_SURFACE_LIT_PARTICLE,
    };
    for (int variant = 0; variant < 3; ++variant) {
        batch.surface_variant = allowed[variant];
        batch.transparency_enabled = 1u;
        batch.blend_mode = BTBH_GPU_SCENE_BLEND_ALPHA;
        assert(btbh_gpu_particle_shadow_problem(&batch) == NULL);
        batch.transparency_enabled = 0u;
        batch.blend_mode = BTBH_GPU_SCENE_BLEND_ADDITIVE;
        assert(btbh_gpu_particle_shadow_problem(&batch) == NULL);
        batch.blend_mode = BTBH_GPU_SCENE_BLEND_ALPHA;
        expect_problem(btbh_gpu_particle_shadow_problem(&batch), "transparency_enabled", "opaque");
        checks += 2;
    }
    for (int variant = 0; variant < BTBH_GPU_SCENE_SURFACE_COUNT; ++variant) {
        batch = lit_batch();
        batch.surface_variant = (BTBHGPUSceneSurfaceVariant)variant;
        const bool takes = variant == BTBH_GPU_SCENE_SURFACE_NONE || variant == BTBH_GPU_SCENE_SURFACE_SOFT_PARTICLE ||
            variant == BTBH_GPU_SCENE_SURFACE_LIT_PARTICLE;
        if (takes) {
            assert(btbh_gpu_particle_shadow_problem(&batch) == NULL);
            checks += 1;
        } else {
            expect_problem(btbh_gpu_particle_shadow_problem(&batch), "surface variant", "another variant");
        }
    }
    batch = lit_batch();
    batch.animation_pose = (BTBHGPUAnimationPose *)(uintptr_t)&stand_in;
    expect_problem(btbh_gpu_particle_shadow_problem(&batch), "static", "posed");
    batch = lit_batch();
    batch.animation_library = (BTBHGPUAnimationLibrary *)(uintptr_t)&stand_in;
    expect_problem(btbh_gpu_particle_shadow_problem(&batch), "static", "animated");
    BTBHGPUSceneHeightLift lift;
    memset(&lift, 0, sizeof(lift));
    batch = lit_batch();
    batch.height_lift = &lift;
    assert(btbh_gpu_particle_shadow_problem(&batch) == NULL);
    checks += 1;
    lift.heightfield = (BTBHGPUTexture *)(uintptr_t)&stand_in;
    expect_problem(btbh_gpu_particle_shadow_problem(&batch), "lifted", "lifted");
    batch = lit_batch();
    batch.dug_by = (const BTBHGPUSdfTerrain *)(const void *)&stand_in;
    expect_problem(btbh_gpu_particle_shadow_problem(&batch), "dug", "dug");
    /* The byte: readers 0..2, the quarter bit, nothing else. */
    for (uint32_t value = 0u; value < 256u; ++value) {
        const bool valid = (value & 3u) <= 2u && (value & ~7u) == 0u;
        assert(btbh_gpu_particle_shadow_setting_valid((uint8_t)value) == valid);
        checks += 1;
    }
    /* Half the sun map's size, or a quarter; at least 1. */
    assert(btbh_gpu_particle_shadow_size(2048u, 1u) == 1024u && btbh_gpu_particle_shadow_size(2048u, 6u) == 512u);
    assert(btbh_gpu_particle_shadow_size(1536u, 2u) == 768u && btbh_gpu_particle_shadow_size(1536u, 5u) == 384u);
    assert(btbh_gpu_particle_shadow_size(1024u, 1u) == 512u && btbh_gpu_particle_shadow_size(1024u, 5u) == 256u);
    assert(btbh_gpu_particle_shadow_size(3u, 5u) == 1u && btbh_gpu_particle_shadow_size(1u, 1u) == 1u);
    checks += 4;
}

/* RFC 0013 phase 2: production_particle_transmittance.frag's optical depth and production_particle_shadow.hlsli's
   slab. */
static void check_particle_shadow_slab(void) {
    /* The optical depth: -ln(1 - alpha), alpha capped at 0.999, times the density (0 is 1); nothing at 1/255 or below,
       or below the cutoff. */
    expect_near(btbh_gpu_particle_optical_depth(0.5f, 0.0f, 0.0f), log(2.0), 1.0e-6, "alpha 0.5");
    expect_near(btbh_gpu_particle_optical_depth(0.5f, 2.0f, 0.0f), 2.0 * log(2.0), 1.0e-6, "alpha 0.5, density 2");
    expect_near(btbh_gpu_particle_optical_depth(1.0f, 1.0f, 0.0f), -log(0.001), 1.0e-3, "alpha 1 (capped)");
    expect_near(btbh_gpu_particle_optical_depth(1.0f / 255.0f, 1.0f, 0.0f), 0.0, 0.0, "alpha 1/255");
    expect_near(btbh_gpu_particle_optical_depth(0.3f, 1.0f, 0.4f), 0.0, 0.0, "below the cutoff");
    expect_near(btbh_gpu_particle_optical_depth(0.4f, 1.0f, 0.4f), -log(0.6), 1.0e-6, "at the cutoff");
    /* The empty texel (cleared to 0, 0, 0, 1): all the light everywhere. */
    for (int i = 0; i <= 10; ++i) {
        expect_near(btbh_gpu_particle_slab_transmittance(0.0f, 0.0f, 1.0f, 0.1f * (float)i), 1.0, 0.0, "empty");
    }
    /* One layer at depth 0.4: all of it before, exp(-tau) at and past it. */
    const float tau = 0.7f;
    expect_near(btbh_gpu_particle_slab_transmittance(tau, tau * 0.4f, 0.4f, 0.39f), 1.0, 0.0, "before a layer");
    expect_near(btbh_gpu_particle_slab_transmittance(tau, tau * 0.4f, 0.4f, 0.4f), exp(-0.7), 1.0e-6, "at a layer");
    expect_near(btbh_gpu_particle_slab_transmittance(tau, tau * 0.4f, 0.4f, 0.9f), exp(-0.7), 1.0e-6, "past a layer");
    /* Three equal layers at 0.3, 0.4, 0.5: the slab spans 0.3 to 0.5 (twice the mean less the front). */
    const float r = 3.0f * tau;
    const float g = tau * (0.3f + 0.4f + 0.5f);
    expect_near(btbh_gpu_particle_slab_transmittance(r, g, 0.3f, 0.2f), 1.0, 0.0, "before the column");
    expect_near(btbh_gpu_particle_slab_transmittance(r, g, 0.3f, 0.3f), 1.0, 1.0e-5, "on its sunny side");
    expect_near(btbh_gpu_particle_slab_transmittance(r, g, 0.3f, 0.4f), exp(-0.5 * 2.1), 1.0e-5, "in its middle");
    expect_near(btbh_gpu_particle_slab_transmittance(r, g, 0.3f, 0.5f), exp(-2.1), 1.0e-5, "on its lee side");
    expect_near(btbh_gpu_particle_slab_transmittance(r, g, 0.3f, 0.8f), exp(-2.1), 1.0e-5, "on the ground past it");
    /* A filtered column edge: the nearest depth slid past the mean (blended with empty texels) shrinks to the mean. */
    expect_near(btbh_gpu_particle_slab_transmittance(0.2f, 0.2f * 0.4f, 0.7f, 0.39f), 1.0, 0.0, "edge, before");
    expect_near(btbh_gpu_particle_slab_transmittance(0.2f, 0.2f * 0.4f, 0.7f, 0.41f), exp(-0.2), 1.0e-6, "edge, after");
    /* Random columns of evenly spaced, equal layers: the slab is exact for them (the coverage even from the first
       layer to the last), so a receiver's transmittance is exp of the share of their summed optical depth before it. */
    for (int trial = 0; trial < 2000; ++trial) {
        const int layers = 2 + (int)(random_unit() * 10.0f);
        const float front = 0.05f + 0.6f * random_unit();
        const float spacing = 0.002f + 0.03f * random_unit();
        const float each = 0.05f + 0.6f * random_unit();
        float optical = 0.0f;
        float weighted = 0.0f;
        for (int layer = 0; layer < layers; ++layer) {
            optical += each;
            weighted += each * (front + spacing * (float)layer);
        }
        const float back = front + spacing * (float)(layers - 1);
        const float depth = front - 0.05f + (back - front + 0.1f) * random_unit();
        double share = (depth - front) / (back - front);
        share = share < 0.0 ? 0.0 : (share > 1.0 ? 1.0 : share);
        expect_near(btbh_gpu_particle_slab_transmittance(optical, weighted, front, depth),
            exp(-(double)optical * share), 2.0e-3, "a random column");
    }
}

int main(void) {
    _Static_assert(sizeof(BTBHGPUSceneParticleFrameUniforms) == 192u, "the frame block is 192 bytes");
    _Static_assert(offsetof(BTBHGPUSceneParticleFrameUniforms, controls) == 64u, "controls");
    _Static_assert(offsetof(BTBHGPUSceneParticleFrameUniforms, cascade_field) == 160u, "cascade_field");
    _Static_assert(offsetof(BTBHGPUSceneParticleFrameUniforms, shadow_controls) == 176u, "shadow_controls");
    _Static_assert(BTBH_GPU_SCENE_SURFACE_LIT_PARTICLE == 11 && BTBH_GPU_SCENE_SURFACE_COUNT == 12,
        "LIT_PARTICLE is 11");
    _Static_assert(BTBH_GPU_SCENE_BATCH_INSTANCE_UV_RECT == 8u, "the UV rect is batch bit 3");
    check_phase();
    check_wrap();
    check_fog_makeup();
    check_fog_amount();
    check_refusals();
    check_particle_shadow_rules();
    check_particle_shadow_slab();
    printf("btbh_gpu_particle_test: %d checks passed\n", checks);
    return 0;
}
