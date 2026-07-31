#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#ifndef BTBH_GPU_PUBLIC_HEADER_PATH
#error "BTBH_GPU_PUBLIC_HEADER_PATH must be defined"
#endif

static int contains_forbidden_token(
    const char *source,
    const char *token
) {
    if (strstr(source, token) == NULL) {
        return 0;
    }
    fprintf(stderr, "public GPU header leaks vendor token: %s\n", token);
    return 1;
}

int main(void)
{
    FILE *file = fopen(BTBH_GPU_PUBLIC_HEADER_PATH, "rb");
    char *source;
    long size;
    int failed = 0;

    if (file == NULL ||
        fseek(file, 0, SEEK_END) != 0 ||
        (size = ftell(file)) < 0 ||
        fseek(file, 0, SEEK_SET) != 0) {
        fprintf(stderr, "could not read public GPU header\n");
        if (file != NULL) {
            fclose(file);
        }
        return 1;
    }
    source = (char *)malloc((size_t)size + 1);
    if (source == NULL ||
        fread(source, 1, (size_t)size, file) != (size_t)size) {
        fprintf(stderr, "could not load public GPU header\n");
        free(source);
        fclose(file);
        return 1;
    }
    fclose(file);
    source[size] = '\0';

    failed |= contains_forbidden_token(source, "#include <SDL");
    failed |= contains_forbidden_token(source, "SDL_GPU");
    failed |= contains_forbidden_token(source, "VkInstance");
    failed |= contains_forbidden_token(source, "VkDevice");
    failed |= contains_forbidden_token(source, "MTLDevice");
    failed |= contains_forbidden_token(source, "id<MTL");
    failed |= contains_forbidden_token(
        source,
        "shadow_resources_ready"
    );
    failed |= contains_forbidden_token(
        source,
        "directional_shadow_update_period_frames"
    );
    failed |= contains_forbidden_token(source, "scene_visible");
    failed |= contains_forbidden_token(
        source,
        "directional_shadow_visible"
    );
    failed |= contains_forbidden_token(
        source,
        "animated_geometry_count"
    );
    failed |= contains_forbidden_token(
        source,
        "shadow_pipeline_bind_count"
    );
    failed |= contains_forbidden_token(
        source,
        "cpu_prepare_setup_milliseconds"
    );
    free(source);
    return failed != 0;
}
