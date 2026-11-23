#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#ifndef BTBH_PRODUCTION_VERTEX_REFLECTION_PATH
#error "BTBH_PRODUCTION_VERTEX_REFLECTION_PATH must be defined"
#endif
#ifndef BTBH_PRODUCTION_TABLE_VERTEX_REFLECTION_PATH
#error "BTBH_PRODUCTION_TABLE_VERTEX_REFLECTION_PATH must be defined"
#endif
#ifndef BTBH_PRODUCTION_ANIMATED_VERTEX_REFLECTION_PATH
#error "BTBH_PRODUCTION_ANIMATED_VERTEX_REFLECTION_PATH must be defined"
#endif
#ifndef BTBH_PRODUCTION_SHADOW_VERTEX_REFLECTION_PATH
#error "BTBH_PRODUCTION_SHADOW_VERTEX_REFLECTION_PATH must be defined"
#endif
#ifndef BTBH_PRODUCTION_FAST_SHADOW_VERTEX_REFLECTION_PATH
#error "BTBH_PRODUCTION_FAST_SHADOW_VERTEX_REFLECTION_PATH must be defined"
#endif
#ifndef BTBH_PRODUCTION_ANIMATED_SHADOW_VERTEX_REFLECTION_PATH
#error "BTBH_PRODUCTION_ANIMATED_SHADOW_VERTEX_REFLECTION_PATH must be defined"
#endif
#ifndef BTBH_PRODUCTION_FRAGMENT_REFLECTION_PATH
#error "BTBH_PRODUCTION_FRAGMENT_REFLECTION_PATH must be defined"
#endif
#ifndef BTBH_PRODUCTION_2D_SHADOW_FRAGMENT_REFLECTION_PATH
#error "BTBH_PRODUCTION_2D_SHADOW_FRAGMENT_REFLECTION_PATH must be defined"
#endif
#ifndef BTBH_PRODUCTION_UNSHADOWED_FRAGMENT_REFLECTION_PATH
#error "BTBH_PRODUCTION_UNSHADOWED_FRAGMENT_REFLECTION_PATH must be defined"
#endif
#ifndef BTBH_PRODUCTION_UNLIT_FRAGMENT_REFLECTION_PATH
#error "BTBH_PRODUCTION_UNLIT_FRAGMENT_REFLECTION_PATH must be defined"
#endif
#ifndef BTBH_PRODUCTION_RC_MATERIAL_FRAGMENT_REFLECTION_PATH
#error "BTBH_PRODUCTION_RC_MATERIAL_FRAGMENT_REFLECTION_PATH must be defined"
#endif
#ifndef BTBH_PRODUCTION_RC_2D_SHADOW_FRAGMENT_REFLECTION_PATH
#error "BTBH_PRODUCTION_RC_2D_SHADOW_FRAGMENT_REFLECTION_PATH must be defined"
#endif
#ifndef BTBH_PRODUCTION_RC_UNSHADOWED_FRAGMENT_REFLECTION_PATH
#error "BTBH_PRODUCTION_RC_UNSHADOWED_FRAGMENT_REFLECTION_PATH must be defined"
#endif
#ifndef BTBH_PRODUCTION_SSAO_REFLECTION_PATH
#error "BTBH_PRODUCTION_SSAO_REFLECTION_PATH must be defined"
#endif
#ifndef BTBH_PRODUCTION_SSAO_DENOISE_REFLECTION_PATH
#error "BTBH_PRODUCTION_SSAO_DENOISE_REFLECTION_PATH must be defined"
#endif
#ifndef BTBH_PRODUCTION_SSAO_RESOLVE_REFLECTION_PATH
#error "BTBH_PRODUCTION_SSAO_RESOLVE_REFLECTION_PATH must be defined"
#endif
#ifndef BTBH_PRODUCTION_HBAO_REFLECTION_PATH
#error "BTBH_PRODUCTION_HBAO_REFLECTION_PATH must be defined"
#endif
#ifndef BTBH_PRODUCTION_HBAO_DENOISE_REFLECTION_PATH
#error "BTBH_PRODUCTION_HBAO_DENOISE_REFLECTION_PATH must be defined"
#endif
#ifndef BTBH_PRODUCTION_HBAO_RESOLVE_REFLECTION_PATH
#error "BTBH_PRODUCTION_HBAO_RESOLVE_REFLECTION_PATH must be defined"
#endif
#ifndef BTBH_PRODUCTION_RC_SOURCE_VERTEX_REFLECTION_PATH
#error "BTBH_PRODUCTION_RC_SOURCE_VERTEX_REFLECTION_PATH must be defined"
#endif
#ifndef BTBH_PRODUCTION_RC_SOURCE_FRAGMENT_REFLECTION_PATH
#error "BTBH_PRODUCTION_RC_SOURCE_FRAGMENT_REFLECTION_PATH must be defined"
#endif
#ifndef BTBH_PRODUCTION_RC_DIRECTIONAL_SOURCE_VERTEX_REFLECTION_PATH
#error "BTBH_PRODUCTION_RC_DIRECTIONAL_SOURCE_VERTEX_REFLECTION_PATH must be defined"
#endif
#ifndef BTBH_PRODUCTION_RC_DIRECTIONAL_SOURCE_FRAGMENT_REFLECTION_PATH
#error "BTBH_PRODUCTION_RC_DIRECTIONAL_SOURCE_FRAGMENT_REFLECTION_PATH must be defined"
#endif
#ifndef BTBH_PRODUCTION_RC_DIRECT_LIGHT_VERTEX_REFLECTION_PATH
#error "BTBH_PRODUCTION_RC_DIRECT_LIGHT_VERTEX_REFLECTION_PATH must be defined"
#endif
#ifndef BTBH_PRODUCTION_RC_DIRECT_LIGHT_FRAGMENT_REFLECTION_PATH
#error "BTBH_PRODUCTION_RC_DIRECT_LIGHT_FRAGMENT_REFLECTION_PATH must be defined"
#endif
#ifndef BTBH_PRODUCTION_RC_MATERIAL_SOURCE_VERTEX_REFLECTION_PATH
#error "BTBH_PRODUCTION_RC_MATERIAL_SOURCE_VERTEX_REFLECTION_PATH must be defined"
#endif
#ifndef BTBH_PRODUCTION_RC_ANIMATED_MATERIAL_SOURCE_VERTEX_REFLECTION_PATH
#error "BTBH_PRODUCTION_RC_ANIMATED_MATERIAL_SOURCE_VERTEX_REFLECTION_PATH must be defined"
#endif
#ifndef BTBH_PRODUCTION_RC_MATERIAL_SOURCE_FRAGMENT_REFLECTION_PATH
#error "BTBH_PRODUCTION_RC_MATERIAL_SOURCE_FRAGMENT_REFLECTION_PATH must be defined"
#endif
#ifndef BTBH_PRODUCTION_HRC_TRACE_REFLECTION_PATH
#error "BTBH_PRODUCTION_HRC_TRACE_REFLECTION_PATH must be defined"
#endif
#ifndef BTBH_PRODUCTION_HRC_MERGE_UP_REFLECTION_PATH
#error "BTBH_PRODUCTION_HRC_MERGE_UP_REFLECTION_PATH must be defined"
#endif
#ifndef BTBH_PRODUCTION_HRC_MERGE_DOWN_REFLECTION_PATH
#error "BTBH_PRODUCTION_HRC_MERGE_DOWN_REFLECTION_PATH must be defined"
#endif
#ifndef BTBH_PRODUCTION_HRC_REPROJECT_REFLECTION_PATH
#error "BTBH_PRODUCTION_HRC_REPROJECT_REFLECTION_PATH must be defined"
#endif
#ifndef BTBH_PRODUCTION_HRC_FINISH_REFLECTION_PATH
#error "BTBH_PRODUCTION_HRC_FINISH_REFLECTION_PATH must be defined"
#endif
#ifndef BTBH_PRODUCTION_HRC_BLUR_REFLECTION_PATH
#error "BTBH_PRODUCTION_HRC_BLUR_REFLECTION_PATH must be defined"
#endif
#ifndef BTBH_PRODUCTION_HRC_RESOLVE_REFLECTION_PATH
#error "BTBH_PRODUCTION_HRC_RESOLVE_REFLECTION_PATH must be defined"
#endif
#ifndef BTBH_PRODUCTION_HRC_RECONSTRUCT_REFLECTION_PATH
#error "BTBH_PRODUCTION_HRC_RECONSTRUCT_REFLECTION_PATH must be defined"
#endif
#ifndef BTBH_PRODUCTION_RC_COMPOSE_REFLECTION_PATH
#error "BTBH_PRODUCTION_RC_COMPOSE_REFLECTION_PATH must be defined"
#endif
#ifndef BTBH_PRODUCTION_LUT_REFLECTION_PATH
#error "BTBH_PRODUCTION_LUT_REFLECTION_PATH must be defined"
#endif
#ifndef BTBH_PRODUCTION_CAS_REFLECTION_PATH
#error "BTBH_PRODUCTION_CAS_REFLECTION_PATH must be defined"
#endif
#ifndef BTBH_PRODUCTION_TILT_SHIFT_REFLECTION_PATH
#error "BTBH_PRODUCTION_TILT_SHIFT_REFLECTION_PATH must be defined"
#endif
#ifndef BTBH_PRODUCTION_POST_REFLECTION_PATH
#error "BTBH_PRODUCTION_POST_REFLECTION_PATH must be defined"
#endif

