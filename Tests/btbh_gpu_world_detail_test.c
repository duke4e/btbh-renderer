/* RFC 0008 without a GPU: the C copy of the world detail's projection, frame and seed hash
 * (src/gpu/btbh_gpu_world_detail.h, kept in step with shaders/gpu/production_world_detail.hlsli) against the RFC's
 * examples, how a detail is packed into the batch's uniforms, and which details are refused.
 *
 * - Faces: up and down, four walls, a 45 degree wall, a 30 degree pitch, and faces at 17 and 19 degrees (either
 *   side of the default 18). T x B = N on each, T and B unit and in the face.
 * - The image's top: up on walls, up the slope on the pitch, to the north on roofs; its right is T (unmirrored
 *   seen from the front, and from above with north up).
 * - No stretch: a world unit up the 30 degree slope moves v by 1 / wall_tile[1], as a unit of height on a wall.
 * - Level faces are projected straight down: a 17 degree face shares the ground's (u, v).
 * - The face normal given either way round, and a vertex normal in the face's plane (the camera decides).
 * - The seed: PCG's values, whole numbers (an ulp either way, and a value rounded to it, give the same offset),
 *   component 0 none, and offsets spread over [0, 1).
 * - The tilt: a flat normal adds none; a tilted one is tangent-space normal mapping in the frame.
 * - The packed uniforms and the refusals.
 *
 * RFC 0009 phase 2, the world-detail table:
 * - The vertex's index: rounded to even on a tie, 0 below 0, 255 above, 255 for NaN; the entry slot min(index, 7);
 *   a map applies below the count and with a layer that isn't 255.
 * - The table's block: 400 bytes at the shader's offsets; each entry's floats to the bit as a detail with the same
 *   floats packs them; the entries past the count zero; the batch extension's mode 16 and the table's seed.
 * - Every refusal of a table and an entry, and what a table without map_layers still checks. */
#undef NDEBUG

#include "btbh_gpu_world_detail.h"

#include <assert.h>
#include <float.h>
#include <math.h>
#include <stddef.h>
#include <stdio.h>
#include <string.h>

static const float tiles[4] = { 1.0f / 4.0f, 1.0f / 4.0f, 1.0f / 2.0f, 1.0f / 1.4f };
static const float cos_18 = 0.95105652f;

static int checks;

static void expect_near(float actual, float expected, float tolerance, const char *what) {
    if (!(fabsf(actual - expected) <= tolerance)) {
        fprintf(stderr, "%s: %.7f, expected %.7f\n", what, actual, expected);
    }
    assert(fabsf(actual - expected) <= tolerance);
    checks += 1;
}

static void expect_near3(const float actual[3], float x, float y, float z, const char *what) {
    expect_near(actual[0], x, 1.0e-5f, what);
    expect_near(actual[1], y, 1.0e-5f, what);
    expect_near(actual[2], z, 1.0e-5f, what);
}

/* A face with outward normal n (not necessarily unit), through `position`. The camera is out along n. */
static BTBHGPUWorldDetailFrame frame_at(const float n[3], const float position[3]) {
    const float to_camera[3] = { n[0] * 5.0f, n[1] * 5.0f, n[2] * 5.0f };
    BTBHGPUWorldDetailFrame frame;
    btbh_gpu_world_detail_frame(position, n, n, to_camera, cos_18, tiles, &frame);
    return frame;
}

/* The frame's invariants: N unit and outward, T and B unit and in the face, T x B = N. */
static void check_frame(const BTBHGPUWorldDetailFrame *frame, const float outward[3], const char *what) {
    float n[3] = { outward[0], outward[1], outward[2] };
    const float length = sqrtf(n[0] * n[0] + n[1] * n[1] + n[2] * n[2]);
    n[0] /= length;
    n[1] /= length;
    n[2] /= length;
    expect_near3(frame->normal, n[0], n[1], n[2], what);
    expect_near(btbh_gpu_world_detail_dot(frame->tangent, frame->tangent), 1.0f, 1.0e-5f, what);
    expect_near(btbh_gpu_world_detail_dot(frame->bitangent, frame->bitangent), 1.0f, 1.0e-5f, what);
    expect_near(btbh_gpu_world_detail_dot(frame->tangent, frame->normal), 0.0f, 1.0e-5f, what);
    expect_near(btbh_gpu_world_detail_dot(frame->bitangent, frame->normal), 0.0f, 1.0e-5f, what);
    float t_cross_b[3];
    btbh_gpu_world_detail_cross(frame->tangent, frame->bitangent, t_cross_b);
    expect_near3(t_cross_b, frame->normal[0], frame->normal[1], frame->normal[2], what);
}

/* The texture coordinate (no seed) at `position` on the face with outward normal n. */
static void coordinate_at(const float n[3], const float position[3], float coordinate[2]) {
    const BTBHGPUWorldDetailFrame frame = frame_at(n, position);
    const float none[2] = { 0.0f, 0.0f };
    btbh_gpu_world_detail_coordinate(&frame, none, coordinate);
}

/* Moving by `step` on the face changes the coordinate by `delta`. */
static void step_on_face(const float n[3], const float position[3], const float step[3], float delta[2]) {
    float a[2];
    float b[2];
    const float moved[3] = { position[0] + step[0], position[1] + step[1], position[2] + step[2] };
    coordinate_at(n, position, a);
    coordinate_at(n, moved, b);
    delta[0] = b[0] - a[0];
    delta[1] = b[1] - a[1];
}

