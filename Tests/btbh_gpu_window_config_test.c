#include "btbh_gpu_internal.h"

#include <assert.h>

int main(void) {
#if defined(__APPLE__)
    const uint32_t platform_default = 2u;
#else
    const uint32_t platform_default = 3u;
#endif
    assert(
        btbh_gpu_parse_allowed_frames_in_flight(NULL) ==
        platform_default
    );
    assert(
        btbh_gpu_parse_allowed_frames_in_flight("") ==
        platform_default
    );
    assert(btbh_gpu_parse_allowed_frames_in_flight("1") == 1u);
    assert(btbh_gpu_parse_allowed_frames_in_flight("2") == 2u);
    assert(btbh_gpu_parse_allowed_frames_in_flight("3") == 3u);
    assert(
        btbh_gpu_parse_allowed_frames_in_flight("0") ==
        platform_default
    );
    assert(
        btbh_gpu_parse_allowed_frames_in_flight("4") ==
        platform_default
    );
    assert(
        btbh_gpu_parse_allowed_frames_in_flight("invalid") ==
        platform_default
    );
    return 0;
}
