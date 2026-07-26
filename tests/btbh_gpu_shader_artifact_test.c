#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#ifndef BTBH_VERTEX_SPIRV_PATH
#error "BTBH_VERTEX_SPIRV_PATH must be defined"
#endif
#ifndef BTBH_VERTEX_MSL_PATH
#error "BTBH_VERTEX_MSL_PATH must be defined"
#endif
#ifndef BTBH_VERTEX_REFLECTION_PATH
#error "BTBH_VERTEX_REFLECTION_PATH must be defined"
#endif
#ifndef BTBH_FRAGMENT_REFLECTION_PATH
#error "BTBH_FRAGMENT_REFLECTION_PATH must be defined"
#endif
#ifndef BTBH_COMPUTE_REFLECTION_PATH
#error "BTBH_COMPUTE_REFLECTION_PATH must be defined"
#endif

static int read_file(const char *path, unsigned char **bytes_out, size_t *size_out)
{
    FILE *file = fopen(path, "rb");
    unsigned char *bytes;
    long size;

    if (file == NULL) {
        fprintf(stderr, "missing shader artifact: %s\n", path);
        return 0;
    }

    if (fseek(file, 0, SEEK_END) != 0 || (size = ftell(file)) < 0 ||
        fseek(file, 0, SEEK_SET) != 0) {
        fclose(file);
        return 0;
    }

    bytes = (unsigned char *)malloc((size_t)size + 1);
    if (bytes == NULL) {
        fclose(file);
        return 0;
    }

    if (fread(bytes, 1, (size_t)size, file) != (size_t)size) {
        free(bytes);
        fclose(file);
        return 0;
    }

    fclose(file);
    bytes[size] = '\0';
    *bytes_out = bytes;
    *size_out = (size_t)size;
    return 1;
}

static int require_text(const char *path, const char *needle)
{
    unsigned char *bytes = NULL;
    size_t size = 0;
    int found;

    if (!read_file(path, &bytes, &size)) {
        return 0;
    }

    (void)size;
    found = strstr((const char *)bytes, needle) != NULL;
    if (!found) {
        fprintf(stderr, "%s does not contain: %s\n", path, needle);
    }
    free(bytes);
    return found;
}

int main(void)
{
    unsigned char *spirv = NULL;
    size_t spirv_size = 0;
    uint32_t spirv_magic = 0;

    if (!read_file(BTBH_VERTEX_SPIRV_PATH, &spirv, &spirv_size) ||
        spirv_size < sizeof(spirv_magic)) {
        return 1;
    }
    memcpy(&spirv_magic, spirv, sizeof(spirv_magic));
    free(spirv);
    if (spirv_magic != UINT32_C(0x07230203)) {
        fprintf(stderr, "invalid SPIR-V magic\n");
        return 1;
    }

    if (!require_text(BTBH_VERTEX_MSL_PATH, "#include <metal_stdlib>") ||
        !require_text(BTBH_VERTEX_REFLECTION_PATH, "\"location\": 0") ||
        !require_text(BTBH_FRAGMENT_REFLECTION_PATH, "\"location\": 2") ||
        !require_text(BTBH_FRAGMENT_REFLECTION_PATH, "SV_Target2") ||
        !require_text(BTBH_COMPUTE_REFLECTION_PATH, "\"readwrite_storage_textures\": 1") ||
        !require_text(BTBH_COMPUTE_REFLECTION_PATH, "\"threadcount_x\": 8") ||
        !require_text(BTBH_COMPUTE_REFLECTION_PATH, "\"threadcount_y\": 8") ||
        !require_text(BTBH_COMPUTE_REFLECTION_PATH, "\"threadcount_z\": 1")) {
        return 1;
    }

    return 0;
}