static void test_faces(void) {
    const float p[3] = { 1.25f, 0.75f, -2.5f };
    const float up[3] = { 0.0f, 1.0f, 0.0f };
    const float down[3] = { 0.0f, -1.0f, 0.0f };
    float delta[2];

    /* Roof: level, T = +x, B = north (-z); u = x / level_tile, v = -z / level_tile. */
    BTBHGPUWorldDetailFrame frame = frame_at(up, p);
    assert(frame.level);
    check_frame(&frame, up, "up");
    expect_near3(frame.tangent, 1.0f, 0.0f, 0.0f, "up T");
    expect_near3(frame.bitangent, 0.0f, 0.0f, -1.0f, "up B");
    expect_near(frame.uv[0], 1.25f / 4.0f, 1.0e-6f, "up u");
    expect_near(frame.uv[1], 2.5f / 4.0f, 1.0e-6f, "up v");
    /* From above with north up on the screen: north (-z) is toward the image's top (t decreases), east is its
       right. */
    const float north[3] = { 0.0f, 0.0f, -1.0f };
    const float east[3] = { 1.0f, 0.0f, 0.0f };
    step_on_face(up, p, north, delta);
    expect_near(delta[0], 0.0f, 1.0e-6f, "roof north u");
    expect_near(delta[1], -0.25f, 1.0e-6f, "roof north t");
    step_on_face(up, p, east, delta);
    expect_near(delta[0], 0.25f, 1.0e-6f, "roof east u");

    /* A face that looks down: v = +z, and the frame stays right-handed (T x B = N). */
    frame = frame_at(down, p);
    assert(frame.level);
    check_frame(&frame, down, "down");
    expect_near3(frame.tangent, 1.0f, 0.0f, 0.0f, "down T");
    expect_near3(frame.bitangent, 0.0f, 0.0f, 1.0f, "down B");
    expect_near(frame.uv[1], -2.5f / 4.0f, 1.0e-6f, "down v");

    /* Four walls: T to the right seen from the front, B up; u = dot(P, T) / wall_tile, v = height / wall_tile. */
    const float walls[4][3] = { { 0, 0, 1 }, { 1, 0, 0 }, { 0, 0, -1 }, { -1, 0, 0 } };
    const float rights[4][3] = { { 1, 0, 0 }, { 0, 0, -1 }, { -1, 0, 0 }, { 0, 0, 1 } };
    for (int wall = 0; wall < 4; ++wall) {
        frame = frame_at(walls[wall], p);
        assert(!frame.level);
        check_frame(&frame, walls[wall], "wall");
        expect_near3(frame.tangent, rights[wall][0], rights[wall][1], rights[wall][2], "wall T");
        expect_near3(frame.bitangent, 0.0f, 1.0f, 0.0f, "wall B");
        expect_near(frame.uv[1], 0.75f / 1.4f, 1.0e-6f, "wall v is the height");
        expect_near(frame.uv[0], btbh_gpu_world_detail_dot(p, rights[wall]) / 2.0f, 1.0e-6f, "wall u");
        /* Upright: a unit up moves t toward the image's top by 1 / wall_tile[1]. */
        step_on_face(walls[wall], p, up, delta);
        expect_near(delta[0], 0.0f, 1.0e-6f, "wall up u");
        expect_near(delta[1], -1.0f / 1.4f, 1.0e-5f, "wall up t");
        /* Unmirrored: a unit to the right (seen from the front) moves u by 1 / wall_tile[0]. */
        step_on_face(walls[wall], p, rights[wall], delta);
        expect_near(delta[0], 0.5f, 1.0e-5f, "wall right u");
        expect_near(delta[1], 0.0f, 1.0e-6f, "wall right t");
    }

    /* A 45 degree wall: T runs along it (one copy of the pattern, not two half-strength ones). */
    const float diagonal[3] = { 0.70710678f, 0.0f, 0.70710678f };
    frame = frame_at(diagonal, p);
    assert(!frame.level);
    check_frame(&frame, diagonal, "45");
    expect_near3(frame.tangent, 0.70710678f, 0.0f, -0.70710678f, "45 T");
    expect_near3(frame.bitangent, 0.0f, 1.0f, 0.0f, "45 B");

    /* A 30 degree pitch facing +z: not level (cos 30 < cos 18); B runs up the slope. */
    const float pitch[3] = { 0.0f, 0.8660254f, 0.5f };
    frame = frame_at(pitch, p);
    assert(!frame.level);
    check_frame(&frame, pitch, "pitch");
    expect_near3(frame.tangent, 1.0f, 0.0f, 0.0f, "pitch T");
    expect_near3(frame.bitangent, 0.0f, 0.5f, -0.8660254f, "pitch B (up the slope)");
    /* The image's top up the slope; and no stretch: a unit up the slope moves v by 1 / wall_tile[1], as a unit of
       height on a wall does. (v = height would have moved it by sin 30 / 1.4: stretched twice.) */
    const float up_slope[3] = { 0.0f, 0.5f, -0.8660254f };
    step_on_face(pitch, p, up_slope, delta);
    expect_near(delta[0], 0.0f, 1.0e-6f, "pitch up-slope u");
    expect_near(delta[1], -1.0f / 1.4f, 1.0e-5f, "pitch up-slope t");
    /* The RFC's example: a 2 x 1.4 tile on a 30 degree roof covers 1.4 units up the slope. */
    const float tile_up_slope[3] = { 0.0f, 0.7f, -1.2124356f };
    step_on_face(pitch, p, tile_up_slope, delta);
    expect_near(delta[1], -1.0f, 1.0e-5f, "one repeat is 1.4 up the slope");

    /* 17 degrees: level, projected straight down (the ground's u and v); 19 degrees: a wall. */
    const float seventeen[3] = { 0.0f, cosf(17.0f * 3.14159265f / 180.0f), sinf(17.0f * 3.14159265f / 180.0f) };
    const float nineteen[3] = { 0.0f, cosf(19.0f * 3.14159265f / 180.0f), sinf(19.0f * 3.14159265f / 180.0f) };
    frame = frame_at(seventeen, p);
    assert(frame.level);
    check_frame(&frame, seventeen, "17");
    expect_near(frame.uv[0], 1.25f / 4.0f, 1.0e-6f, "17 u = x");
    expect_near(frame.uv[1], 2.5f / 4.0f, 1.0e-6f, "17 v = -z");
    /* Its frame: +x made orthogonal to N, B = cross(N, T) (north, tilted into the face). */
    expect_near3(frame.tangent, 1.0f, 0.0f, 0.0f, "17 T");
    expect_near(frame.bitangent[2], -seventeen[1], 1.0e-5f, "17 B toward the north");
    frame = frame_at(nineteen, p);
    assert(!frame.level);
    check_frame(&frame, nineteen, "19");
    /* A level face tilted toward x: T is still +x made orthogonal to N. */
    const float tilted_east[3] = { sinf(0.2f), cosf(0.2f), 0.0f };
    frame = frame_at(tilted_east, p);
    assert(frame.level);
    check_frame(&frame, tilted_east, "level tilted toward x");
    expect_near3(frame.tangent, cosf(0.2f), -sinf(0.2f), 0.0f, "level tilted T");

    /* The angle is a parameter: at 10 degrees the 17 degree face is a wall. */
    BTBHGPUWorldDetailFrame steep;
    btbh_gpu_world_detail_frame(p, seventeen, seventeen, seventeen, btbh_gpu_world_detail_cos_level(10.0f), tiles,
        &steep);
    assert(!steep.level);
    expect_near(btbh_gpu_world_detail_cos_level(0.0f), cos_18, 1.0e-6f, "0 means 18 degrees");

    /* The face normal either way round (as cross(ddx, ddy) gives it): the vertex normal picks the side. */
    const float wall[3] = { 0.0f, 0.0f, 1.0f };
    const float inward[3] = { 0.0f, 0.0f, -3.0f };
    const float camera[3] = { 0.0f, 0.0f, 5.0f };
    BTBHGPUWorldDetailFrame flipped;
    btbh_gpu_world_detail_frame(p, inward, wall, camera, cos_18, tiles, &flipped);
    check_frame(&flipped, wall, "face given inward");
    /* A vertex normal in the face's plane: turned toward the camera. */
    const float along[3] = { 1.0f, 0.0f, 0.0f };
    const float behind[3] = { 0.0f, 0.0f, -5.0f };
    btbh_gpu_world_detail_frame(p, wall, along, camera, cos_18, tiles, &flipped);
    check_frame(&flipped, wall, "camera side, in front");
    btbh_gpu_world_detail_frame(p, wall, along, behind, cos_18, tiles, &flipped);
    const float back[3] = { 0.0f, 0.0f, -1.0f };
    check_frame(&flipped, back, "camera side, behind");
    /* A zero face (no derivatives): the vertex normal stands in. */
    const float zero[3] = { 0.0f, 0.0f, 0.0f };
    btbh_gpu_world_detail_frame(p, zero, up, camera, cos_18, tiles, &flipped);
    check_frame(&flipped, up, "zero face");
    printf("faces: up, down, four walls, 45 degrees, a 30 degree pitch, 17 and 19 degrees: frames, the image's "
           "top and right, no stretch on the pitch\n");
}