static int require_text(const char *path, const char *needle)
{
    FILE *file = fopen(path, "rb");
    char *text;
    long size;
    int found;

    if (file == NULL ||
        fseek(file, 0, SEEK_END) != 0 ||
        (size = ftell(file)) < 0 ||
        fseek(file, 0, SEEK_SET) != 0) {
        fprintf(stderr, "missing Phase 2 shader artifact: %s\n", path);
        if (file != NULL) {
            fclose(file);
        }
        return 0;
    }
    text = (char *)malloc((size_t)size + 1u);
    if (text == NULL ||
        fread(text, 1u, (size_t)size, file) != (size_t)size) {
        free(text);
        fclose(file);
        return 0;
    }
    fclose(file);
    text[size] = '\0';
    found = strstr(text, needle) != NULL;
    if (!found) {
        fprintf(stderr, "%s does not contain: %s\n", path, needle);
    }
    free(text);
    return found;
}

/* 1 when the artifact exists and doesn't contain `needle`. */
static int forbid_text(const char *path, const char *needle)
{
    FILE *file = fopen(path, "rb");
    char *text;
    long size;
    int found;

    if (file == NULL ||
        fseek(file, 0, SEEK_END) != 0 ||
        (size = ftell(file)) < 0 ||
        fseek(file, 0, SEEK_SET) != 0) {
        fprintf(stderr, "missing Phase 2 shader artifact: %s\n", path);
        if (file != NULL) {
            fclose(file);
        }
        return 0;
    }
    text = (char *)malloc((size_t)size + 1u);
    if (text == NULL ||
        fread(text, 1u, (size_t)size, file) != (size_t)size) {
        free(text);
        fclose(file);
        return 0;
    }
    fclose(file);
    text[size] = '\0';
    found = strstr(text, needle) != NULL;
    if (found) {
        fprintf(stderr, "%s contains: %s\n", path, needle);
    }
    free(text);
    return !found;
}

