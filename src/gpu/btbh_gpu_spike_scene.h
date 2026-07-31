#ifndef BTBH_GPU_SPIKE_SCENE_H
#define BTBH_GPU_SPIKE_SCENE_H

#include <stddef.h>
#include <stdint.h>

#define BTBH_GPU_SCENE_INSTANCE_COUNT 16u
#define BTBH_GPU_SCENE_BONE_COUNT 4u
#define BTBH_GPU_SCENE_INTERVAL_COUNT 16u

typedef enum BTBHGPUSceneResult {
    BTBH_GPU_SCENE_OK = 0,
    BTBH_GPU_SCENE_INVALID_ARGUMENT = 1,
    BTBH_GPU_SCENE_BUFFER_TOO_SMALL = 2
} BTBHGPUSceneResult;

typedef enum BTBHGPUScenePass {
    BTBH_GPU_SCENE_PASS_INVALID = 0,
    BTBH_GPU_SCENE_PASS_UPLOAD = 1,
    BTBH_GPU_SCENE_PASS_DIRECTIONAL_SHADOW = 2,
    BTBH_GPU_SCENE_PASS_SPOT_SHADOW = 3,
    BTBH_GPU_SCENE_PASS_GBUFFER_OPAQUE = 4,
    BTBH_GPU_SCENE_PASS_GBUFFER_INSTANCED = 5,
    BTBH_GPU_SCENE_PASS_GBUFFER_SKINNED = 6,
    BTBH_GPU_SCENE_PASS_COMPUTE_PATTERN = 7,
    BTBH_GPU_SCENE_PASS_INTERVAL_TRACE = 8,
    BTBH_GPU_SCENE_PASS_DEFERRED_LIGHTING = 9,
    BTBH_GPU_SCENE_PASS_TRANSPARENT = 10,
    BTBH_GPU_SCENE_PASS_READBACK = 11
} BTBHGPUScenePass;

typedef struct BTBHGPUSceneContract {
    uint32_t seed;
    int32_t camera_eye_millimeters[3];
    int32_t camera_target_millimeters[3];
    uint32_t instance_count;
    uint32_t bone_count;
    uint32_t transparent_draw_count;
    uint32_t directional_light_count;
    uint32_t spot_light_count;
    uint32_t animation_time_milliseconds;
    const BTBHGPUScenePass *passes;
    size_t pass_count;
} BTBHGPUSceneContract;

typedef struct BTBHGPUSceneInstance {
    float translation[4];
    float color[4];
} BTBHGPUSceneInstance;

typedef struct BTBHGPUSceneFixture {
    BTBHGPUSceneInstance instances[BTBH_GPU_SCENE_INSTANCE_COUNT];
    float bones[BTBH_GPU_SCENE_BONE_COUNT][16];
    float intervals[BTBH_GPU_SCENE_INTERVAL_COUNT][4];
} BTBHGPUSceneFixture;

const BTBHGPUSceneContract *btbh_gpu_spike_scene_contract(void);

const char *btbh_gpu_spike_pass_name(BTBHGPUScenePass pass);

BTBHGPUSceneResult btbh_gpu_spike_scene_manifest(
    char *buffer,
    size_t capacity
);

BTBHGPUSceneResult btbh_gpu_spike_scene_build_fixture(
    BTBHGPUSceneFixture *fixture
);

uint64_t btbh_gpu_spike_scene_fixture_signature(
    const BTBHGPUSceneFixture *fixture
);

uint64_t btbh_gpu_spike_scene_signature(void);

#endif