static void test_seed(void) {
    const float custom[4] = { 7.0f, 99.0f, 1234.0f, -1.0f };
    float offset[2];
    /* PCG's halves: 7 -> 0x7e67121c, 99 -> 0x9b76d347, 1234 -> 0x6c7ab28b, -1 -> 0xe62a4902. */
    assert(btbh_gpu_world_detail_hash(7u) == 0x7e67121cu);
    assert(btbh_gpu_world_detail_hash(0u) == 0x07bb2fe2u);
    btbh_gpu_world_detail_offset(custom, 1u, offset);
    expect_near(offset[0], (float)0x121cu / 65536.0f, 0.0f, "seed 7 u");
    expect_near(offset[1], (float)0x7e67u / 65536.0f, 0.0f, "seed 7 v");
    btbh_gpu_world_detail_offset(custom, 2u, offset);
    expect_near(offset[0], (float)0xd347u / 65536.0f, 0.0f, "seed 99 u");
    expect_near(offset[1], (float)0x9b76u / 65536.0f, 0.0f, "seed 99 v");
    btbh_gpu_world_detail_offset(custom, 3u, offset);
    expect_near(offset[0], (float)0xb28bu / 65536.0f, 0.0f, "seed 1234 u");
    btbh_gpu_world_detail_offset(custom, 4u, offset);
    expect_near(offset[0], (float)0x4902u / 65536.0f, 0.0f, "seed -1 u");
    expect_near(offset[1], (float)0xe62au / 65536.0f, 0.0f, "seed -1 v");
    /* Component 0: no offset. */
    btbh_gpu_world_detail_offset(custom, 0u, offset);
    assert(offset[0] == 0.0f && offset[1] == 0.0f);
    /* Whole numbers: an interpolated 7 off by an ulp either way, or 6.6, gives 7's offset. */
    float seven[2];
    const float exact[4] = { 0.0f, 0.0f, 0.0f, 7.0f };
    btbh_gpu_world_detail_offset(exact, 4u, seven);
    const float variants[4] = { nextafterf(7.0f, 8.0f), nextafterf(7.0f, 6.0f), 6.6f, 7.4f };
    for (int index = 0; index < 4; ++index) {
        const float noisy[4] = { 0.0f, 0.0f, 0.0f, variants[index] };
        btbh_gpu_world_detail_offset(noisy, 4u, offset);
        assert(offset[0] == seven[0] && offset[1] == seven[1]);
    }
    /* Exact up to 2^24: its neighbour below differs. Beyond 2^31 it is clamped (no undefined conversion). */
    float big[2];
    float below[2];
    const float at_limit[4] = { 16777216.0f, 16777215.0f, 1.0e20f, -1.0e20f };
    btbh_gpu_world_detail_offset(at_limit, 1u, big);
    btbh_gpu_world_detail_offset(at_limit, 2u, below);
    assert(big[0] != below[0] || big[1] != below[1]);
    btbh_gpu_world_detail_offset(at_limit, 3u, offset);
    assert(offset[0] >= 0.0f && offset[0] < 1.0f);
    btbh_gpu_world_detail_offset(at_limit, 4u, offset);
    assert(offset[0] >= 0.0f && offset[0] < 1.0f);
    /* Spread: lots 0..1023 cover [0, 1) evenly along u and v, and neighbouring lots never share an offset. */
    double sum[2] = { 0.0, 0.0 };
    uint32_t bins[2][8];
    memset(bins, 0, sizeof(bins));
    float previous[2] = { -1.0f, -1.0f };
    for (uint32_t lot = 0u; lot < 1024u; ++lot) {
        const float seed[4] = { (float)lot, 0.0f, 0.0f, 0.0f };
        btbh_gpu_world_detail_offset(seed, 1u, offset);
        for (int axis = 0; axis < 2; ++axis) {
            assert(offset[axis] >= 0.0f && offset[axis] < 1.0f);
            sum[axis] += offset[axis];
            bins[axis][(uint32_t)(offset[axis] * 8.0f)] += 1u;
        }
        assert(offset[0] != previous[0] || offset[1] != previous[1]);
        previous[0] = offset[0];
        previous[1] = offset[1];
    }
    for (int axis = 0; axis < 2; ++axis) {
        expect_near((float)(sum[axis] / 1024.0), 0.5f, 0.03f, "mean offset");
        for (int bin = 0; bin < 8; ++bin) {
            assert(bins[axis][bin] > 96u && bins[axis][bin] < 160u);
        }
    }
    /* The offset shifts the texture coordinate. */
    const float up[3] = { 0.0f, 1.0f, 0.0f };
    const float p[3] = { 1.0f, 0.0f, -1.0f };
    const BTBHGPUWorldDetailFrame frame = frame_at(up, p);
    float coordinate[2];
    btbh_gpu_world_detail_coordinate(&frame, seven, coordinate);
    expect_near(coordinate[0], 0.25f + seven[0], 1.0e-6f, "offset u");
    expect_near(coordinate[1], -0.25f + seven[1], 1.0e-6f, "offset t");
    printf("seed: PCG's values, whole numbers, component 0 none, 1024 lots spread over [0, 1)\n");
}

