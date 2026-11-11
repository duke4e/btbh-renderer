/* Models that reference the same external image share one texture with
 * BTBH_GPU_MODEL_IMPORT_SHARE_EXTERNAL_TEXTURES, and each load their own without it.
 *
 * Fixtures (Tests/Fixtures): external-texture-triangle.gltf (albedo placeholder.png),
 * material-factor-triangle.gltf (normal and ORM from placeholder.png) and
 * nested/shared-texture-material.gltf, which reaches placeholder.png through "..", "./" and
 * "name/.." spellings for its albedo, emission, normal and ORM, and has a second material with an
 * embedded PNG. The checks: the same image and format share one texture, sRGB (albedo, emission)
 * never shares with linear (normal, ORM), a composed ORM shares by its pair of images, embedded
 * images stay per model; models destroyed in either order free a shared texture once, with the
 * last of them; btbh_gpu_texture_destroy on a shared texture does nothing; a window destroyed
 * with such models alive frees the shares, and the models' destroys after it touch nothing.
 * The window's texture list is the leak and double-free check.
 *
 * It needs a GPU (a claimed window); with no device it is skipped (77). */
#undef NDEBUG

#include "btbh_gpu/btbh_gpu.h"
#include "btbh_gpu_internal.h"

#include <SDL3/SDL_hints.h>

#include <assert.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#define SMOKE_SKIP 77

#if defined(__APPLE__)
#define SMOKE_BACKEND BTBH_GPU_BACKEND_METAL
#else
#define SMOKE_BACKEND BTBH_GPU_BACKEND_VULKAN
#endif

static char external_path[1024];
static char nested_path[1024];
static char factor_path[1024];
static char dotted_external_path[1024];

static uint32_t texture_count(const BTBHGPUWindow *window) {
    uint32_t count = 0u;
    for (const BTBHGPUTexture *texture = window->texture_head; texture != NULL; texture = texture->next) {
        count += 1u;
    }
    return count;
}

static uint32_t share_count(const BTBHGPUWindow *window) {
    uint32_t count = 0u;
    for (
        const BTBHGPUModelTextureShare *share = window->model_texture_share_head;
        share != NULL;
        share = share->next
    ) {
        count += 1u;
    }
    return count;
}

static bool texture_listed(const BTBHGPUWindow *window, const BTBHGPUTexture *wanted) {
    for (const BTBHGPUTexture *texture = window->texture_head; texture != NULL; texture = texture->next) {
        if (texture == wanted) {
            return true;
        }
    }
    return false;
}

static uint32_t references(const BTBHGPUTexture *texture) {
    return texture != NULL && texture->share != NULL ? texture->share->reference_count : 0u;
}

static BTBHGPUModel *load(BTBHGPUWindow *window, const char *path, uint32_t flags) {
    BTBHGPUModelReport report;
    memset(&report, 0, sizeof(report));
    BTBHGPUModel *model = btbh_gpu_window_model_load_ex(window, path, flags, &report);
    if (model == NULL) {
        fprintf(stderr, "%s: %s\n", path, report.message);
    }
    assert(model != NULL);
    assert(report.result == BTBH_GPU_OK);
    return model;
}

static BTBHGPUModelMaterialInfo material(const BTBHGPUModel *model, uint32_t slot) {
    BTBHGPUModelMaterialInfo info;
    memset(&info, 0, sizeof(info));
    assert(btbh_gpu_model_material_info(model, slot, &info) == BTBH_GPU_OK);
    return info;
}

static BTBHGPUWindow *open_window(BTBHGPUDevice *device) {
    const BTBHGPUWindowDescriptor descriptor = {
        .title = "BTBH model texture share smoke",
        .width = 64,
        .height = 64,
        .hidden = 1,
    };
    BTBHGPUWindowReport report;
    memset(&report, 0, sizeof(report));
    BTBHGPUWindow *window = btbh_gpu_window_create(&descriptor, &report);
    assert(window != NULL);
    assert(btbh_gpu_window_claim(device, window, &report) == BTBH_GPU_OK);
    return window;
}

/* Without the flag: every model its own texture, as before. */
static void check_unshared(BTBHGPUWindow *window) {
    const uint32_t baseline = texture_count(window);
    BTBHGPUModel *first = load(window, external_path, 0u);
    BTBHGPUModel *second = load(window, external_path, 0u);
    const BTBHGPUModelMaterialInfo a = material(first, 1u);
    const BTBHGPUModelMaterialInfo b = material(second, 1u);
    assert(a.albedo_texture != NULL && b.albedo_texture != NULL);
    assert(a.albedo_texture != b.albedo_texture);
    assert(a.albedo_texture->share == NULL && b.albedo_texture->share == NULL);
    assert(share_count(window) == 0u);
    assert(texture_count(window) == baseline + 2u);
    btbh_gpu_model_destroy(first);
    assert(texture_count(window) == baseline + 1u);
    assert(texture_listed(window, b.albedo_texture));
    btbh_gpu_model_destroy(second);
    assert(texture_count(window) == baseline);
}

