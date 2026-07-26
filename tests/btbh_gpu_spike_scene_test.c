#include "btbh_gpu_spike_scene.h"

#include <assert.h>
#include <math.h>
#include <stdint.h>
#include <string.h>

static const BTBHGPUScenePass expected_passes[] = {
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

static const char expected_manifest[] =
    "btbh-gpu-phase2-scene-v1|seed=1112818248|"
    "camera=0,1500,-6000;0,0,0|instances=16|bones=4|"
    "transparent=1|directional=1|spot=1|animation_ms=1250|"
    "passes=upload,directional-shadow,spot-shadow,gbuffer-opaque,"
    "gbuffer-instanced,gbuffer-skinned,compute-pattern,interval-trace,"
    "deferred-lighting,transparent,readback";

int main(void)
{
    const BTBHGPUSceneContract *contract = btbh_gpu_spike_scene_contract();
    BTBHGPUSceneFixture fixture;
    char manifest[512];

    assert(contract != NULL);
    assert(contract->seed == UINT32_C(0x42544248));
    assert(contract->camera_eye_millimeters[0] == 0);
    assert(contract->camera_eye_millimeters[1] == 1500);
    assert(contract->camera_eye_millimeters[2] == -6000);
    assert(contract->camera_target_millimeters[0] == 0);
    assert(contract->camera_target_millimeters[1] == 0);
    assert(contract->camera_target_millimeters[2] == 0);
    assert(contract->instance_count == 16u);
    assert(contract->bone_count == 4u);
    assert(contract->transparent_draw_count == 1u);
    assert(contract->directional_light_count == 1u);
    assert(contract->spot_light_count == 1u);
    assert(contract->animation_time_milliseconds == 1250u);
    assert(
        contract->pass_count ==
        sizeof(expected_passes) / sizeof(expected_passes[0])
    );
    for (size_t index = 0u; index < contract->pass_count; ++index) {
        assert(contract->passes[index] == expected_passes[index]);
        assert(btbh_gpu_spike_pass_name(contract->passes[index]) != NULL);
    }

    assert(
        btbh_gpu_spike_scene_manifest(manifest, sizeof(manifest)) ==
        BTBH_GPU_SCENE_OK
    );
    assert(strcmp(manifest, expected_manifest) == 0);
    assert(
        btbh_gpu_spike_scene_build_fixture(&fixture) == BTBH_GPU_SCENE_OK
    );
    assert(fabsf(fixture.instances[0].translation[0] + 0.72f) < 0.000001f);
    assert(fabsf(fixture.instances[15].translation[1] - 0.72f) < 0.000001f);
    assert(fixture.intervals[0][1] == 0.25f);
    assert(fixture.bones[3][0] == 0.92387950f);
    assert(fixture.bones[3][1] == 0.38268343f);
    assert(fixture.bones[3][4] == -0.38268343f);
    assert(fixture.bones[3][5] == 0.92387950f);
    assert(fixture.bones[3][1] != 0.0f);
    assert(
        btbh_gpu_spike_scene_fixture_signature(&fixture) ==
        btbh_gpu_spike_scene_signature()
    );
    assert(
        btbh_gpu_spike_scene_signature() ==
        UINT64_C(0x7cc30645be466e5f)
    );
    assert(
        btbh_gpu_spike_scene_manifest(manifest, 8u) ==
        BTBH_GPU_SCENE_BUFFER_TOO_SMALL
    );
    return 0;
}