static void test_gradient(void) {
    const float pitch[3] = { 0.0f, 0.8660254f, 0.5f };
    const float p[3] = { 0.0f, 0.0f, 0.0f };
    const BTBHGPUWorldDetailFrame frame = frame_at(pitch, p);
    float gradient[3];
    const float flat[2] = { 0.0f, 0.0f };
    btbh_gpu_world_detail_gradient(&frame, flat, gradient);
    expect_near3(gradient, 0.0f, 0.0f, 0.0f, "flat adds no tilt");
    /* On a flat first set (normal N), normalize(N - gradient) is tangent-space normal mapping in (T, B, N). */
    const float tilt[2] = { 0.36f, -0.48f };
    btbh_gpu_world_detail_gradient(&frame, tilt, gradient);
    const float mz = sqrtf(1.0f - 0.36f * 0.36f - 0.48f * 0.48f);
    float shaded[3];
    float expected[3];
    for (int axis = 0; axis < 3; ++axis) {
        shaded[axis] = frame.normal[axis] - gradient[axis];
        expected[axis] = tilt[0] * frame.tangent[axis] + tilt[1] * frame.bitangent[axis] + mz * frame.normal[axis];
    }
    const float zero[3] = { 0.0f, 0.0f, 0.0f };
    btbh_gpu_world_detail_normalize(shaded, zero, shaded);
    expect_near3(shaded, expected[0], expected[1], expected[2], "normal mapping in the frame");
    /* An OpenGL map's green tilts toward B (the image's top): up the slope here. */
    const float green[2] = { 0.0f, 0.5f };
    btbh_gpu_world_detail_gradient(&frame, green, gradient);
    for (int axis = 0; axis < 3; ++axis) {
        shaded[axis] = frame.normal[axis] - gradient[axis];
    }
    assert(btbh_gpu_world_detail_dot(shaded, frame.bitangent) > 0.4f);
    printf("tilt: a flat normal adds none; a tilted one maps in (T, B, N), green toward the image's top\n");
}

static BTBHGPUSceneWorldDetail valid_detail(void) {
    static int textures[3];
    BTBHGPUSceneWorldDetail detail;
    memset(&detail, 0, sizeof(detail));
    detail.normal_texture = (BTBHGPUTexture *)(void *)&textures[1];
    detail.orm_texture = (BTBHGPUTexture *)(void *)&textures[2];
    detail.flags = BTBH_GPU_SCENE_WORLD_DETAIL_ALBEDO_IN_ORM_BLUE;
    detail.level_tile[0] = 4.0f;
    detail.level_tile[1] = 4.0f;
    detail.wall_tile[0] = 2.0f;
    detail.wall_tile[1] = 1.4f;
    detail.seed_component = 4u;
    return detail;
}