typedef struct SharedModels {
    BTBHGPUModel *external;  /* sRGB albedo P */
    BTBHGPUModel *nested;    /* sRGB albedo and emission P, linear normal P, ORM (P, P), embedded albedo */
    BTBHGPUModel *factor;    /* linear normal P, ORM (P, P) */
    BTBHGPUModel *dotted;    /* sRGB albedo P, its model path spelled with "/./" (and '\' on Windows) */
    BTBHGPUTexture *srgb;
    BTBHGPUTexture *linear;
    BTBHGPUTexture *orm;
    BTBHGPUTexture *embedded;
} SharedModels;

static SharedModels load_shared(BTBHGPUWindow *window) {
    const uint32_t flags = BTBH_GPU_MODEL_IMPORT_SHARE_EXTERNAL_TEXTURES;
    const uint32_t baseline = texture_count(window);
    SharedModels models;
    memset(&models, 0, sizeof(models));
    models.external = load(window, external_path, flags);
    models.nested = load(window, nested_path, flags);
    models.factor = load(window, factor_path, flags);
    models.dotted = load(window, dotted_external_path, flags);

    const BTBHGPUModelMaterialInfo external = material(models.external, 1u);
    const BTBHGPUModelMaterialInfo nested = material(models.nested, 1u);
    const BTBHGPUModelMaterialInfo embedded = material(models.nested, 2u);
    const BTBHGPUModelMaterialInfo factor = material(models.factor, 1u);
    const BTBHGPUModelMaterialInfo dotted = material(models.dotted, 1u);
    models.srgb = external.albedo_texture;
    models.linear = nested.normal_texture;
    models.orm = nested.orm_texture;
    models.embedded = embedded.albedo_texture;
    assert(models.srgb != NULL && models.linear != NULL && models.orm != NULL && models.embedded != NULL);

    /* One image, one format: one texture, whatever the path's spelling or the slot. */
    assert(nested.albedo_texture == models.srgb);
    assert(nested.emission_texture == models.srgb);
    assert(dotted.albedo_texture == models.srgb);
    assert(factor.normal_texture == models.linear);
    assert(factor.orm_texture == models.orm);
    /* sRGB never shares with linear; a composed ORM never with its source image. */
    assert(models.linear != models.srgb);
    assert(models.srgb->format == SDL_GPU_TEXTUREFORMAT_R8G8B8A8_UNORM_SRGB);
    assert(models.linear->format == SDL_GPU_TEXTUREFORMAT_R8G8B8A8_UNORM);
    assert(models.orm != models.linear && models.orm != models.srgb);
    /* An embedded image stays the model's own. */
    assert(models.embedded != models.srgb);
    assert(models.embedded->share == NULL);

    /* A count per material slot that holds it. */
    assert(references(models.srgb) == 4u);
    assert(references(models.linear) == 2u);
    assert(references(models.orm) == 2u);
    assert(share_count(window) == 3u);
    assert(texture_count(window) == baseline + 4u);
    return models;
}

/* Destroyed in load order, each step checked against the window's textures. */
static void check_shared_in_load_order(BTBHGPUWindow *window) {
    const uint32_t baseline = texture_count(window);
    SharedModels models = load_shared(window);

    /* The host destroying a shared texture from a material info does nothing. */
    btbh_gpu_texture_destroy(models.srgb);
    btbh_gpu_texture_destroy(models.orm);
    assert(texture_count(window) == baseline + 4u);
    assert(references(models.srgb) == 4u && references(models.orm) == 2u);

    btbh_gpu_model_destroy(models.external);
    assert(references(models.srgb) == 3u);
    assert(texture_count(window) == baseline + 4u);
    btbh_gpu_model_destroy(models.nested);
    /* Its two slots on the sRGB texture, the linear and ORM ones, and its embedded texture. */
    assert(references(models.srgb) == 1u);
    assert(references(models.linear) == 1u && references(models.orm) == 1u);
    assert(texture_count(window) == baseline + 3u);
    assert(!texture_listed(window, models.embedded));
    btbh_gpu_model_destroy(models.factor);
    assert(share_count(window) == 1u);
    assert(texture_count(window) == baseline + 1u);
    assert(texture_listed(window, models.srgb));
    btbh_gpu_model_destroy(models.dotted);
    assert(share_count(window) == 0u);
    assert(texture_count(window) == baseline);
}