int main(void)
{
    if (!require_text(
            BTBH_PRODUCTION_VERTEX_REFLECTION_PATH,
            "\"name\": \"in.var.TEXCOORD6\", \"type\": \"float4\", \"location\": 4"
        ) ||
        /* RFC 0009: the table's static vertex shader reads the joint slot's first float after the tangent, at
           location 5 (0..4 as in 0.7.0), and passes the index in the flat animated_model (location 8).
           production_model.vert, the animated one and the scene fragment shader read and pass on what 0.7.0's
           did: no location 5, no output or input of its own for the index. */
        !require_text(
            BTBH_PRODUCTION_TABLE_VERTEX_REFLECTION_PATH,
            "\"name\": \"in.var.TEXCOORD6\", \"type\": \"float4\", \"location\": 4"
        ) ||
        !require_text(
            BTBH_PRODUCTION_TABLE_VERTEX_REFLECTION_PATH,
            "\"name\": \"in.var.TEXCOORD7\", \"type\": \"float\", \"location\": 5"
        ) ||
        !require_text(
            BTBH_PRODUCTION_TABLE_VERTEX_REFLECTION_PATH,
            "\"name\": \"out.var.TEXCOORD8\", \"type\": \"float\", \"location\": 8"
        ) ||
        !forbid_text(
            BTBH_PRODUCTION_TABLE_VERTEX_REFLECTION_PATH,
            "TEXCOORD11"
        ) ||
        !forbid_text(
            BTBH_PRODUCTION_VERTEX_REFLECTION_PATH,
            "\"name\": \"in.var.TEXCOORD7\""
        ) ||
        !forbid_text(
            BTBH_PRODUCTION_VERTEX_REFLECTION_PATH,
            "TEXCOORD11"
        ) ||
        !forbid_text(
            BTBH_PRODUCTION_ANIMATED_VERTEX_REFLECTION_PATH,
            "TEXCOORD11"
        ) ||
        !forbid_text(
            BTBH_PRODUCTION_FRAGMENT_REFLECTION_PATH,
            "TEXCOORD11"
        ) ||
        !require_text(
            BTBH_PRODUCTION_FRAGMENT_REFLECTION_PATH,
            "\"name\": \"in.var.TEXCOORD8\", \"type\": \"float\", \"location\": 8"
        ) ||
        !require_text(
            BTBH_PRODUCTION_ANIMATED_VERTEX_REFLECTION_PATH,
            "\"name\": \"in.var.TEXCOORD6\", \"type\": \"float4\", \"location\": 6"
        ) ||
        !require_text(
            BTBH_PRODUCTION_SHADOW_VERTEX_REFLECTION_PATH,
            "\"storage_buffers\": 1"
        ) ||
        !require_text(
            BTBH_PRODUCTION_SHADOW_VERTEX_REFLECTION_PATH,
            "\"uniform_buffers\": 1"
        ) ||
        !require_text(
            BTBH_PRODUCTION_FAST_SHADOW_VERTEX_REFLECTION_PATH,
            "\"storage_buffers\": 0"
        ) ||
        !require_text(
            BTBH_PRODUCTION_FAST_SHADOW_VERTEX_REFLECTION_PATH,
            "\"uniform_buffers\": 1"
        ) ||
        !require_text(
            BTBH_PRODUCTION_FAST_SHADOW_VERTEX_REFLECTION_PATH,
            "\"name\": \"in.var.TEXCOORD1\", \"type\": \"float4\", \"location\": 1"
        ) ||
        !require_text(
            BTBH_PRODUCTION_FAST_SHADOW_VERTEX_REFLECTION_PATH,
            "\"name\": \"in.var.TEXCOORD2\", \"type\": \"float4\", \"location\": 2"
        ) ||
        !require_text(
            BTBH_PRODUCTION_FAST_SHADOW_VERTEX_REFLECTION_PATH,
            "\"name\": \"in.var.TEXCOORD3\", \"type\": \"float4\", \"location\": 3"
        ) ||
        !require_text(
            BTBH_PRODUCTION_FAST_SHADOW_VERTEX_REFLECTION_PATH,
            "\"name\": \"in.var.TEXCOORD4\", \"type\": \"float4\", \"location\": 4"
        ) ||
        !require_text(
            BTBH_PRODUCTION_ANIMATED_SHADOW_VERTEX_REFLECTION_PATH,
            "\"storage_buffers\": 2"
        ) ||
        !require_text(
            BTBH_PRODUCTION_ANIMATED_SHADOW_VERTEX_REFLECTION_PATH,
            "\"name\": \"in.var.TEXCOORD4\", \"type\": \"float4\", \"location\": 2"
        ) ||
        !require_text(
            BTBH_PRODUCTION_ANIMATED_SHADOW_VERTEX_REFLECTION_PATH,
            "\"name\": \"in.var.TEXCOORD5\", \"type\": \"float4\", \"location\": 3"
        ) ||
        !require_text(
            BTBH_PRODUCTION_FRAGMENT_REFLECTION_PATH,
            "\"samplers\": 15"
        ) ||
        !require_text(
            BTBH_PRODUCTION_FRAGMENT_REFLECTION_PATH,
            "\"location\": 2"
        ) ||
        !require_text(
            BTBH_PRODUCTION_2D_SHADOW_FRAGMENT_REFLECTION_PATH,
            "\"samplers\": 14"
        ) ||
        !require_text(
            BTBH_PRODUCTION_UNSHADOWED_FRAGMENT_REFLECTION_PATH,
            "\"samplers\": 10"
        ) ||
        !require_text(
            BTBH_PRODUCTION_UNLIT_FRAGMENT_REFLECTION_PATH,
            "\"samplers\": 3"
        ) ||
        !require_text(
            BTBH_PRODUCTION_RC_MATERIAL_FRAGMENT_REFLECTION_PATH,
            "\"samplers\": 15"
        ) ||
        !require_text(
            BTBH_PRODUCTION_RC_2D_SHADOW_FRAGMENT_REFLECTION_PATH,
            "\"samplers\": 14"
        ) ||
        !require_text(
            BTBH_PRODUCTION_RC_UNSHADOWED_FRAGMENT_REFLECTION_PATH,
            "\"samplers\": 10"
        ) ||
        !require_text(
            BTBH_PRODUCTION_SSAO_REFLECTION_PATH,
            "\"samplers\": 2, \"storage_textures\": 0"
        ) ||
        !require_text(
            BTBH_PRODUCTION_SSAO_DENOISE_REFLECTION_PATH,
            "\"samplers\": 3, \"storage_textures\": 0"
        ) ||
        !require_text(
            BTBH_PRODUCTION_SSAO_RESOLVE_REFLECTION_PATH,
            "\"samplers\": 7, \"storage_textures\": 0"
        ) ||
        !require_text(
            BTBH_PRODUCTION_HBAO_REFLECTION_PATH,
            "\"samplers\": 2, \"storage_textures\": 0"
        ) ||
        !require_text(
            BTBH_PRODUCTION_HBAO_DENOISE_REFLECTION_PATH,
            "\"samplers\": 1, \"storage_textures\": 0"
        ) ||
        !require_text(
            BTBH_PRODUCTION_HBAO_RESOLVE_REFLECTION_PATH,
            "\"samplers\": 6, \"storage_textures\": 0"
        ) ||
        /* RFC 0010: the emitters' records and their tops. */
        !require_text(
            BTBH_PRODUCTION_RC_SOURCE_VERTEX_REFLECTION_PATH,
            "\"storage_buffers\": 2"
        ) ||
        !require_text(
            BTBH_PRODUCTION_RC_SOURCE_VERTEX_REFLECTION_PATH,
            "\"uniform_buffers\": 1"
        ) ||
        !require_text(
            BTBH_PRODUCTION_RC_SOURCE_FRAGMENT_REFLECTION_PATH,
            "\"samplers\": 0, \"storage_textures\": 0"
        ) ||
        !require_text(
            BTBH_PRODUCTION_RC_DIRECTIONAL_SOURCE_VERTEX_REFLECTION_PATH,
            "\"storage_buffers\": 1"
        ) ||
        !require_text(
            BTBH_PRODUCTION_RC_DIRECTIONAL_SOURCE_VERTEX_REFLECTION_PATH,
            "\"uniform_buffers\": 1"
        ) ||
        !require_text(
            BTBH_PRODUCTION_RC_DIRECTIONAL_SOURCE_FRAGMENT_REFLECTION_PATH,
            "\"samplers\": 0, \"storage_textures\": 0"
        ) ||
        !require_text(
            BTBH_PRODUCTION_RC_DIRECT_LIGHT_VERTEX_REFLECTION_PATH,
            "\"storage_buffers\": 1"
        ) ||
        !require_text(
            BTBH_PRODUCTION_RC_DIRECT_LIGHT_VERTEX_REFLECTION_PATH,
            "\"uniform_buffers\": 1"
        ) ||
        !require_text(
            BTBH_PRODUCTION_RC_DIRECT_LIGHT_FRAGMENT_REFLECTION_PATH,
            "\"samplers\": 1, \"storage_textures\": 0"
        ) ||
        !require_text(
            BTBH_PRODUCTION_RC_MATERIAL_SOURCE_VERTEX_REFLECTION_PATH,
            "\"storage_buffers\": 1"
        ) ||
        !require_text(
            BTBH_PRODUCTION_RC_ANIMATED_MATERIAL_SOURCE_VERTEX_REFLECTION_PATH,
            "\"storage_buffers\": 2"
        ) ||
        !require_text(
            BTBH_PRODUCTION_RC_MATERIAL_SOURCE_FRAGMENT_REFLECTION_PATH,
            "\"samplers\": 3, \"storage_textures\": 0"
        ) ||
        /* RFC 0007: the dig mask's placement is the fragment stage's second uniform buffer (RFC 0008: with the
           world detail's after it, in the scene shaders; the scene shaders' samplers above end with the dig mask,
           the world detail's three and RFC 0009's table array). RFC 0009: the table's entries are the scene
           shaders' third; the fast unlit and RC source shaders keep two. */
        !require_text(
            BTBH_PRODUCTION_FRAGMENT_REFLECTION_PATH,
            "\"uniform_buffers\": 3"
        ) ||
        !require_text(
            BTBH_PRODUCTION_UNLIT_FRAGMENT_REFLECTION_PATH,
            "\"uniform_buffers\": 2"
        ) ||
        !require_text(
            BTBH_PRODUCTION_RC_MATERIAL_SOURCE_FRAGMENT_REFLECTION_PATH,
            "\"uniform_buffers\": 2"
        ) ||
        !require_text(
            BTBH_PRODUCTION_2D_SHADOW_FRAGMENT_REFLECTION_PATH,
            "\"uniform_buffers\": 3"
        ) ||
        !require_text(
            BTBH_PRODUCTION_UNSHADOWED_FRAGMENT_REFLECTION_PATH,
            "\"uniform_buffers\": 3"
        ) ||
        !require_text(
            BTBH_PRODUCTION_RC_MATERIAL_FRAGMENT_REFLECTION_PATH,
            "\"uniform_buffers\": 3"
        ) ||
        /* RFC 0010: the trace's and the finish's last texture is the sources' tops. */
        !require_text(
            BTBH_PRODUCTION_HRC_TRACE_REFLECTION_PATH,
            "\"samplers\": 4"
        ) ||
        !require_text(
            BTBH_PRODUCTION_HRC_MERGE_UP_REFLECTION_PATH,
            "\"samplers\": 1"
        ) ||
        !require_text(
            BTBH_PRODUCTION_HRC_MERGE_DOWN_REFLECTION_PATH,
            "\"samplers\": 3"
        ) ||
        !require_text(
            BTBH_PRODUCTION_HRC_REPROJECT_REFLECTION_PATH,
            "\"samplers\": 3"
        ) ||
        !require_text(
            BTBH_PRODUCTION_HRC_REPROJECT_REFLECTION_PATH,
            "\"readwrite_storage_textures\": 1"
        ) ||
        !require_text(
            BTBH_PRODUCTION_HRC_FINISH_REFLECTION_PATH,
            "\"samplers\": 5"
        ) ||
        !require_text(
            BTBH_PRODUCTION_HRC_BLUR_REFLECTION_PATH,
            "\"samplers\": 1"
        ) ||
        !require_text(
            BTBH_PRODUCTION_HRC_RESOLVE_REFLECTION_PATH,
            "\"samplers\": 8"
        ) ||
        !require_text(
            BTBH_PRODUCTION_HRC_RESOLVE_REFLECTION_PATH,
            "\"readwrite_storage_textures\": 1"
        ) ||
        !require_text(
            BTBH_PRODUCTION_HRC_RECONSTRUCT_REFLECTION_PATH,
            "\"samplers\": 3"
        ) ||
        !require_text(
            BTBH_PRODUCTION_HRC_RECONSTRUCT_REFLECTION_PATH,
            "\"readwrite_storage_textures\": 1"
        ) ||
        !require_text(
            BTBH_PRODUCTION_RC_COMPOSE_REFLECTION_PATH,
            "\"samplers\": 8, \"storage_textures\": 0"
        ) ||
        !require_text(
            BTBH_PRODUCTION_LUT_REFLECTION_PATH,
            "\"samplers\": 2, \"storage_textures\": 0"
        ) ||
        !require_text(
            BTBH_PRODUCTION_CAS_REFLECTION_PATH,
            "\"samplers\": 1, \"storage_textures\": 0"
        ) ||
        !require_text(
            BTBH_PRODUCTION_TILT_SHIFT_REFLECTION_PATH,
            "\"samplers\": 1, \"storage_textures\": 0"
        ) ||
        !require_text(
            BTBH_PRODUCTION_POST_REFLECTION_PATH,
            "\"samplers\": 1, \"storage_textures\": 0"
        ) ||
        !require_text(
            BTBH_PRODUCTION_FRAGMENT_REFLECTION_PATH,
            "\"name\": \"in.var.TEXCOORD4\""
        )) {
        return 1;
    }
    return 0;
}