static void refused(BTBHGPUSceneWorldDetail detail, const char *what) {
    const char *problem = btbh_gpu_world_detail_problem(&detail);
    if (problem == NULL) {
        fprintf(stderr, "not refused: %s\n", what);
    }
    assert(problem != NULL);
    checks += 1;
}

static void test_refusals(void) {
    static int albedo;
    BTBHGPUSceneWorldDetail detail = valid_detail();
    assert(btbh_gpu_world_detail_problem(&detail) == NULL);
    /* Zeroed scales and angle are the defaults, not refusals. */
    detail.albedo_scale = 1.25f;
    detail.roughness_scale = 1.25f;
    detail.normal_scale = -0.5f;
    detail.level_degrees = 89.0f;
    assert(btbh_gpu_world_detail_problem(&detail) == NULL);
    const float bad_tiles[4] = { 0.0f, -1.0f, NAN, INFINITY };
    for (int value = 0; value < 4; ++value) {
        for (int slot = 0; slot < 4; ++slot) {
            detail = valid_detail();
            if (slot < 2) {
                detail.level_tile[slot] = bad_tiles[value];
            } else {
                detail.wall_tile[slot - 2] = bad_tiles[value];
            }
            refused(detail, "a tile that isn't finite and above 0");
        }
    }
    detail = valid_detail();
    detail.level_degrees = 90.0f;
    refused(detail, "level_degrees 90");
    detail.level_degrees = -1.0f;
    refused(detail, "level_degrees -1");
    detail.level_degrees = NAN;
    refused(detail, "level_degrees NaN");
    detail = valid_detail();
    detail.albedo_scale = NAN;
    refused(detail, "albedo_scale NaN");
    detail = valid_detail();
    detail.roughness_scale = INFINITY;
    refused(detail, "roughness_scale infinite");
    detail = valid_detail();
    detail.seed_component = 5u;
    refused(detail, "seed_component 5");
    detail = valid_detail();
    detail.flags = 0x80u;
    refused(detail, "an unknown flag");
    detail.flags = BTBH_GPU_SCENE_WORLD_DETAIL_ALBEDO_IN_ORM_BLUE | 0x02u;
    refused(detail, "a known and an unknown flag");
    detail = valid_detail();
    detail.reserved[1] = 1u;
    refused(detail, "reserved");
    detail = valid_detail();
    detail.albedo_texture = (BTBHGPUTexture *)(void *)&albedo;
    refused(detail, "packed with an albedo texture");
    detail = valid_detail();
    detail.orm_texture = NULL;
    refused(detail, "packed without an ORM texture");
    /* Three maps, unpacked: valid. */
    detail = valid_detail();
    detail.flags = 0u;
    detail.albedo_texture = (BTBHGPUTexture *)(void *)&albedo;
    assert(btbh_gpu_world_detail_problem(&detail) == NULL);
    printf("refusals: zero, negative, NaN and infinite tiles, the angle, NaN scales, seed component 5, unknown "
           "flags, reserved, packed with an albedo texture or without an ORM texture\n");
}

static void test_packing(void) {
    static int albedo;
    BTBHGPUSceneGeometryBatch batch;
    memset(&batch, 0, sizeof(batch));
    float tiles_out[4];
    float controls[4];
    uint32_t mode[4];
    /* No detail: all zero. */
    btbh_gpu_world_detail_pack(&batch, tiles_out, controls, mode);
    assert(mode[0] == 0u && tiles_out[0] == 0.0f && controls[3] == 0.0f);
    assert(!btbh_gpu_world_detail_active(&batch));
    /* A detail without textures: all zero, as without one. */
    BTBHGPUSceneWorldDetail detail = valid_detail();
    detail.normal_texture = NULL;
    detail.orm_texture = NULL;
    detail.flags = 0u;
    batch.world_detail = &detail;
    btbh_gpu_world_detail_pack(&batch, tiles_out, controls, mode);
    assert(mode[0] == 0u && mode[1] == 0u && tiles_out[2] == 0.0f);
    assert(!btbh_gpu_world_detail_active(&batch));
    /* Packed: normal and ORM, the albedo in the ORM's blue; zeroed scales are 1, the angle 18 degrees. */
    detail = valid_detail();
    btbh_gpu_world_detail_pack(&batch, tiles_out, controls, mode);
    assert(btbh_gpu_world_detail_active(&batch));
    assert(mode[0] == (BTBH_GPU_WORLD_DETAIL_MAP_NORMAL | BTBH_GPU_WORLD_DETAIL_MAP_ORM |
        BTBH_GPU_WORLD_DETAIL_MAP_ALBEDO_IN_ORM_BLUE));
    assert(mode[1] == 4u && mode[2] == 0u && mode[3] == 0u);
    expect_near(tiles_out[0], 0.25f, 0.0f, "1 / level_tile u");
    expect_near(tiles_out[1], 0.25f, 0.0f, "1 / level_tile v");
    expect_near(tiles_out[2], 0.5f, 0.0f, "1 / wall_tile u");
    expect_near(tiles_out[3], 1.0f / 1.4f, 0.0f, "1 / wall_tile v");
    expect_near(controls[0], 1.0f, 0.0f, "albedo scale 0 means 1");
    expect_near(controls[1], 1.0f, 0.0f, "normal scale 0 means 1");
    expect_near(controls[2], 1.0f, 0.0f, "roughness scale 0 means 1");
    expect_near(controls[3], cos_18, 1.0e-6f, "cos 18");
    /* v2's scales and an angle. */
    detail.albedo_scale = 1.25f;
    detail.normal_scale = 0.5f;
    detail.roughness_scale = 1.25f;
    detail.level_degrees = 30.0f;
    btbh_gpu_world_detail_pack(&batch, tiles_out, controls, mode);
    expect_near(controls[0], 1.25f, 0.0f, "albedo scale");
    expect_near(controls[1], 0.5f, 0.0f, "normal scale");
    expect_near(controls[2], 1.25f, 0.0f, "roughness scale");
    expect_near(controls[3], 0.8660254f, 1.0e-6f, "cos 30");
    /* Three maps. */
    detail.flags = 0u;
    detail.albedo_texture = (BTBHGPUTexture *)(void *)&albedo;
    btbh_gpu_world_detail_pack(&batch, tiles_out, controls, mode);
    assert(mode[0] == (BTBH_GPU_WORLD_DETAIL_MAP_ALBEDO | BTBH_GPU_WORLD_DETAIL_MAP_NORMAL |
        BTBH_GPU_WORLD_DETAIL_MAP_ORM));
    /* A refracting batch ignores it. */
    batch.surface_variant = BTBH_GPU_SCENE_SURFACE_REFRACT;
    btbh_gpu_world_detail_pack(&batch, tiles_out, controls, mode);
    assert(mode[0] == 0u && !btbh_gpu_world_detail_active(&batch));
    printf("packing: none and no textures all zero, packed and three maps, zeroed scales 1, the angle's cosine, "
           "refraction ignores it\n");
}

