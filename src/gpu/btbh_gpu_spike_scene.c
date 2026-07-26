#include "btbh_gpu_spike_scene.h"

#include <string.h>

static const BTBHGPUScenePass btbh_scene_passes[] = {
    BTBH_GPU_SCENE_PASS_UPLOAD,
    BTBH_GPU_SCENE_PASS_DIRECTIONAL_SHADOW,
    BTBH_GPU_SCENE_PASS_SPOT_SHADOW,
    BTBH_GPU_SCENE_PASS_GBUFFER_OPAQUE,
    BTBH_GPU_SCENE_PASS_GBUFFER_INSTANCED,
    BTBH_GPU_SCENE_PASS_GBUFFER_SKINNED,
    BTBH_GPU_SCENE_PASS_COMPUTE_PATTERN,
    BTBH_GPU_SCENE_PASS_INTERVAL_TRACE,
    BTBH_GPU_SCENE_PASS_DEFERRED_LIGHTING,
    BTBH_GPU_SCENE_PASS_TRANSPARENT,
    BTBH_GPU_SCENE_PASS_READBACK
};

static const BTBHGPUSceneContract btbh_scene_contract = {
    UINT32_C(0x42544248),
    { 0, 1500, -6000 },
    { 0, 0, 0 },
    16u,
    4u,
    1u,
    1u,
    1u,
    1250u,
    btbh_scene_passes,
    sizeof(btbh_scene_passes) / sizeof(btbh_scene_passes[0])
};

static const char btbh_scene_manifest[] =
    "btbh-gpu-phase2-scene-v1|seed=1112818248|"
    "camera=0,1500,-6000;0,0,0|instances=16|bones=4|"
    "transparent=1|directional=1|spot=1|animation_ms=1250|"
    "passes=upload,directional-shadow,spot-shadow,gbuffer-opaque,"
    "gbuffer-instanced,gbuffer-skinned,compute-pattern,interval-trace,"
    "deferred-lighting,transparent,readback";

static void btbh_scene_identity(float matrix[16])
{
    (void)memset(matrix, 0, sizeof(float) * 16u);
    matrix[0] = 1.0f;
    matrix[5] = 1.0f;
    matrix[10] = 1.0f;
    matrix[15] = 1.0f;
}

static void btbh_scene_hash_byte(uint64_t *hash, uint8_t value)
{
    *hash ^= (uint64_t)value;
    *hash *= UINT64_C(1099511628211);
}

static void btbh_scene_hash_u32(uint64_t *hash, uint32_t value)
{
    btbh_scene_hash_byte(hash, (uint8_t)(value & UINT32_C(0xff)));
    btbh_scene_hash_byte(hash, (uint8_t)((value >> 8u) & UINT32_C(0xff)));
    btbh_scene_hash_byte(hash, (uint8_t)((value >> 16u) & UINT32_C(0xff)));
    btbh_scene_hash_byte(hash, (uint8_t)((value >> 24u) & UINT32_C(0xff)));
}

static void btbh_scene_hash_float(uint64_t *hash, float value)
{
    uint32_t bits;
    (void)memcpy(&bits, &value, sizeof(bits));
    btbh_scene_hash_u32(hash, bits);
}

const BTBHGPUSceneContract *btbh_gpu_spike_scene_contract(void)
{
    return &btbh_scene_contract;
}

const char *btbh_gpu_spike_pass_name(BTBHGPUScenePass pass)
{
    switch (pass) {
        case BTBH_GPU_SCENE_PASS_UPLOAD:
            return "upload";
        case BTBH_GPU_SCENE_PASS_DIRECTIONAL_SHADOW:
            return "directional-shadow";
        case BTBH_GPU_SCENE_PASS_SPOT_SHADOW:
            return "spot-shadow";
        case BTBH_GPU_SCENE_PASS_GBUFFER_OPAQUE:
            return "gbuffer-opaque";
        case BTBH_GPU_SCENE_PASS_GBUFFER_INSTANCED:
            return "gbuffer-instanced";
        case BTBH_GPU_SCENE_PASS_GBUFFER_SKINNED:
            return "gbuffer-skinned";
        case BTBH_GPU_SCENE_PASS_COMPUTE_PATTERN:
            return "compute-pattern";
        case BTBH_GPU_SCENE_PASS_INTERVAL_TRACE:
            return "interval-trace";
        case BTBH_GPU_SCENE_PASS_DEFERRED_LIGHTING:
            return "deferred-lighting";
        case BTBH_GPU_SCENE_PASS_TRANSPARENT:
            return "transparent";
        case BTBH_GPU_SCENE_PASS_READBACK:
            return "readback";
        case BTBH_GPU_SCENE_PASS_INVALID:
        default:
            return NULL;
    }
}

BTBHGPUSceneResult btbh_gpu_spike_scene_manifest(
    char *buffer,
    size_t capacity
) {
    const size_t required = sizeof(btbh_scene_manifest);

    if (buffer == NULL) {
        return BTBH_GPU_SCENE_INVALID_ARGUMENT;
    }
    if (capacity < required) {
        return BTBH_GPU_SCENE_BUFFER_TOO_SMALL;
    }
    (void)memcpy(buffer, btbh_scene_manifest, required);
    return BTBH_GPU_SCENE_OK;
}

