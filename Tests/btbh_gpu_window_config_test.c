/* The checks run through assert(), so keep them in Release builds too. */
#undef NDEBUG

#include "btbh_gpu_internal.h"

#include <assert.h>

/* What a window takes for each request, given whether it supports IMMEDIATE and MAILBOX (VSYNC
   always): the default keeps the choice before 0.3.0, a request falls back to VSYNC. */
static void test_present_mode_choice(void) {
    for (int immediate = 0; immediate < 2; immediate += 1) {
        for (int mailbox = 0; mailbox < 2; mailbox += 1) {
            const bool has_immediate = immediate != 0;
            const bool has_mailbox = mailbox != 0;
            const BTBHGPUPresentMode fastest =
                has_immediate ? BTBH_GPU_PRESENT_MODE_IMMEDIATE
                : has_mailbox ? BTBH_GPU_PRESENT_MODE_MAILBOX
                : BTBH_GPU_PRESENT_MODE_VSYNC;
            assert(
                btbh_gpu_resolve_present_mode(
                    BTBH_GPU_PRESENT_MODE_INVALID,
                    has_immediate,
                    has_mailbox
                ) == fastest
            );
            assert(
                btbh_gpu_resolve_present_mode(
                    BTBH_GPU_PRESENT_MODE_VSYNC,
                    has_immediate,
                    has_mailbox
                ) == BTBH_GPU_PRESENT_MODE_VSYNC
            );
            assert(
                btbh_gpu_resolve_present_mode(
                    BTBH_GPU_PRESENT_MODE_IMMEDIATE,
                    has_immediate,
                    has_mailbox
                ) == (has_immediate
                    ? BTBH_GPU_PRESENT_MODE_IMMEDIATE
                    : BTBH_GPU_PRESENT_MODE_VSYNC)
            );
            assert(
                btbh_gpu_resolve_present_mode(
                    BTBH_GPU_PRESENT_MODE_MAILBOX,
                    has_immediate,
                    has_mailbox
                ) == (has_mailbox
                    ? BTBH_GPU_PRESENT_MODE_MAILBOX
                    : BTBH_GPU_PRESENT_MODE_VSYNC)
            );
            /* Values that aren't a BTBHGPUPresentMode aren't requests. */
            assert(
                btbh_gpu_resolve_present_mode(-1, has_immediate, has_mailbox) ==
                BTBH_GPU_PRESENT_MODE_INVALID
            );
            assert(
                btbh_gpu_resolve_present_mode(4, has_immediate, has_mailbox) ==
                BTBH_GPU_PRESENT_MODE_INVALID
            );
            assert(
                btbh_gpu_resolve_present_mode(255, has_immediate, has_mailbox) ==
                BTBH_GPU_PRESENT_MODE_INVALID
            );
        }
    }
}

/* An unknown present mode fails the create (before SDL is touched), and the runtime switch
   rejects it without a window. */
static void test_present_mode_arguments(void) {
    BTBHGPUWindowDescriptor descriptor = {0};
    descriptor.title = "present mode";
    descriptor.width = 64u;
    descriptor.height = 64u;
    descriptor.hidden = 1u;
    descriptor.present_mode = 4u;
    BTBHGPUWindowReport report = {0};
    assert(btbh_gpu_window_create(&descriptor, &report) == NULL);
    assert(report.result == BTBH_GPU_WINDOW_FAILURE);
    assert(report.error_stage == BTBH_GPU_ERROR_STAGE_WINDOW_CREATION);
    assert(
        btbh_gpu_window_set_present_mode(
            NULL,
            BTBH_GPU_PRESENT_MODE_VSYNC,
            &report
        ) == BTBH_GPU_INVALID_ARGUMENT
    );
}

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
    test_present_mode_choice();
    test_present_mode_arguments();
    return 0;
}