/* ------------------------------------------------------------------------------------- RFC 0009: the table */

_Static_assert(sizeof(BTBHGPUWorldDetailTableUniforms) == 400u, "the table's block is 400 bytes");
_Static_assert(offsetof(BTBHGPUWorldDetailTableUniforms, tiles) == 0u, "float4 tiles[8] at 0");
_Static_assert(offsetof(BTBHGPUWorldDetailTableUniforms, controls) == 128u, "float4 controls[8] at 128");
_Static_assert(offsetof(BTBHGPUWorldDetailTableUniforms, layers) == 256u, "uint4 layers[8] at 256");
_Static_assert(offsetof(BTBHGPUWorldDetailTableUniforms, count) == 384u, "uint4 count at 384");

static void test_table_index(void) {
    static const struct {
        float joint;
        uint32_t index;
    } cases[] = {
        { 0.0f, 0u }, { 1.0f, 1u }, { 7.0f, 7u }, { 8.0f, 8u }, { 254.0f, 254u }, { 255.0f, 255u },
        { 0.4f, 0u }, { 0.5f, 0u }, { 1.5f, 2u }, { 2.5f, 2u }, { 2.6f, 3u }, { 7.6f, 8u }, { 254.5f, 254u },
        { 254.6f, 255u }, { 256.0f, 255u }, { 1.0e6f, 255u }, { -0.0f, 0u }, { -0.4f, 0u }, { -0.6f, 0u },
        { -2.0f, 0u }, { -1.0e6f, 0u },
    };
    for (size_t index = 0u; index < sizeof(cases) / sizeof(cases[0]); ++index) {
        const uint32_t got = btbh_gpu_world_detail_table_index(cases[index].joint);
        if (got != cases[index].index) {
            fprintf(stderr, "index of %g: %u, expected %u\n", (double)cases[index].joint, got, cases[index].index);
        }
        assert(got == cases[index].index);
        checks += 1;
    }
    assert(btbh_gpu_world_detail_table_index(INFINITY) == 255u);
    assert(btbh_gpu_world_detail_table_index(-INFINITY) == 0u);
    assert(btbh_gpu_world_detail_table_index(NAN) == 255u);
    checks += 3;
    /* The slot read and whether a map applies. */
    for (uint32_t index = 0u; index < 300u; ++index) {
        const uint32_t slot = btbh_gpu_world_detail_table_slot(index);
        assert(slot == (index < 8u ? index : 7u));
        for (uint32_t count = 1u; count <= 8u; ++count) {
            assert(btbh_gpu_world_detail_table_applies(index, count, 3u) == (index < count));
            assert(!btbh_gpu_world_detail_table_applies(index, count, BTBH_GPU_SCENE_WORLD_DETAIL_NO_LAYER));
        }
        checks += 1;
    }
    printf("table index: rounded to even on a tie, 0 below 0, 255 above and for NaN; slot min(index, 7); a map "
           "applies below the count with a layer that isn't 255\n");
}

static BTBHGPUSceneWorldDetailTableEntry valid_entry(uint8_t normal_layer, uint8_t orm_layer) {
    BTBHGPUSceneWorldDetailTableEntry entry;
    memset(&entry, 0, sizeof(entry));
    entry.level_tile[0] = 4.0f;
    entry.level_tile[1] = 4.0f;
    entry.wall_tile[0] = 2.0f;
    entry.wall_tile[1] = 1.4f;
    entry.normal_layer = normal_layer;
    entry.orm_layer = orm_layer;
    return entry;
}

static void table_refused(
    const BTBHGPUSceneWorldDetailTable *table,
    uint32_t layer_count,
    uint32_t expected_entry,
    const char *what
) {
    uint32_t entry = 12345u;
    const char *problem = btbh_gpu_world_detail_table_problem(table, layer_count, &entry);
    if (problem == NULL || entry != expected_entry) {
        fprintf(stderr, "%s: %s (entry %u)\n", what, problem != NULL ? problem : "accepted", entry);
    }
    assert(problem != NULL);
    assert(entry == expected_entry);
    checks += 1;
}