/* Destroyed the other way round. */
static void check_shared_in_reverse_order(BTBHGPUWindow *window) {
    const uint32_t baseline = texture_count(window);
    SharedModels models = load_shared(window);
    btbh_gpu_model_destroy(models.dotted);
    assert(references(models.srgb) == 3u);
    btbh_gpu_model_destroy(models.factor);
    assert(references(models.linear) == 1u && references(models.orm) == 1u);
    assert(texture_count(window) == baseline + 4u);
    btbh_gpu_model_destroy(models.nested);
    assert(references(models.srgb) == 1u);
    assert(share_count(window) == 1u);
    assert(texture_count(window) == baseline + 1u);
    btbh_gpu_model_destroy(models.external);
    assert(share_count(window) == 0u);
    assert(texture_count(window) == baseline);

    /* Loaded again after all were freed: a fresh share. */
    BTBHGPUModel *again = load(window, external_path, BTBH_GPU_MODEL_IMPORT_SHARE_EXTERNAL_TEXTURES);
    assert(references(material(again, 1u).albedo_texture) == 1u);
    btbh_gpu_model_destroy(again);
    assert(texture_count(window) == baseline);
}

/* Shared and unshared models of the same image side by side. */
static void check_mixed(BTBHGPUWindow *window) {
    const uint32_t baseline = texture_count(window);
    BTBHGPUModel *shared = load(window, external_path, BTBH_GPU_MODEL_IMPORT_SHARE_EXTERNAL_TEXTURES);
    BTBHGPUModel *own = load(window, external_path, 0u);
    BTBHGPUModel *ignored = load(
        window,
        external_path,
        BTBH_GPU_MODEL_IMPORT_SHARE_EXTERNAL_TEXTURES | BTBH_GPU_MODEL_IMPORT_IGNORE_MATERIAL_TEXTURES
    );
    assert(material(shared, 1u).albedo_texture != material(own, 1u).albedo_texture);
    assert(material(own, 1u).albedo_texture->share == NULL);
    assert(material(ignored, 1u).albedo_texture == NULL);
    assert(references(material(shared, 1u).albedo_texture) == 1u);
    assert(texture_count(window) == baseline + 2u);
    btbh_gpu_model_destroy(ignored);
    btbh_gpu_model_destroy(own);
    btbh_gpu_model_destroy(shared);
    assert(texture_count(window) == baseline);
}

int main(void) {
    (void)snprintf(external_path, sizeof(external_path), "%s/external-texture-triangle.gltf",
        BTBH_GPU_FIXTURE_DIR);
    (void)snprintf(nested_path, sizeof(nested_path), "%s/nested/shared-texture-material.gltf",
        BTBH_GPU_FIXTURE_DIR);
    (void)snprintf(factor_path, sizeof(factor_path), "%s/material-factor-triangle.gltf",
        BTBH_GPU_FIXTURE_DIR);
    (void)snprintf(dotted_external_path, sizeof(dotted_external_path), "%s/./nested/../external-texture-triangle.gltf",
        BTBH_GPU_FIXTURE_DIR);
#if defined(_WIN32)
    for (char *cursor = dotted_external_path; *cursor != '\0'; cursor += 1) {
        if (*cursor == '/') {
            *cursor = '\\';
        }
    }
#endif

    (void)SDL_SetHint(SDL_HINT_WINDOW_ACTIVATE_WHEN_SHOWN, "0");
    const BTBHGPUDeviceDescriptor device_descriptor = {
        .backend = SMOKE_BACKEND,
        .required_features = BTBH_GPU_REQUIRED_FEATURES,
        .debug_mode = 1,
    };
    BTBHGPUDeviceReport device_report;
    memset(&device_report, 0, sizeof(device_report));
    BTBHGPUDevice *device = btbh_gpu_device_create(&device_descriptor, &device_report);
    if (device == NULL) {
        fprintf(stderr, "skipped: no device (%s)\n", device_report.message);
        return SMOKE_SKIP;
    }
    BTBHGPUWindow *window = open_window(device);
    const uint32_t baseline = texture_count(window);

    check_unshared(window);
    check_shared_in_load_order(window);
    check_shared_in_reverse_order(window);
    check_mixed(window);
    assert(texture_count(window) == baseline);
    assert(share_count(window) == 0u);

    /* A window destroyed with shared models alive frees their textures; the models' destroys
       after it free only the models. */
    SharedModels left = load_shared(window);
    btbh_gpu_window_destroy(window);
    btbh_gpu_model_destroy(left.dotted);
    btbh_gpu_model_destroy(left.external);
    btbh_gpu_model_destroy(left.nested);
    btbh_gpu_model_destroy(left.factor);

    btbh_gpu_device_destroy(device);
    printf("model texture share smoke passed\n");
    return 0;
}
