#include "btbh_gpu_internal.h"

#include <assert.h>

int main(void) {
    assert(btbh_gpu_parse_allowed_frames_in_flight(NULL) == 3u);
    assert(btbh_gpu_parse_allowed_frames_in_flight("") == 3u);
    assert(btbh_gpu_parse_allowed_frames_in_flight("1") == 1u);
    assert(btbh_gpu_parse_allowed_frames_in_flight("2") == 2u);
    assert(btbh_gpu_parse_allowed_frames_in_flight("3") == 3u);
    assert(btbh_gpu_parse_allowed_frames_in_flight("0") == 3u);
    assert(btbh_gpu_parse_allowed_frames_in_flight("4") == 3u);
    assert(btbh_gpu_parse_allowed_frames_in_flight("invalid") == 3u);
    return 0;
}