static void test_table_refusals(void) {
    /* A stand-in for map_layers: the checks here never look inside it (the scene checks its kind and owner). */
    static int array_stand_in;
    BTBHGPUSceneWorldDetailTableEntry entries[8];
    for (uint32_t index = 0u; index < 8u; ++index) {
        entries[index] = valid_entry((uint8_t)(2u * index), (uint8_t)(2u * index + 1u));
    }
    BTBHGPUSceneWorldDetailTable table;
    memset(&table, 0, sizeof(table));
    table.map_layers = (BTBHGPUTexture *)(void *)&array_stand_in;
    table.entries = entries;
    table.entry_count = 8u;
    table.seed_component = 4u;
    uint32_t entry = 0u;
    assert(btbh_gpu_world_detail_table_problem(&table, 16u, &entry) == NULL && entry == UINT32_MAX);
    /* An absent map (255) and the last layer are fine; v2's eight classes over 16 layers. */
    entries[3].normal_layer = BTBH_GPU_SCENE_WORLD_DETAIL_NO_LAYER;
    entries[5].orm_layer = BTBH_GPU_SCENE_WORLD_DETAIL_NO_LAYER;
    entries[7].orm_layer = 15u;
    entries[2].level_degrees = 89.9f;
    entries[2].albedo_scale = -1.0f;
    assert(btbh_gpu_world_detail_table_problem(&table, 16u, &entry) == NULL);
    checks += 2;
    /* The table's own fields. */
    BTBHGPUSceneWorldDetailTable bad = table;
    bad.entry_count = 0u;
    table_refused(&bad, 16u, UINT32_MAX, "entry_count 0");
    bad.entry_count = 9u;
    table_refused(&bad, 16u, UINT32_MAX, "entry_count 9");
    bad = table;
    bad.entries = NULL;
    table_refused(&bad, 16u, UINT32_MAX, "entries NULL");
    bad = table;
    bad.seed_component = 5u;
    table_refused(&bad, 16u, UINT32_MAX, "seed_component 5");
    bad = table;
    bad.flags = 1u;
    table_refused(&bad, 16u, UINT32_MAX, "flags");
    bad = table;
    bad.reserved[1] = 1u;
    table_refused(&bad, 16u, UINT32_MAX, "reserved");
    /* Without map_layers it draws as none: the entries go unchecked, the seed, flags and reserved bytes don't. */
    bad = table;
    bad.map_layers = NULL;
    bad.entries = NULL;
    bad.entry_count = 0u;
    assert(btbh_gpu_world_detail_table_problem(&bad, 0u, &entry) == NULL);
    checks += 1;
    bad.seed_component = 7u;
    table_refused(&bad, 0u, UINT32_MAX, "seed_component 7 without map_layers");
    /* An entry's fields: each fault names the entry. */
    for (uint32_t fault = 0u; fault < 14u; ++fault) {
        BTBHGPUSceneWorldDetailTableEntry broken[8];
        memcpy(broken, entries, sizeof(broken));
        BTBHGPUSceneWorldDetailTableEntry *e = &broken[6];
        const char *what = "";
        switch (fault) {
            case 0u: e->level_tile[0] = 0.0f; what = "level_tile 0"; break;
            case 1u: e->level_tile[1] = -1.0f; what = "level_tile negative"; break;
            case 2u: e->wall_tile[0] = NAN; what = "wall_tile NaN"; break;
            case 3u: e->wall_tile[1] = INFINITY; what = "wall_tile infinite"; break;
            case 4u: e->albedo_scale = NAN; what = "albedo_scale NaN"; break;
            case 5u: e->normal_scale = INFINITY; what = "normal_scale infinite"; break;
            case 6u: e->roughness_scale = -INFINITY; what = "roughness_scale infinite"; break;
            case 7u: e->level_degrees = 90.0f; what = "level_degrees 90"; break;
            case 8u: e->level_degrees = -1.0f; what = "level_degrees -1"; break;
            case 9u: e->normal_layer = 16u; what = "normal_layer 16 of 16"; break;
            case 10u: e->orm_layer = 254u; what = "orm_layer 254 of 16"; break;
            case 11u: e->flags = 1u; what = "flags"; break;
            case 12u: e->reserved = 1u; what = "reserved"; break;
            default: e->level_degrees = NAN; what = "level_degrees NaN"; break;
        }
        BTBHGPUSceneWorldDetailTable with = table;
        with.entries = broken;
        table_refused(&with, 16u, 6u, what);
    }
    /* An entry past the count isn't checked. */
    BTBHGPUSceneWorldDetailTableEntry past[8];
    memcpy(past, entries, sizeof(past));
    past[7].level_tile[0] = 0.0f;
    BTBHGPUSceneWorldDetailTable shorter = table;
    shorter.entries = past;
    shorter.entry_count = 7u;
    assert(btbh_gpu_world_detail_table_problem(&shorter, 16u, &entry) == NULL);
    /* A layer is checked against the array's count: 16 layers take 15, one layer takes only 0 and 255. */
    BTBHGPUSceneWorldDetailTableEntry single = valid_entry(0u, BTBH_GPU_SCENE_WORLD_DETAIL_NO_LAYER);
    shorter.entries = &single;
    shorter.entry_count = 1u;
    assert(btbh_gpu_world_detail_table_problem(&shorter, 1u, &entry) == NULL);
    single.orm_layer = 1u;
    table_refused(&shorter, 1u, 0u, "orm_layer 1 of 1");
    checks += 2;
    printf("table refusals: entry_count 0 and 9, entries NULL, seed 5, flags, reserved; per entry tiles 0, negative, "
           "NaN, infinite, scales NaN and infinite, angles 90, -1 and NaN, layers past the count, flags, reserved "
           "(naming the entry); none without map_layers but its seed, flags and reserved\n");
}