BTBHGPUSceneResult btbh_gpu_spike_scene_build_fixture(
    BTBHGPUSceneFixture *fixture
)
{
    const BTBHGPUSceneContract *contract = &btbh_scene_contract;
    const float grid_step =
        (float)(-contract->camera_eye_millimeters[2]) / 12500.0f;
    const float grid_origin = grid_step * -1.5f;
    const float red_base = (float)(contract->seed & UINT32_C(0xff)) / 400.0f;
    const float green_base =
        (float)((contract->seed >> 8u) & UINT32_C(0xff)) / 300.0f;
    const float blue_base =
        (float)(((contract->seed >> 16u) & UINT32_C(0xff)) - 2u) / 100.0f;
    const float interval_width =
        (float)contract->animation_time_milliseconds / 5000.0f;

    if (fixture == NULL) {
        return BTBH_GPU_SCENE_INVALID_ARGUMENT;
    }

    (void)memset(fixture, 0, sizeof(*fixture));
    for (uint32_t index = 0u;
         index < BTBH_GPU_SCENE_INSTANCE_COUNT;
         ++index) {
        const uint32_t column = index & 3u;
        const uint32_t row = index >> 2u;
        fixture->instances[index].translation[0] =
            grid_origin + (float)column * grid_step;
        fixture->instances[index].translation[1] =
            grid_origin + (float)row * grid_step;
        fixture->instances[index].translation[2] =
            (float)(contract->camera_eye_millimeters[1] -
                    contract->camera_target_millimeters[1]) / 3333.3333f;
        fixture->instances[index].translation[3] = 0.0f;
        fixture->instances[index].color[0] =
            red_base + (float)column * red_base;
        fixture->instances[index].color[1] =
            green_base + (float)row * 0.16f;
        fixture->instances[index].color[2] =
            blue_base - (float)column * 0.12f;
        fixture->instances[index].color[3] = 1.0f;
        fixture->intervals[index][0] = (float)index / 32.0f;
        fixture->intervals[index][1] =
            fixture->intervals[index][0] + interval_width;
        fixture->intervals[index][2] = (float)(index & 3u) / 3.0f;
        fixture->intervals[index][3] = 1.0f;
    }

    for (uint32_t index = 0u; index < BTBH_GPU_SCENE_BONE_COUNT; ++index) {
        btbh_scene_identity(fixture->bones[index]);
    }

    /*
     * The fixed 1250 ms sample selects the 22.5-degree lookup entry. Using
     * canonical constants avoids libm variance between Vulkan and Metal.
     */
    if ((contract->animation_time_milliseconds / 250u) % 4u == 1u) {
        fixture->bones[3][0] = 0.92387950f;
        fixture->bones[3][1] = 0.38268343f;
        fixture->bones[3][4] = -0.38268343f;
        fixture->bones[3][5] = 0.92387950f;
    }
    return BTBH_GPU_SCENE_OK;
}

uint64_t btbh_gpu_spike_scene_fixture_signature(
    const BTBHGPUSceneFixture *fixture
) {
    const unsigned char *cursor = (const unsigned char *)btbh_scene_manifest;
    uint64_t hash = UINT64_C(14695981039346656037);

    if (fixture == NULL) {
        return 0u;
    }

    while (*cursor != '\0') {
        btbh_scene_hash_byte(&hash, *cursor);
        ++cursor;
    }
    btbh_scene_hash_byte(&hash, 0u);

    for (uint32_t instance = 0u;
         instance < BTBH_GPU_SCENE_INSTANCE_COUNT;
         ++instance) {
        for (uint32_t component = 0u; component < 4u; ++component) {
            btbh_scene_hash_float(
                &hash,
                fixture->instances[instance].translation[component]
            );
        }
        for (uint32_t component = 0u; component < 4u; ++component) {
            btbh_scene_hash_float(
                &hash,
                fixture->instances[instance].color[component]
            );
        }
    }
    for (uint32_t bone = 0u; bone < BTBH_GPU_SCENE_BONE_COUNT; ++bone) {
        for (uint32_t component = 0u; component < 16u; ++component) {
            btbh_scene_hash_float(&hash, fixture->bones[bone][component]);
        }
    }
    for (uint32_t interval = 0u;
         interval < BTBH_GPU_SCENE_INTERVAL_COUNT;
         ++interval) {
        for (uint32_t component = 0u; component < 4u; ++component) {
            btbh_scene_hash_float(
                &hash,
                fixture->intervals[interval][component]
            );
        }
    }
    return hash;
}

uint64_t btbh_gpu_spike_scene_signature(void)
{
    BTBHGPUSceneFixture fixture;
    if (btbh_gpu_spike_scene_build_fixture(&fixture) != BTBH_GPU_SCENE_OK) {
        return 0u;
    }
    return btbh_gpu_spike_scene_fixture_signature(&fixture);
}
