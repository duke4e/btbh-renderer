/* RFC 0007's native tests: the SDF terrain's CPU core (no window, no GPU). */

#include <btbh_gpu/btbh_gpu.h>
#include "btbh_gpu_sdf_terrain_internal.h"

#include <SDL3/SDL.h>

#include <assert.h>
#include <math.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#define NEWLINE "\n"

#define CHECK(condition)                                                                     \
    do {                                                                                     \
        if (!(condition)) {                                                                  \
            fprintf(stderr, "%s:%d: check failed: %s\n", __FILE__, __LINE__, #condition);   \
            exit(1);                                                                         \
        }                                                                                    \
    } while (0)

/* Flat ground at y = ground_y: asphalt (0) over soil (1) from 0.3 down. */
typedef struct FlatBase {
    float ground_y;
} FlatBase;

static void flat_base(void *user, const float *points, uint32_t count, float *distances, uint8_t *materials) {
    const FlatBase *base = (const FlatBase *)user;
    for (uint32_t index = 0u; index < count; ++index) {
        const float y = points[index * 3u + 1u];
        distances[index] = y - base->ground_y;
        materials[index] = base->ground_y - y <= 0.3f ? 0u : 1u;
    }
}

/* ---------------------------------------------------------------------------------------- a job runner */

/* Four persistent workers (a host's job system); every other call hands the indices out in reverse. */
typedef struct JobPool {
    SDL_Thread *threads[4];
    SDL_Semaphore *start;
    SDL_Semaphore *done;
    SDL_AtomicInt next;
    uint32_t count;
    void (*job)(void *, uint32_t);
    void *user;
    int reverse;
    int quit;
} JobPool;

static JobPool pool;
static uint32_t jobs_calls = 0u;

static int job_worker(void *data) {
    JobPool *p = (JobPool *)data;
    for (;;) {
        SDL_WaitSemaphore(p->start);
        if (p->quit) {
            return 0;
        }
        for (;;) {
            const int taken = SDL_AddAtomicInt(&p->next, 1);
            if (taken < 0 || (uint32_t)taken >= p->count) {
                break;
            }
            const uint32_t index = p->reverse ? p->count - 1u - (uint32_t)taken : (uint32_t)taken;
            p->job(p->user, index);
        }
        SDL_SignalSemaphore(p->done);
    }
}

static void pool_start(void) {
    pool.start = SDL_CreateSemaphore(0u);
    pool.done = SDL_CreateSemaphore(0u);
    CHECK(pool.start != NULL && pool.done != NULL);
    for (int index = 0; index < 4; ++index) {
        pool.threads[index] = SDL_CreateThread(job_worker, "sdf-terrain-job", &pool);
        CHECK(pool.threads[index] != NULL);
    }
}

static void pool_stop(void) {
    pool.quit = 1;
    for (int index = 0; index < 4; ++index) {
        SDL_SignalSemaphore(pool.start);
    }
    for (int index = 0; index < 4; ++index) {
        SDL_WaitThread(pool.threads[index], NULL);
    }
    SDL_DestroySemaphore(pool.start);
    SDL_DestroySemaphore(pool.done);
}

static void run_jobs_threads(void *host_user, uint32_t count, void (*job)(void *, uint32_t), void *job_user) {
    (void)host_user;
    jobs_calls += 1u;
    SDL_SetAtomicInt(&pool.next, 0);
    pool.count = count;
    pool.job = job;
    pool.user = job_user;
    pool.reverse = (int)(jobs_calls & 1u);
    for (int index = 0; index < 4; ++index) {
        SDL_SignalSemaphore(pool.start);
    }
    for (int index = 0; index < 4; ++index) {
        SDL_WaitSemaphore(pool.done);
    }
}

/* ---------------------------------------------------------------------------------------- scenarios */

static FlatBase flat = { 0.0f };

static BTBHGPUSdfTerrainDesc flat_desc(float cell, uint32_t chunk_cells, uint8_t normals) {
    BTBHGPUSdfTerrainDesc desc;
    memset(&desc, 0, sizeof(desc));
    desc.bounds_min[0] = -32.0f;
    desc.bounds_min[1] = -8.0f;
    desc.bounds_min[2] = -32.0f;
    desc.bounds_max[0] = 32.0f;
    desc.bounds_max[1] = 8.0f;
    desc.bounds_max[2] = 32.0f;
    desc.cell_size = cell;
    desc.chunk_cells = chunk_cells;
    desc.base_function = flat_base;
    desc.base_user = &flat;
    desc.normals = normals;
    desc.material_colors[0][0] = 0.2f;
    desc.material_colors[0][1] = 0.2f;
    desc.material_colors[0][2] = 0.22f;
    desc.material_colors[0][3] = 1.0f;
    desc.material_colors[1][0] = 0.4f;
    desc.material_colors[1][1] = 0.28f;
    desc.material_colors[1][2] = 0.15f;
    desc.material_colors[1][3] = 1.0f;
    desc.material_colors[2][0] = 0.05f;
    desc.material_colors[2][1] = 0.04f;
    desc.material_colors[2][2] = 0.03f;
    desc.material_colors[2][3] = 1.0f;
    return desc;
}

static BTBHGPUSdfEdit crater_bowl(float x, float z, float radius, float depth) {
    BTBHGPUSdfEdit edit;
    memset(&edit, 0, sizeof(edit));
    edit.shape = BTBH_GPU_SDF_SHAPE_ELLIPSOID;
    edit.operation = BTBH_GPU_SDF_OPERATION_SUBTRACT;
    edit.position[0] = x;
    edit.position[1] = depth * 0.4f;
    edit.position[2] = z;
    edit.size[0] = radius;
    edit.size[1] = depth;
    edit.size[2] = radius;
    edit.smoothing = 0.3f;
    edit.material = 2u;
    edit.material_band = 0.15f;
    return edit;
}

static BTBHGPUSdfEdit crater_rim(float x, float z, float radius) {
    BTBHGPUSdfEdit edit;
    memset(&edit, 0, sizeof(edit));
    edit.shape = BTBH_GPU_SDF_SHAPE_TORUS;
    edit.operation = BTBH_GPU_SDF_OPERATION_ADD;
    edit.position[0] = x;
    edit.position[1] = -0.25f;
    edit.position[2] = z;
    edit.size[0] = radius * 1.04f;
    edit.size[1] = 0.5f;
    edit.smoothing = 0.6f;
    edit.material = 1u;
    return edit;
}

static BTBHGPUSdfEdit rubble_box(float x, float z, float yaw, float pitch) {
    BTBHGPUSdfEdit edit;
    memset(&edit, 0, sizeof(edit));
    edit.shape = BTBH_GPU_SDF_SHAPE_BOX;
    edit.operation = BTBH_GPU_SDF_OPERATION_ADD;
    edit.position[0] = x;
    edit.position[1] = 0.3f;
    edit.position[2] = z;
    edit.size[0] = 1.5f;
    edit.size[1] = 0.8f;
    edit.size[2] = 1.0f;
    edit.corner_radius = 0.1f;
    /* yaw about y, then pitch about x */
    const float cy = cosf(yaw * 0.5f);
    const float sy = sinf(yaw * 0.5f);
    const float cp = cosf(pitch * 0.5f);
    const float sp = sinf(pitch * 0.5f);
    edit.rotation[0] = cy * sp;
    edit.rotation[1] = sy * cp;
    edit.rotation[2] = -sy * sp;
    edit.rotation[3] = cy * cp;
    edit.material = 3u;
    return edit;
}

/* The road crater and a rubble pile, straddling chunk seams (x = 0 and z = 0 are seams for 32-cell chunks at 0.5). */
static void add_scene(BTBHGPUSdfTerrain *terrain, uint32_t ids[9]) {
    BTBHGPUSdfEdit edit = crater_bowl(0.3f, -0.2f, 3.0f, 1.5f);
    ids[0] = btbh_gpu_sdf_terrain_add_edit(terrain, &edit);
    edit = crater_rim(0.3f, -0.2f, 3.0f);
    ids[1] = btbh_gpu_sdf_terrain_add_edit(terrain, &edit);
    edit = rubble_box(8.0f, 5.0f, 0.5f, 0.35f);
    ids[2] = btbh_gpu_sdf_terrain_add_edit(terrain, &edit);
    edit = rubble_box(9.5f, 6.0f, -0.4f, -0.2f);
    ids[3] = btbh_gpu_sdf_terrain_add_edit(terrain, &edit);
    BTBHGPUSdfEdit trench;
    memset(&trench, 0, sizeof(trench));
    trench.shape = BTBH_GPU_SDF_SHAPE_CAPSULE;
    trench.operation = BTBH_GPU_SDF_OPERATION_SUBTRACT;
    trench.position[0] = -10.0f;
    trench.position[1] = 0.2f;
    trench.position[2] = 8.0f;
    trench.size[0] = 0.9f;
    trench.size[1] = 5.0f;
    trench.rotation[2] = sinf(0.78539816f);  /* a quarter turn about z: lies along x */
    trench.rotation[3] = cosf(0.78539816f);
    trench.smoothing = 0.2f;
    ids[4] = btbh_gpu_sdf_terrain_add_edit(terrain, &trench);
    BTBHGPUSdfEdit paint;
    memset(&paint, 0, sizeof(paint));
    paint.shape = BTBH_GPU_SDF_SHAPE_CYLINDER;
    paint.operation = BTBH_GPU_SDF_OPERATION_PAINT;
    paint.position[0] = -8.0f;
    paint.position[2] = -9.0f;
    paint.size[0] = 2.0f;
    paint.size[1] = 1.0f;
    paint.corner_radius = 0.2f;
    paint.material = 2u;
    ids[5] = btbh_gpu_sdf_terrain_add_edit(terrain, &paint);
    BTBHGPUSdfEdit sphere;
    memset(&sphere, 0, sizeof(sphere));
    sphere.shape = BTBH_GPU_SDF_SHAPE_SPHERE;
    sphere.operation = BTBH_GPU_SDF_OPERATION_ADD;
    sphere.position[0] = 10.0f;
    sphere.position[1] = 0.5f;
    sphere.position[2] = -10.0f;
    sphere.size[0] = 1.8f;
    sphere.smoothing = 0.8f;
    sphere.material = 4u;
    ids[6] = btbh_gpu_sdf_terrain_add_edit(terrain, &sphere);
    /* A floating ball trimmed by INTERSECT into a slab; the box's bounds stay clear of the ground, which it would
     * otherwise clear away within them. */
    BTBHGPUSdfEdit ball = sphere;
    ball.position[0] = -6.0f;
    ball.position[1] = 4.0f;
    ball.position[2] = -2.0f;
    ball.smoothing = 0.0f;
    ball.material = 3u;
    ids[7] = btbh_gpu_sdf_terrain_add_edit(terrain, &ball);
    BTBHGPUSdfEdit cut;
    memset(&cut, 0, sizeof(cut));
    cut.shape = BTBH_GPU_SDF_SHAPE_BOX;
    cut.operation = BTBH_GPU_SDF_OPERATION_INTERSECT;
    cut.position[0] = -6.0f;
    cut.position[1] = 4.0f;
    cut.position[2] = -2.0f;
    cut.size[0] = 1.5f;
    cut.size[1] = 0.8f;
    cut.size[2] = 3.0f;
    cut.material = 5u;
    ids[8] = btbh_gpu_sdf_terrain_add_edit(terrain, &cut);
    for (int index = 0; index < 9; ++index) {
        CHECK(ids[index] != 0u);
    }
}

static BTBHGPUSdfTerrainReport settle(BTBHGPUSdfTerrain *terrain, uint32_t budget, uint32_t *calls) {
    BTBHGPUSdfTerrainReport report;
    uint32_t count = 0u;
    for (;;) {
        CHECK(btbh_gpu_sdf_terrain_update(terrain, budget, &report) == BTBH_GPU_OK);
        count += 1u;
        if (report.pending_bricks == 0u && report.bricks_evaluated == 0u && report.chunks_meshed == 0u) {
            break;
        }
        if (count == 1000u) {
            fprintf(stderr, "settle: still busy after 1000 updates: %u bricks, %u chunks, %u pending, %u live, %u frozen\n",
                report.bricks_evaluated, report.chunks_meshed, report.pending_bricks, report.edit_count,
                report.frozen_edits);
        }
        CHECK(count < 2000u);
    }
    if (calls != NULL) {
        *calls = count;
    }
    return report;
}

static uint64_t scene_hash(uint8_t normals, uint32_t budget, int jobs, uint32_t chunk_cells) {
    BTBHGPUSdfTerrainDesc desc = flat_desc(0.5f, chunk_cells, normals);
    if (jobs) {
        desc.run_jobs = run_jobs_threads;
    }
    BTBHGPUSdfTerrainReport report;
    BTBHGPUSdfTerrain *terrain = btbh_gpu_sdf_terrain_create(&desc, &report);
    CHECK(terrain != NULL && report.result == BTBH_GPU_OK);
    uint32_t ids[9];
    add_scene(terrain, ids);
    uint32_t calls = 0u;
    (void)settle(terrain, budget, &calls);
    const uint64_t hash = btbh_gpu_sdf_terrain_mesh_hash(terrain);
    btbh_gpu_sdf_terrain_destroy(terrain);
    return hash;
}

/* ---------------------------------------------------------------------------------------- mesh helpers */

typedef struct Triangle {
    float v[3][3];
} Triangle;

static uint32_t collect_triangles(const BTBHGPUSdfTerrain *terrain, Triangle **out) {
    uint32_t total = 0u;
    const uint32_t chunks = btbh_gpu_sdf_terrain_chunk_count(terrain);
    for (uint32_t index = 0u; index < chunks; ++index) {
        const float *positions = NULL;
        const uint32_t *indices = NULL;
        uint32_t vertex_count = 0u;
        uint32_t index_count = 0u;
        CHECK(btbh_gpu_sdf_terrain_chunk_geometry(terrain, index, &positions, &vertex_count, &indices, &index_count) ==
              BTBH_GPU_OK);
        total += index_count / 3u;
    }
    Triangle *triangles = (Triangle *)malloc(sizeof(Triangle) * (total > 0u ? total : 1u));
    CHECK(triangles != NULL);
    uint32_t cursor = 0u;
    for (uint32_t index = 0u; index < chunks; ++index) {
        const float *positions = NULL;
        const uint32_t *indices = NULL;
        uint32_t vertex_count = 0u;
        uint32_t index_count = 0u;
        (void)btbh_gpu_sdf_terrain_chunk_geometry(terrain, index, &positions, &vertex_count, &indices, &index_count);
        for (uint32_t i = 0u; i < index_count; i += 3u) {
            for (int k = 0; k < 3; ++k) {
                CHECK(indices[i + (uint32_t)k] < vertex_count);
                memcpy(triangles[cursor].v[k], &positions[indices[i + (uint32_t)k] * 3u], sizeof(float) * 3u);
            }
            cursor += 1u;
        }
    }
    *out = triangles;
    return total;
}

static int compare_vertex(const float *a, const float *b) {
    for (int axis = 0; axis < 3; ++axis) {
        if (a[axis] < b[axis]) {
            return -1;
        }
        if (a[axis] > b[axis]) {
            return 1;
        }
    }
    return 0;
}

/* Rotates a triangle so its smallest vertex comes first (keeping the winding). */
static void canonical(Triangle *triangle) {
    int first = 0;
    for (int k = 1; k < 3; ++k) {
        if (compare_vertex(triangle->v[k], triangle->v[first]) < 0) {
            first = k;
        }
    }
    Triangle copy = *triangle;
    for (int k = 0; k < 3; ++k) {
        memcpy(triangle->v[k], copy.v[(first + k) % 3], sizeof(float) * 3u);
    }
}

static int compare_triangle(const void *a, const void *b) {
    const Triangle *ta = (const Triangle *)a;
    const Triangle *tb = (const Triangle *)b;
    for (int k = 0; k < 3; ++k) {
        const int c = compare_vertex(ta->v[k], tb->v[k]);
        if (c != 0) {
            return c;
        }
    }
    return 0;
}

typedef struct Edge {
    float a[3];
    float b[3];
} Edge;

static int compare_edge(const void *x, const void *y) {
    const Edge *ea = (const Edge *)x;
    const Edge *eb = (const Edge *)y;
    const int c = compare_vertex(ea->a, eb->a);
    return c != 0 ? c : compare_vertex(ea->b, eb->b);
}

/* Edges used by exactly one triangle once vertices are welded by position. */
static uint32_t boundary_edges(const Triangle *triangles, uint32_t count) {
    Edge *edges = (Edge *)malloc(sizeof(Edge) * 3u * (count > 0u ? count : 1u));
    CHECK(edges != NULL);
    for (uint32_t t = 0u; t < count; ++t) {
        for (int k = 0; k < 3; ++k) {
            const float *p = triangles[t].v[k];
            const float *q = triangles[t].v[(k + 1) % 3];
            Edge *edge = &edges[t * 3u + (uint32_t)k];
            if (compare_vertex(p, q) <= 0) {
                memcpy(edge->a, p, sizeof(float) * 3u);
                memcpy(edge->b, q, sizeof(float) * 3u);
            } else {
                memcpy(edge->a, q, sizeof(float) * 3u);
                memcpy(edge->b, p, sizeof(float) * 3u);
            }
        }
    }
    qsort(edges, (size_t)count * 3u, sizeof(Edge), compare_edge);
    uint32_t single = 0u;
    for (uint32_t i = 0u; i < count * 3u;) {
        uint32_t j = i + 1u;
        while (j < count * 3u && compare_edge(&edges[i], &edges[j]) == 0) {
            j += 1u;
        }
        if (j - i == 1u) {
            single += 1u;
        }
        i = j;
    }
    free(edges);
    return single;
}

/* The highest point where the vertical line at (x, z) meets a triangle, or -INFINITY. */
static float mesh_top(const Triangle *triangles, uint32_t count, float x, float z) {
    float best = -INFINITY;
    for (uint32_t t = 0u; t < count; ++t) {
        const float *a = triangles[t].v[0];
        const float *b = triangles[t].v[1];
        const float *c = triangles[t].v[2];
        const float d = (b[2] - c[2]) * (a[0] - c[0]) + (c[0] - b[0]) * (a[2] - c[2]);
        if (fabsf(d) < 1.0e-12f) {
            continue;
        }
        const float l0 = ((b[2] - c[2]) * (x - c[0]) + (c[0] - b[0]) * (z - c[2])) / d;
        const float l1 = ((c[2] - a[2]) * (x - c[0]) + (a[0] - c[0]) * (z - c[2])) / d;
        const float l2 = 1.0f - l0 - l1;
        if (l0 < -1.0e-5f || l1 < -1.0e-5f || l2 < -1.0e-5f) {
            continue;
        }
        const float y = l0 * a[1] + l1 * b[1] + l2 * c[1];
        if (y > best) {
            best = y;
        }
    }
    return best;
}

/* ---------------------------------------------------------------------------------------- tests */

static void test_create_validation(void) {
    BTBHGPUSdfTerrainReport report;
    BTBHGPUSdfTerrainDesc desc = flat_desc(0.5f, 32u, 0u);
    desc.cell_size = 0.05f;
    CHECK(btbh_gpu_sdf_terrain_create(&desc, &report) == NULL && report.result == BTBH_GPU_INVALID_ARGUMENT);
    desc = flat_desc(0.5f, 12u, 0u);
    CHECK(btbh_gpu_sdf_terrain_create(&desc, &report) == NULL && report.result == BTBH_GPU_INVALID_ARGUMENT);
    desc = flat_desc(0.5f, 32u, 0u);
    desc.base_function = NULL;
    CHECK(btbh_gpu_sdf_terrain_create(&desc, &report) == NULL && report.result == BTBH_GPU_INVALID_ARGUMENT);
    CHECK(btbh_gpu_sdf_terrain_create(NULL, &report) == NULL);
    desc = flat_desc(0.5f, 0u, 0u);
    BTBHGPUSdfTerrain *terrain = btbh_gpu_sdf_terrain_create(&desc, &report);
    CHECK(terrain != NULL && report.result == BTBH_GPU_OK && report.mask_bytes == 0u && report.mesh_bytes == 0u);
    /* 128 x 32 x 128 cells in 64-cell chunks: 3 x 1 x 3 (the last one holds the far samples) */
    CHECK(btbh_gpu_sdf_terrain_chunk_count(terrain) == 3u * 1u * 3u);
    BTBHGPUSdfEdit edit = crater_bowl(0.0f, 0.0f, 2.0f, 1.0f);
    edit.material = 8u;
    CHECK(btbh_gpu_sdf_terrain_add_edit(terrain, &edit) == 0u);
    edit = crater_bowl(0.0f, 0.0f, 2.0f, 1.0f);
    edit.size[1] = 0.0f;
    CHECK(btbh_gpu_sdf_terrain_add_edit(terrain, &edit) == 0u);
    edit = crater_bowl(0.0f, 0.0f, 2.0f, 1.0f);
    edit.position[0] = NAN;
    CHECK(btbh_gpu_sdf_terrain_add_edit(terrain, &edit) == 0u);
    edit = crater_bowl(0.0f, 0.0f, 2.0f, 1.0f);
    const uint32_t id = btbh_gpu_sdf_terrain_add_edit(terrain, &edit);
    CHECK(id != 0u);
    CHECK(btbh_gpu_sdf_terrain_set_edit(terrain, id + 1u, &edit) == BTBH_GPU_INVALID_ARGUMENT);
    CHECK(btbh_gpu_sdf_terrain_remove_edit(terrain, id) == BTBH_GPU_OK);
    CHECK(btbh_gpu_sdf_terrain_remove_edit(terrain, id) == BTBH_GPU_INVALID_ARGUMENT);
    CHECK(btbh_gpu_sdf_terrain_set_edit(terrain, id, &edit) == BTBH_GPU_INVALID_ARGUMENT);
    btbh_gpu_sdf_terrain_destroy(terrain);
    printf("create and edit validation: ok\n");
}

static void test_queries(void) {
    BTBHGPUSdfTerrainDesc desc = flat_desc(0.5f, 32u, 0u);
    BTBHGPUSdfTerrainReport report;
    BTBHGPUSdfTerrain *terrain = btbh_gpu_sdf_terrain_create(&desc, &report);
    CHECK(terrain != NULL);
    /* No edit: the base, at once. */
    CHECK(fabsf(btbh_gpu_sdf_terrain_height(terrain, 3.0f, 4.0f, 10.0f)) < 1.0e-6f);
    CHECK(btbh_gpu_sdf_terrain_height(terrain, 3.0f, 4.0f, -1.0f) == -INFINITY);
    const float above[3] = { 3.0f, 2.0f, 4.0f };
    CHECK(fabsf(btbh_gpu_sdf_terrain_distance(terrain, above) - 2.0f) < 1.0e-6f);
    BTBHGPUSdfEdit bowl = crater_bowl(0.0f, 0.0f, 3.0f, 1.5f);
    const uint32_t id = btbh_gpu_sdf_terrain_add_edit(terrain, &bowl);
    CHECK(id != 0u);
    /* Exact and immediate, before any update. */
    const float bottom = btbh_gpu_sdf_terrain_height(terrain, 0.0f, 0.0f, 10.0f);
    CHECK(fabsf(bottom - (0.6f - 1.5f)) < 0.02f);
    CHECK(fabsf(btbh_gpu_sdf_terrain_height(terrain, 20.0f, 20.0f, 10.0f)) < 1.0e-6f);
    const float inside_crater[3] = { 0.0f, -0.5f, 0.0f };
    CHECK(btbh_gpu_sdf_terrain_distance(terrain, inside_crater) > 0.0f);
    float hit[3];
    const float origin[3] = { -10.0f, 5.0f, 0.0f };
    const float toward[3] = { 10.0f, -5.9f, 0.0f };
    CHECK(btbh_gpu_sdf_terrain_raycast(terrain, origin, toward, 40.0f, hit) == 1u);
    CHECK(fabsf(btbh_gpu_sdf_terrain_distance(terrain, hit)) < 0.01f);
    const float sky[3] = { 0.0f, 1.0f, 0.0f };
    CHECK(btbh_gpu_sdf_terrain_raycast(terrain, origin, sky, 40.0f, hit) == 0u);
    float deltas[16 * 16];
    CHECK(btbh_gpu_sdf_terrain_sample_height_deltas(terrain, -8.0f, -8.0f, 1.0f, 16u, 16u, deltas) == BTBH_GPU_OK);
    CHECK(deltas[8 * 16 + 8] < -0.8f);   /* (0.5, 0.5): near the bottom */
    CHECK(deltas[0] == 0.0f);             /* (-7.5, -7.5): untouched */
    /* Moving it is non-destructive. */
    bowl.position[0] = 6.0f;
    CHECK(btbh_gpu_sdf_terrain_set_edit(terrain, id, &bowl) == BTBH_GPU_OK);
    CHECK(fabsf(btbh_gpu_sdf_terrain_height(terrain, 0.0f, 0.0f, 10.0f)) < 1.0e-5f);
    CHECK(btbh_gpu_sdf_terrain_height(terrain, 6.0f, 0.0f, 10.0f) < -0.8f);
    (void)settle(terrain, 0u, NULL);
    float rects[16][4];
    const uint32_t changes = btbh_gpu_sdf_terrain_take_changes(terrain, rects, 16u);
    CHECK(changes > 0u);
    for (uint32_t index = 0u; index < changes; ++index) {
        CHECK(rects[index][0] <= rects[index][2] && rects[index][1] <= rects[index][3]);
        CHECK(rects[index][2] >= 6.0f - 4.0f && rects[index][0] <= 6.0f + 4.0f);
    }
    CHECK(btbh_gpu_sdf_terrain_take_changes(terrain, rects, 16u) == 0u);
    btbh_gpu_sdf_terrain_destroy(terrain);
    printf("queries: ok\n");
}

static void test_determinism(void) {
    for (uint8_t normals = 0u; normals <= 1u; ++normals) {
        const uint64_t reference = scene_hash(normals, 0u, 0, 32u);
        CHECK(reference != scene_hash(normals == 0u ? 1u : 0u, 0u, 0, 32u));
        CHECK(scene_hash(normals, 0u, 1, 32u) == reference);
        CHECK(scene_hash(normals, 1u, 0, 32u) == reference);
        CHECK(scene_hash(normals, 40u, 1, 32u) == reference);
        CHECK(scene_hash(normals, 300u, 0, 32u) == reference);
        printf("determinism (%s): mesh hash %016llx with and without jobs, whatever the budget\n",
            normals ? "faceted" : "smooth", (unsigned long long)reference);
    }
}

/* Chunks that touch switch in the same call: never a half-built crater. */
static void test_group_switch(void) {
    BTBHGPUSdfTerrainDesc desc = flat_desc(0.5f, 16u, 0u);
    BTBHGPUSdfTerrainReport report;
    BTBHGPUSdfTerrain *terrain = btbh_gpu_sdf_terrain_create(&desc, &report);
    CHECK(terrain != NULL);
    BTBHGPUSdfEdit bowl = crater_bowl(0.0f, 0.0f, 6.0f, 2.0f);
    CHECK(btbh_gpu_sdf_terrain_add_edit(terrain, &bowl) != 0u);
    const uint32_t chunks = btbh_gpu_sdf_terrain_chunk_count(terrain);
    uint32_t meshed_before = 0u;
    uint32_t calls = 0u;
    for (;;) {
        CHECK(btbh_gpu_sdf_terrain_update(terrain, 1u, &report) == BTBH_GPU_OK);
        calls += 1u;
        uint32_t meshed = 0u;
        for (uint32_t index = 0u; index < chunks; ++index) {
            const float *positions = NULL;
            const uint32_t *indices = NULL;
            uint32_t vertex_count = 0u;
            uint32_t index_count = 0u;
            (void)btbh_gpu_sdf_terrain_chunk_geometry(terrain, index, &positions, &vertex_count, &indices, &index_count);
            meshed += index_count > 0u ? 1u : 0u;
        }
        CHECK(meshed == 0u || meshed_before == 0u || meshed == meshed_before);
        if (meshed > 0u) {
            CHECK(report.pending_bricks == 0u);
        }
        meshed_before = meshed;
        if (report.pending_bricks == 0u && report.bricks_evaluated == 0u && report.chunks_meshed == 0u) {
            break;
        }
    }
    CHECK(meshed_before >= 4u);
    btbh_gpu_sdf_terrain_destroy(terrain);
    printf("group switch: %u chunks switched together after %u budgeted calls\n", meshed_before, calls);
}

static void test_remove_restores_empty(void) {
    BTBHGPUSdfTerrainDesc desc = flat_desc(0.5f, 32u, 1u);
    BTBHGPUSdfTerrainReport report;
    BTBHGPUSdfTerrain *terrain = btbh_gpu_sdf_terrain_create(&desc, &report);
    CHECK(terrain != NULL);
    const uint64_t empty = btbh_gpu_sdf_terrain_mesh_hash(terrain);
    uint32_t ids[9];
    add_scene(terrain, ids);
    report = settle(terrain, 0u, NULL);
    CHECK(btbh_gpu_sdf_terrain_mesh_hash(terrain) != empty);
    CHECK(btbh_gpu_sdf_terrain_mask_texel_count(terrain) > 0u);
    CHECK(report.mask_bytes > 0u && report.mesh_bytes > 0u && report.brick_count > 0u && report.edit_count == 9u);
    /* The crater digs, so its centre is masked down to the floor; the rubble's top is the patch's. */
    const float centre = btbh_gpu_sdf_terrain_mask_texel(terrain, 64u, 63u);
    CHECK(centre < (8.0f - 0.8f) / 0.5f);
    CHECK(btbh_gpu_sdf_terrain_mask_texel(terrain, 2u, 2u) == 65504.0f);
    for (int index = 8; index >= 0; --index) {
        CHECK(btbh_gpu_sdf_terrain_remove_edit(terrain, ids[index]) == BTBH_GPU_OK);
    }
    report = settle(terrain, 0u, NULL);
    CHECK(btbh_gpu_sdf_terrain_mesh_hash(terrain) == empty);
    CHECK(btbh_gpu_sdf_terrain_mask_texel_count(terrain) == 0u);
    CHECK(report.mask_bytes == 0u && report.mesh_bytes == 0u && report.brick_count == 0u && report.edit_count == 0u);
    CHECK(fabsf(btbh_gpu_sdf_terrain_height(terrain, 0.3f, -0.2f, 10.0f)) < 1.0e-6f);
    btbh_gpu_sdf_terrain_destroy(terrain);
    printf("removing every edit restores an empty mesh set and an empty dig mask: ok\n");
}

/* The mesh lies on the surface: every vertex within half a cell of it (apron vertices sink a twentieth), and where
 * the surface is not steep, height() and the mesh's top agree within half a cell. */
static void check_heights(const BTBHGPUSdfTerrain *terrain, float cell, float x0, float x1, float z0, float z1,
    float *worst_vertex, float *worst_height, uint32_t *compared) {
    Triangle *triangles = NULL;
    const uint32_t count = collect_triangles(terrain, &triangles);
    CHECK(count > 0u);
    *worst_vertex = 0.0f;
    for (uint32_t t = 0u; t < count; ++t) {
        for (int k = 0; k < 3; ++k) {
            const float error = fabsf(btbh_gpu_sdf_terrain_distance(terrain, triangles[t].v[k]));
            *worst_vertex = error > *worst_vertex ? error : *worst_vertex;
        }
    }
    *worst_height = 0.0f;
    *compared = 0u;
    const float step = 0.37f * cell / 0.5f;
    for (float z = z0; z <= z1; z += step) {
        for (float x = x0; x <= x1; x += step) {
            const float top = mesh_top(triangles, count, x, z);
            if (top == -INFINITY) {
                continue;
            }
            const float height = btbh_gpu_sdf_terrain_height(terrain, x, z, 20.0f);
            /* Only where the surface is a plane at the cell's scale, no steeper than 45 degrees: on walls a
             * vertical comparison means little, and surface nets bevel sharp edges and corners (one vertex per
             * cell) by up to about half a cell. */
            const float e = cell;
            const float x_minus = btbh_gpu_sdf_terrain_height(terrain, x - e, z, 20.0f);
            const float x_plus = btbh_gpu_sdf_terrain_height(terrain, x + e, z, 20.0f);
            const float z_minus = btbh_gpu_sdf_terrain_height(terrain, x, z - e, 20.0f);
            const float z_plus = btbh_gpu_sdf_terrain_height(terrain, x, z + e, 20.0f);
            const float hx = x_plus - x_minus;
            const float hz = z_plus - z_minus;
            if (fabsf(hx) > 2.0f * e || fabsf(hz) > 2.0f * e ||
                fabsf(x_plus + x_minus - 2.0f * height) > 0.25f * cell ||
                fabsf(z_plus + z_minus - 2.0f * height) > 0.25f * cell) {
                continue;
            }
            const float error = fabsf(height - top);
            if (error > 0.5f * cell && SDL_getenv("SDF_TEST_DEBUG") != NULL) {
                const float p[3] = { x, top, z };
                printf("  (%.2f, %.2f): height %.3f mesh %.3f, field at the mesh %.3f, slopes %.3f %.3f\n", x, z,
                    height, top, btbh_gpu_sdf_terrain_distance(terrain, p), hx / (2.0f * e), hz / (2.0f * e));
            }
            *worst_height = error > *worst_height ? error : *worst_height;
            *compared += 1u;
        }
    }
    free(triangles);
}

static void test_heights_agree_with_mesh(uint8_t normals) {
    BTBHGPUSdfTerrainDesc desc = flat_desc(0.5f, 32u, normals);
    BTBHGPUSdfTerrainReport report;
    BTBHGPUSdfTerrain *terrain = btbh_gpu_sdf_terrain_create(&desc, &report);
    CHECK(terrain != NULL);
    uint32_t ids[9];
    add_scene(terrain, ids);
    (void)settle(terrain, 0u, NULL);
    float worst_vertex = 0.0f;
    float worst_height = 0.0f;
    uint32_t compared = 0u;
    check_heights(terrain, desc.cell_size, -16.0f, 14.0f, -14.0f, 10.0f, &worst_vertex, &worst_height, &compared);
    CHECK(compared > 500u);
    CHECK(worst_vertex <= 0.5f * desc.cell_size);
    CHECK(worst_height <= 0.5f * desc.cell_size);
    btbh_gpu_sdf_terrain_destroy(terrain);
    printf("heights agree with the %s mesh within %.3f cells at %u points; its vertices lie within %.3f cells of "
           "the surface\n", normals ? "faceted" : "smooth", worst_height / desc.cell_size, compared,
        worst_vertex / desc.cell_size);
}

static void test_seams(void) {
    /* A closed shape (a turned box floating above the ground, on a corner where eight 8-cell chunks meet): no
     * boundary edge at all once the chunks' vertices are welded. */
    BTBHGPUSdfTerrainDesc desc = flat_desc(0.5f, 8u, 0u);
    BTBHGPUSdfTerrainReport report;
    BTBHGPUSdfTerrain *terrain = btbh_gpu_sdf_terrain_create(&desc, &report);
    CHECK(terrain != NULL);
    BTBHGPUSdfEdit box = rubble_box(0.0f, 0.0f, 0.6f, 0.4f);
    box.position[1] = 4.0f;
    CHECK(btbh_gpu_sdf_terrain_add_edit(terrain, &box) != 0u);
    BTBHGPUSdfEdit ball;
    memset(&ball, 0, sizeof(ball));
    ball.shape = BTBH_GPU_SDF_SHAPE_SPHERE;
    ball.position[0] = 1.2f;
    ball.position[1] = 4.6f;
    ball.position[2] = -0.4f;
    ball.size[0] = 1.1f;
    ball.smoothing = 0.5f;
    CHECK(btbh_gpu_sdf_terrain_add_edit(terrain, &ball) != 0u);
    (void)settle(terrain, 0u, NULL);
    Triangle *triangles = NULL;
    const uint32_t count = collect_triangles(terrain, &triangles);
    uint32_t chunks_with_mesh = 0u;
    for (uint32_t index = 0u; index < btbh_gpu_sdf_terrain_chunk_count(terrain); ++index) {
        const float *positions = NULL;
        const uint32_t *indices = NULL;
        uint32_t vertex_count = 0u;
        uint32_t index_count = 0u;
        (void)btbh_gpu_sdf_terrain_chunk_geometry(terrain, index, &positions, &vertex_count, &indices, &index_count);
        chunks_with_mesh += index_count > 0u ? 1u : 0u;
    }
    CHECK(count > 100u && chunks_with_mesh >= 8u);
    CHECK(boundary_edges(triangles, count) == 0u);
    free(triangles);
    btbh_gpu_sdf_terrain_destroy(terrain);

    /* The open scene: the same triangles whatever the chunk size. */
    Triangle *sets[2] = { NULL, NULL };
    uint32_t counts[2] = { 0u, 0u };
    const uint32_t sizes[2] = { 8u, 64u };
    for (int run = 0; run < 2; ++run) {
        desc = flat_desc(0.5f, sizes[run], 0u);
        terrain = btbh_gpu_sdf_terrain_create(&desc, &report);
        CHECK(terrain != NULL);
        uint32_t ids[9];
        add_scene(terrain, ids);
        (void)settle(terrain, 0u, NULL);
        counts[run] = collect_triangles(terrain, &sets[run]);
        for (uint32_t t = 0u; t < counts[run]; ++t) {
            canonical(&sets[run][t]);
        }
        qsort(sets[run], counts[run], sizeof(Triangle), compare_triangle);
        btbh_gpu_sdf_terrain_destroy(terrain);
    }
    CHECK(counts[0] == counts[1] && counts[0] > 0u);
    CHECK(memcmp(sets[0], sets[1], sizeof(Triangle) * counts[0]) == 0);
    const uint32_t open_edges = boundary_edges(sets[0], counts[0]);
    free(sets[0]);
    free(sets[1]);
    printf("seams: a closed shape over 8 chunks has no boundary edge; the scene's %u triangles (%u rim edges) are "
           "the same in 8- and 64-cell chunks\n", counts[0], open_edges);
}

static void test_faceted(void) {
    BTBHGPUSdfTerrainDesc desc = flat_desc(0.5f, 32u, 1u);
    BTBHGPUSdfTerrainReport report;
    BTBHGPUSdfTerrain *terrain = btbh_gpu_sdf_terrain_create(&desc, &report);
    CHECK(terrain != NULL);
    uint32_t ids[9];
    add_scene(terrain, ids);
    (void)settle(terrain, 0u, NULL);
    uint32_t triangles = 0u;
    uint32_t inward = 0u;
    uint32_t materials_seen = 0u;
    for (uint32_t index = 0u; index < btbh_gpu_sdf_terrain_chunk_count(terrain); ++index) {
        const float *positions = NULL;
        const uint32_t *indices = NULL;
        uint32_t vertex_count = 0u;
        uint32_t index_count = 0u;
        (void)btbh_gpu_sdf_terrain_chunk_geometry(terrain, index, &positions, &vertex_count, &indices, &index_count);
        if (index_count == 0u) {
            continue;
        }
        CHECK(vertex_count == index_count);
        const float *normals = btbh_gpu_sdf_terrain_chunk_normals(terrain, index);
        const uint8_t *materials = btbh_gpu_sdf_terrain_chunk_materials(terrain, index);
        CHECK(normals != NULL && materials != NULL);
        for (uint32_t i = 0u; i < index_count; i += 3u) {
            const uint32_t a = indices[i];
            const uint32_t b = indices[i + 1u];
            const uint32_t c = indices[i + 2u];
            CHECK(memcmp(&normals[a * 3u], &normals[b * 3u], sizeof(float) * 3u) == 0);
            CHECK(memcmp(&normals[a * 3u], &normals[c * 3u], sizeof(float) * 3u) == 0);
            CHECK(materials[a] == materials[b] && materials[a] == materials[c]);
            materials_seen |= 1u << materials[a];
            /* The face normal points out of the ground. */
            const float *pa = &positions[a * 3u];
            const float *pb = &positions[b * 3u];
            const float *pc = &positions[c * 3u];
            const float centre[3] = {
                (pa[0] + pb[0] + pc[0]) / 3.0f, (pa[1] + pb[1] + pc[1]) / 3.0f, (pa[2] + pb[2] + pc[2]) / 3.0f
            };
            const float *n = &normals[a * 3u];
            const float out[3] = { centre[0] + n[0] * 0.2f, centre[1] + n[1] * 0.2f, centre[2] + n[2] * 0.2f };
            const float in[3] = { centre[0] - n[0] * 0.2f, centre[1] - n[1] * 0.2f, centre[2] - n[2] * 0.2f };
            if (btbh_gpu_sdf_terrain_distance(terrain, out) < btbh_gpu_sdf_terrain_distance(terrain, in)) {
                inward += 1u;
                if (SDL_getenv("SDF_TEST_DEBUG") != NULL) {
                    printf("  inward: centre (%.2f, %.2f, %.2f) normal (%.2f, %.2f, %.2f) out %.3f in %.3f material %u\n",
                        centre[0], centre[1], centre[2], n[0], n[1], n[2], btbh_gpu_sdf_terrain_distance(terrain, out),
                        btbh_gpu_sdf_terrain_distance(terrain, in), materials[a]);
                }
            }
            triangles += 1u;
        }
    }
    CHECK(triangles > 0u);
    /* Surface nets can fold a triangle over where two sharp boxes meet: a handful at most. */
    CHECK(inward * 200u <= triangles);
    /* asphalt, soil, scorch, rubble, the mound and the intersect's cut all show */
    CHECK((materials_seen & 0x3Fu) == 0x3Fu);
    btbh_gpu_sdf_terrain_destroy(terrain);
    printf("faceted: %u triangles, one normal and one colour each; %u face into the ground (folds at box "
           "corners)\n", triangles, inward);
}

static float crater_height_after(uint32_t max_edits, uint8_t cap, uint32_t *frozen, uint32_t *rejected,
    BTBHGPUSdfTerrain **keep) {
    BTBHGPUSdfTerrainDesc desc = flat_desc(0.5f, 16u, 0u);
    desc.max_edits = max_edits;
    desc.edit_cap = cap;
    BTBHGPUSdfTerrainReport report;
    BTBHGPUSdfTerrain *terrain = btbh_gpu_sdf_terrain_create(&desc, &report);
    CHECK(terrain != NULL);
    uint32_t first = 0u;
    *rejected = 0u;
    for (int index = 0; index < 12; ++index) {
        /* A battle's worth of craters, some on top of each other, with an update now and then. */
        BTBHGPUSdfEdit bowl = crater_bowl(-12.0f + 2.1f * (float)index, 3.0f * sinf((float)index), 2.0f, 1.0f);
        const uint32_t id = btbh_gpu_sdf_terrain_add_edit(terrain, &bowl);
        if (id == 0u) {
            *rejected += 1u;
        }
        if (index == 0) {
            first = id;
        }
        if (index % 3 == 2) {
            (void)btbh_gpu_sdf_terrain_update(terrain, 0u, &report);
        }
    }
    report = settle(terrain, 0u, NULL);
    *frozen = report.frozen_edits;
    if (cap == BTBH_GPU_SDF_EDIT_CAP_FREEZE && max_edits != 0u && max_edits < 12u) {
        CHECK(report.edit_count <= max_edits);
        BTBHGPUSdfEdit bowl = crater_bowl(0.0f, 0.0f, 2.0f, 1.0f);
        CHECK(btbh_gpu_sdf_terrain_set_edit(terrain, first, &bowl) == BTBH_GPU_INVALID_ARGUMENT);
    }
    *keep = terrain;
    return 0.0f;
}

static void test_edit_cap(void) {
    uint32_t frozen = 0u;
    uint32_t rejected = 0u;
    BTBHGPUSdfTerrain *reference = NULL;
    BTBHGPUSdfTerrain *freezing = NULL;
    BTBHGPUSdfTerrain *rejecting = NULL;
    (void)crater_height_after(0u, BTBH_GPU_SDF_EDIT_CAP_FREEZE, &frozen, &rejected, &reference);
    CHECK(frozen == 0u && rejected == 0u);
    (void)crater_height_after(4u, BTBH_GPU_SDF_EDIT_CAP_FREEZE, &frozen, &rejected, &freezing);
    CHECK(frozen >= 8u && rejected == 0u);
    const uint32_t frozen_count = frozen;
    (void)crater_height_after(4u, BTBH_GPU_SDF_EDIT_CAP_REJECT, &frozen, &rejected, &rejecting);
    CHECK(frozen == 0u && rejected == 8u);
    /* Freezing keeps the surface (within int8 precision, interpolated). */
    float worst = 0.0f;
    for (float z = -6.0f; z <= 6.0f; z += 0.29f) {
        for (float x = -16.0f; x <= 16.0f; x += 0.29f) {
            const float a = btbh_gpu_sdf_terrain_height(reference, x, z, 10.0f);
            const float b = btbh_gpu_sdf_terrain_height(freezing, x, z, 10.0f);
            const float error = fabsf(a - b);
            if (error > 0.025f && SDL_getenv("SDF_TEST_DEBUG") != NULL) {
                printf("  frozen (%.2f, %.2f): %.4f against %.4f\n", x, z, b, a);
            }
            worst = error > worst ? error : worst;
        }
    }
    /* Near the surface (within half the clamp distance) the frozen terrain's samples are the reference's within two
     * int8 steps (newer edits apply to the dequantized base). Deeper, a smooth edit blends against the clamped base:
     * the lossy step. The queries read the samples back trilinearly, which bends the surface where it curves within
     * a cell. */
    uint32_t samples_compared = 0u;
    int worst_step = 0;
    for (int32_t z = 40; z <= 88; ++z) {
        for (int32_t y = 0; y <= 32; ++y) {
            for (int32_t x = 20; x <= 108; ++x) {
                int8_t qa = 0;
                int8_t qb = 0;
                uint8_t ma = 0u;
                uint8_t mb = 0u;
                if (btbh_gpu_sdf_terrain_sample(reference, x, y, z, &qa, &ma) &&
                    btbh_gpu_sdf_terrain_sample(freezing, x, y, z, &qb, &mb) && abs((int)qa) < 64) {
                    const int step = abs((int)qa - (int)qb);
                    if (step > 1 && SDL_getenv("SDF_TEST_DEBUG") != NULL) {
                        printf("  sample (%d, %d, %d) world (%.2f, %.2f, %.2f): %d against %d\n", x, y, z,
                            -32.0f + 0.5f * (float)x, -8.0f + 0.5f * (float)y, -32.0f + 0.5f * (float)z, qb, qa);
                    }
                    worst_step = step > worst_step ? step : worst_step;
                    samples_compared += 1u;
                }
            }
        }
    }
    if (SDL_getenv("SDF_TEST_DEBUG") != NULL) {
        printf("  frozen samples: %u compared, worst %d steps; heights worst %.4f cells\n", samples_compared, worst_step,
            worst / 0.5f);
    }
    CHECK(samples_compared > 2000u && worst_step <= 2);
    CHECK(worst < 0.25f * 0.5f);
    /* New edits still apply on a frozen chunk, and removing them restores the frozen base. */
    const float before = btbh_gpu_sdf_terrain_height(freezing, -12.0f, 0.0f, 10.0f);
    BTBHGPUSdfEdit mound;
    memset(&mound, 0, sizeof(mound));
    mound.shape = BTBH_GPU_SDF_SHAPE_SPHERE;
    mound.position[0] = -12.0f;
    mound.position[1] = -0.5f;
    mound.size[0] = 1.5f;
    const uint32_t id = btbh_gpu_sdf_terrain_add_edit(freezing, &mound);
    CHECK(id != 0u);
    CHECK(btbh_gpu_sdf_terrain_height(freezing, -12.0f, 0.0f, 10.0f) > 0.9f);
    (void)settle(freezing, 0u, NULL);
    const uint64_t with_mound = btbh_gpu_sdf_terrain_mesh_hash(freezing);
    CHECK(btbh_gpu_sdf_terrain_remove_edit(freezing, id) == BTBH_GPU_OK);
    CHECK(fabsf(btbh_gpu_sdf_terrain_height(freezing, -12.0f, 0.0f, 10.0f) - before) < 1.0e-5f);
    (void)settle(freezing, 0u, NULL);
    CHECK(btbh_gpu_sdf_terrain_mesh_hash(freezing) != with_mound);
    btbh_gpu_sdf_terrain_destroy(reference);
    btbh_gpu_sdf_terrain_destroy(freezing);
    btbh_gpu_sdf_terrain_destroy(rejecting);
    printf("edit cap: freeze folded %u edits, kept %u samples within %d int8 step and heights within %.3f "
           "cells; reject returned 0 for 8%s", frozen_count, samples_compared, worst_step, worst / 0.5f, NEWLINE);
}

/* CHIMERA's kind of base: a heightfield (a slope and a bump), 0.25 cells, 96-cell chunks. */
static void test_heightfield(void) {
    const uint32_t width = 64u;
    const uint32_t depth = 64u;
    float *heights = (float *)malloc(sizeof(float) * (width + 1u) * (depth + 1u));
    CHECK(heights != NULL);
    for (uint32_t k = 0u; k <= depth; ++k) {
        for (uint32_t i = 0u; i <= width; ++i) {
            const float x = (float)i * 0.5f;
            const float z = (float)k * 0.5f;
            heights[k * (width + 1u) + i] = 0.05f * x + 1.2f * expf(-((x - 20.0f) * (x - 20.0f) + (z - 12.0f) * (z - 12.0f)) / 18.0f);
        }
    }
    BTBHGPUSdfTerrainDesc desc;
    memset(&desc, 0, sizeof(desc));
    desc.bounds_min[1] = -6.0f;
    desc.bounds_max[0] = 32.0f;
    desc.bounds_max[1] = 6.0f;
    desc.bounds_max[2] = 32.0f;
    desc.cell_size = 0.25f;
    desc.chunk_cells = 96u;
    desc.base_heights = heights;
    desc.base_width = width;
    desc.base_depth = depth;
    desc.base_cell = 0.5f;
    desc.base_surface_material = 0u;
    desc.base_subsurface_material = 6u;
    desc.base_subsurface_depth = 0.4f;
    BTBHGPUSdfTerrainReport report;
    BTBHGPUSdfTerrain *terrain = btbh_gpu_sdf_terrain_create(&desc, &report);
    free(heights);
    CHECK(terrain != NULL);
    CHECK(fabsf(btbh_gpu_sdf_terrain_height(terrain, 10.0f, 5.0f, 10.0f) - (0.5f + 1.2f * expf(-149.0f / 18.0f))) < 0.01f);
    const float ground = btbh_gpu_sdf_terrain_height(terrain, 22.0f, 14.0f, 10.0f);
    BTBHGPUSdfEdit bowl = crater_bowl(22.0f, 14.0f, 2.6f, 1.4f);
    bowl.position[1] = ground + 0.6f;
    bowl.material = 5u;
    CHECK(btbh_gpu_sdf_terrain_add_edit(terrain, &bowl) != 0u);
    BTBHGPUSdfEdit rim = crater_rim(22.0f, 14.0f, 2.6f);
    rim.position[1] = ground - 0.25f;
    rim.material = 6u;
    CHECK(btbh_gpu_sdf_terrain_add_edit(terrain, &rim) != 0u);
    (void)settle(terrain, 0u, NULL);
    float worst_vertex = 0.0f;
    float worst = 0.0f;
    uint32_t compared = 0u;
    check_heights(terrain, desc.cell_size, 17.0f, 27.0f, 9.0f, 19.0f, &worst_vertex, &worst, &compared);
    CHECK(compared > 300u && worst <= 0.5f * desc.cell_size && worst_vertex <= 0.5f * desc.cell_size);
    btbh_gpu_sdf_terrain_destroy(terrain);
    printf("heightfield base: heights agree with the mesh within %.3f cells at %u points; vertices within %.3f "
           "cells of the surface\n", worst / 0.25f, compared, worst_vertex / 0.25f);
}

/* Measured costs (no assertion): one crater at CHIMERA's and at BTBH's settings. */
static void measure(const char *name, float cell, uint32_t chunk_cells, uint8_t normals, float radius, float depth,
    int jobs) {
    BTBHGPUSdfTerrainDesc desc = flat_desc(cell, chunk_cells, normals);
    if (jobs) {
        desc.run_jobs = run_jobs_threads;
    }
    BTBHGPUSdfTerrainReport report;
    BTBHGPUSdfTerrain *terrain = btbh_gpu_sdf_terrain_create(&desc, &report);
    CHECK(terrain != NULL);
    BTBHGPUSdfEdit bowl = crater_bowl(1.3f, -0.7f, radius, depth);
    CHECK(btbh_gpu_sdf_terrain_add_edit(terrain, &bowl) != 0u);
    BTBHGPUSdfEdit rim = crater_rim(1.3f, -0.7f, radius);
    rim.size[1] = radius * 0.18f;
    CHECK(btbh_gpu_sdf_terrain_add_edit(terrain, &rim) != 0u);
    /* One work item per call (a 1 us budget), so the time splits into bricks and meshes. */
    uint32_t bricks = 0u;
    uint32_t chunks = 0u;
    double brick_ms = 0.0;
    double mesh_ms = 0.0;
    double other_ms = 0.0;
    for (;;) {
        const uint64_t call_started = SDL_GetTicksNS();
        CHECK(btbh_gpu_sdf_terrain_update(terrain, jobs ? 0u : 1u, &report) == BTBH_GPU_OK);
        const double call_ms = (double)(SDL_GetTicksNS() - call_started) / 1.0e6;
        if (report.chunks_meshed > 0u) {
            mesh_ms += call_ms;
        } else if (report.bricks_evaluated > 0u) {
            brick_ms += call_ms;
        } else {
            other_ms += call_ms;
        }
        bricks += report.bricks_evaluated;
        chunks += report.chunks_meshed;
        if (report.pending_bricks == 0u && report.bricks_evaluated == 0u && report.chunks_meshed == 0u) {
            break;
        }
    }
    const double milliseconds = brick_ms + mesh_ms + other_ms;
    printf("cost %-34s bricks %.2f ms, meshes %.2f ms, the rest %.2f ms\n", name, brick_ms, mesh_ms, other_ms);
    uint32_t vertices = 0u;
    uint32_t indices_total = 0u;
    for (uint32_t index = 0u; index < btbh_gpu_sdf_terrain_chunk_count(terrain); ++index) {
        const float *positions = NULL;
        const uint32_t *indices = NULL;
        uint32_t vertex_count = 0u;
        uint32_t index_count = 0u;
        (void)btbh_gpu_sdf_terrain_chunk_geometry(terrain, index, &positions, &vertex_count, &indices, &index_count);
        vertices += vertex_count;
        indices_total += index_count;
    }
    /* What the GPU would hold: 96-byte vertices, and 4-byte indices for smooth meshes. */
    const double gpu_kib = ((double)vertices * 96.0 + (normals ? 0.0 : (double)indices_total * 4.0)) / 1024.0;
    float q[3] = { 1.3f, 0.0f, -0.7f };
    const uint64_t query_started = SDL_GetTicksNS();
    float sink = 0.0f;
    for (int index = 0; index < 1000; ++index) {
        q[0] = 1.3f + 0.01f * (float)index;
        sink += btbh_gpu_sdf_terrain_height(terrain, q[0], q[2], 10.0f);
    }
    const double query_us = (double)(SDL_GetTicksNS() - query_started) / 1.0e3 / 1000.0;
    const uint64_t far_started = SDL_GetTicksNS();
    for (int index = 0; index < 1000; ++index) {
        sink += btbh_gpu_sdf_terrain_height(terrain, -25.0f + 0.01f * (float)index, 25.0f, 10.0f);
    }
    const double far_us = (double)(SDL_GetTicksNS() - far_started) / 1.0e3 / 1000.0;
    printf("cost %-34s %7.2f ms (%s), %4u bricks (%u kept, %.0f KiB), %u chunks, %u vertices, "
           "CPU mesh %.0f KiB, GPU mesh %.0f KiB, CPU mask %.0f KiB; height %.2f us in it, %.3f us elsewhere%s\n",
        name, milliseconds, jobs ? "4 job threads" : "one thread", bricks, report.brick_count,
        (double)report.brick_count * 1100.0 / 1024.0, chunks, vertices, (double)report.mesh_bytes / 1024.0, gpu_kib,
        (double)report.mask_bytes / 1024.0, query_us, far_us, sink == 12345.0f ? "!" : "");
    btbh_gpu_sdf_terrain_destroy(terrain);
}

int main(void) {
    pool_start();
    test_create_validation();
    test_queries();
    test_determinism();
    test_group_switch();
    test_remove_restores_empty();
    test_heights_agree_with_mesh(0u);
    test_heights_agree_with_mesh(1u);
    test_seams();
    test_faceted();
    test_edit_cap();
    test_heightfield();
    measure("6 m crater, 0.25 m cells, smooth", 0.25f, 96u, 0u, 2.6f, 1.4f, 0);
    measure("6 m crater, 0.25 m cells, faceted", 0.25f, 96u, 1u, 2.6f, 1.4f, 0);
    measure("20 m stomp, 0.5 m cells, faceted", 0.5f, 64u, 1u, 10.0f, 3.0f, 0);
    measure("20 m stomp, 0.5 m cells, faceted", 0.5f, 64u, 1u, 10.0f, 3.0f, 1);
    pool_stop();
    printf("btbh_gpu_sdf_terrain_test: all passed\n");
    return 0;
}