static void test_table_packing(void) {
    static int array_stand_in;
    static int texture_stand_in;
    BTBHGPUSceneWorldDetailTableEntry entries[3] = {
        valid_entry(0u, 1u), valid_entry(2u, 3u), valid_entry(BTBH_GPU_SCENE_WORLD_DETAIL_NO_LAYER, 5u),
    };
    entries[1].albedo_scale = 1.25f;
    entries[1].normal_scale = 0.5f;
    entries[1].roughness_scale = 1.25f;
    entries[1].level_degrees = 30.0f;
    entries[1].wall_tile[1] = 0.7f;
    entries[2].level_tile[0] = 3.0f;
    BTBHGPUSceneWorldDetailTable table;
    memset(&table, 0, sizeof(table));
    table.map_layers = (BTBHGPUTexture *)(void *)&array_stand_in;
    table.entries = entries;
    table.entry_count = 3u;
    table.seed_component = 4u;
    BTBHGPUWorldDetailTableUniforms block;
    memset(&block, 0xa5, sizeof(block));
    btbh_gpu_world_detail_table_pack(&table, &block);
    assert(block.count[0] == 3u && block.count[1] == 0u && block.count[2] == 0u && block.count[3] == 0u);
    for (uint32_t index = 0u; index < 8u; ++index) {
        /* Each entry as btbh_gpu_world_detail_pack packs a detail with the same floats, to the bit. */
        if (index < 3u) {
            BTBHGPUSceneWorldDetail detail;
            memset(&detail, 0, sizeof(detail));
            /* The entry's first 32 bytes are the detail's 24..55. */
            memcpy(&detail.level_tile[0], &entries[index], 32u);
            detail.orm_texture = (BTBHGPUTexture *)(void *)&texture_stand_in;
            detail.flags = BTBH_GPU_SCENE_WORLD_DETAIL_ALBEDO_IN_ORM_BLUE;
            BTBHGPUSceneGeometryBatch batch;
            memset(&batch, 0, sizeof(batch));
            batch.world_detail = &detail;
            float tiles_out[4];
            float controls[4];
            uint32_t mode[4];
            btbh_gpu_world_detail_pack(&batch, tiles_out, controls, mode);
            assert(memcmp(block.tiles[index], tiles_out, sizeof(tiles_out)) == 0);
            assert(memcmp(block.controls[index], controls, sizeof(controls)) == 0);
            assert(block.layers[index][0] == entries[index].normal_layer);
            assert(block.layers[index][1] == entries[index].orm_layer);
            assert(block.layers[index][2] == 0u && block.layers[index][3] == 0u);
        } else {
            static const float zero[4] = { 0.0f, 0.0f, 0.0f, 0.0f };
            static const uint32_t zero_layers[4] = { 0u, 0u, 0u, 0u };
            assert(memcmp(block.tiles[index], zero, sizeof(zero)) == 0);
            assert(memcmp(block.controls[index], zero, sizeof(zero)) == 0);
            assert(memcmp(block.layers[index], zero_layers, sizeof(zero_layers)) == 0);
        }
        checks += 1;
    }
    expect_near(block.controls[1][3], 0.8660254f, 1.0e-6f, "entry 1's cos 30");
    expect_near(block.tiles[1][3], 1.0f / 0.7f, 0.0f, "entry 1's 1 / wall_tile v");
    expect_near(block.controls[0][0], 1.0f, 0.0f, "a zero scale means 1");
    /* The batch extension: mode 16 and the table's seed, the single detail's floats zero; a table without
       map_layers, or on a refracting batch, packs nothing. */
    BTBHGPUSceneGeometryBatch batch;
    memset(&batch, 0, sizeof(batch));
    batch.world_detail_table = &table;
    float tiles_out[4];
    float controls[4];
    uint32_t mode[4];
    btbh_gpu_world_detail_pack(&batch, tiles_out, controls, mode);
    assert(btbh_gpu_world_detail_table_active(&batch));
    assert(mode[0] == BTBH_GPU_WORLD_DETAIL_MAP_TABLE && mode[1] == 4u && mode[2] == 0u && mode[3] == 0u);
    assert(tiles_out[0] == 0.0f && controls[0] == 0.0f);
    table.map_layers = NULL;
    btbh_gpu_world_detail_pack(&batch, tiles_out, controls, mode);
    assert(!btbh_gpu_world_detail_table_active(&batch) && mode[0] == 0u);
    table.map_layers = (BTBHGPUTexture *)(void *)&array_stand_in;
    batch.surface_variant = BTBH_GPU_SCENE_SURFACE_REFRACT;
    btbh_gpu_world_detail_pack(&batch, tiles_out, controls, mode);
    assert(!btbh_gpu_world_detail_table_active(&batch) && mode[0] == 0u);
    checks += 3;
    printf("table packing: 400 bytes at the shader's offsets, each entry to the bit as a detail with its floats, "
           "the rest zero; mode 16 with the table's seed; nothing without map_layers or when refracting\n");
}

int main(void) {
    test_faces();
    test_seed();
    test_gradient();
    test_refusals();
    test_packing();
    test_table_index();
    test_table_refusals();
    test_table_packing();
    printf("world detail: %d checks passed\n", checks);
    return 0;
}
