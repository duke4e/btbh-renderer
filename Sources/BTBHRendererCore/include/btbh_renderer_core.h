#ifndef BTBH_RENDERER_CORE_H
#define BTBH_RENDERER_CORE_H

#include <stdint.h>

#if defined(__cplusplus)
extern "C" {
#endif

#define BTBH_RENDERER_ABI_VERSION UINT32_C(1)

uint32_t btbh_renderer_abi_version(void);

typedef struct BTBHVec2 { float x, y; } BTBHVec2;
typedef struct BTBHVec3 { float x, y, z; } BTBHVec3;
typedef struct BTBHVec4 { float x, y, z, w; } BTBHVec4;
typedef struct BTBHQuaternion { float x, y, z, w; } BTBHQuaternion;
typedef struct BTBHMatrix4 { float elements[16]; } BTBHMatrix4;
typedef struct BTBHColor { uint8_t r, g, b, a; } BTBHColor;
typedef struct BTBHRect { float x, y, width, height; } BTBHRect;
typedef struct BTBHBounds3 { BTBHVec3 minimum, maximum; } BTBHBounds3;
typedef enum BTBHProjection { BTBH_PROJECTION_PERSPECTIVE = 0, BTBH_PROJECTION_ORTHOGRAPHIC = 1 } BTBHProjection;
typedef struct BTBHCamera {
    BTBHVec3 position;
    BTBHVec3 target;
    BTBHVec3 up;
    float vertical_fov_degrees;
    BTBHProjection projection;
} BTBHCamera;

typedef struct BTBHHandlePool BTBHHandlePool;

typedef enum BTBHHandleResult {
    BTBH_HANDLE_OK = 0,
    BTBH_HANDLE_INVALID_ARGUMENT = 1,
    BTBH_HANDLE_OUT_OF_MEMORY = 2,
    BTBH_HANDLE_EXHAUSTED = 3,
    BTBH_HANDLE_STALE = 4
} BTBHHandleResult;

BTBHHandlePool *btbh_handle_pool_create(uint32_t initial_capacity);
void btbh_handle_pool_destroy(BTBHHandlePool *pool);
BTBHHandleResult btbh_handle_pool_acquire(BTBHHandlePool *pool, uint64_t *out_handle);
int btbh_handle_pool_contains(const BTBHHandlePool *pool, uint64_t handle);
BTBHHandleResult btbh_handle_pool_release(BTBHHandlePool *pool, uint64_t handle);

#if defined(__cplusplus)
}
#endif

#endif
