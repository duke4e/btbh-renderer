#include "btbh_gpu_internal.h"

#include <assert.h>
#include <stdlib.h>

int main(void)
{
    const uint8_t metallic_roughness[8] = {
        10u, 20u, 30u, 40u,
        50u, 60u, 70u, 80u
    };
    uint8_t *composed = NULL;
    uint32_t width = 0u;
    uint32_t height = 0u;

    assert(btbh_gpu_model_compose_orm_rgba8(
        NULL,
        0u,
        0u,
        metallic_roughness,
        2u,
        1u,
        &composed,
        &width,
        &height
    ));
    assert(width == 2u);
    assert(height == 1u);
    assert(composed[0] == 255u);
    assert(composed[1] == 20u);
    assert(composed[2] == 30u);
    assert(composed[3] == 255u);
    assert(composed[4] == 255u);
    assert(composed[5] == 60u);
    assert(composed[6] == 70u);
    assert(composed[7] == 255u);
    free(composed);

    const uint8_t occlusion[4] = {90u, 1u, 2u, 3u};
    composed = NULL;
    assert(btbh_gpu_model_compose_orm_rgba8(
        occlusion,
        1u,
        1u,
        metallic_roughness,
        2u,
        1u,
        &composed,
        &width,
        &height
    ));
    assert(width == 2u);
    assert(height == 1u);
    assert(composed[0] == 90u);
    assert(composed[1] == 20u);
    assert(composed[2] == 30u);
    assert(composed[4] == 90u);
    assert(composed[5] == 60u);
    assert(composed[6] == 70u);
    free(composed);

    composed = NULL;
    assert(btbh_gpu_model_compose_orm_rgba8(
        occlusion,
        1u,
        1u,
        NULL,
        0u,
        0u,
        &composed,
        &width,
        &height
    ));
    assert(width == 1u);
    assert(height == 1u);
    assert(composed[0] == 90u);
    assert(composed[1] == 255u);
    assert(composed[2] == 255u);
    assert(composed[3] == 255u);
    free(composed);

    return 0;
}
