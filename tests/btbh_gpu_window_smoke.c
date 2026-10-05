/* The smoke checks run through assert(), so keep them in Release builds too. */
#undef NDEBUG

#include "btbh_gpu/btbh_gpu.h"

#include <SDL3/SDL_events.h>
#include <SDL3/SDL_timer.h>
#include <SDL3/SDL_video.h>

#include <assert.h>
#include <stdio.h>
#include <stdlib.h>

static const BTBHGPUFeatureFlags required_features =
    BTBH_GPU_REQUIRED_FEATURES;

int main(void) {
#if defined(_WIN32)
    const BTBHGPUBackendRequest backend = BTBH_GPU_BACKEND_VULKAN;
#elif defined(__APPLE__)
    const BTBHGPUBackendRequest backend = BTBH_GPU_BACKEND_METAL;
#else
#error "The Phase 2 window smoke supports Windows and macOS only"
#endif

    const BTBHGPUWindowDescriptor window_descriptor = {
        .title = "BTBH SDL_GPU window smoke",
        .width = 640,
        .height = 360,
        .hidden = 0,
        .high_pixel_density = 1,
        .fullscreen_toggle = 1
    };
    BTBHGPUWindowReport window_report = {0};
    BTBHGPUWindow *window = btbh_gpu_window_create(
        &window_descriptor,
        &window_report
    );
    if (window == NULL) {
        fprintf(stderr, "Window creation failed: %s\n", window_report.message);
        return 1;
    }

    const BTBHGPUDeviceDescriptor device_descriptor = {
        .backend = backend,
        .required_features = required_features,
        .debug_mode = 1
    };
    BTBHGPUDeviceReport device_report = {0};
    BTBHGPUDevice *device = btbh_gpu_device_create(
        &device_descriptor,
        &device_report
    );
    if (device == NULL) {
        fprintf(stderr, "Device creation failed: %s\n", device_report.message);
        btbh_gpu_window_destroy(window);
        return 1;
    }

    assert(
        btbh_gpu_window_claim(device, window, &window_report) ==
        BTBH_GPU_OK
    );
    assert(window_report.claimed != 0);
    assert(window_report.logical_width == 640);
    assert(window_report.logical_height == 360);
    assert(window_report.drawable_width > 0);
    assert(window_report.drawable_height > 0);
    assert(window_report.swapchain_format != BTBH_GPU_TEXTURE_FORMAT_INVALID);
    assert(window_report.composition == BTBH_GPU_SWAPCHAIN_SDR);
    assert(window_report.present_mode != BTBH_GPU_PRESENT_MODE_INVALID);
    btbh_gpu_window_set_frame_telemetry_enabled(window, 1u);
    BTBHGPUWindowReport present_report = {0};
    assert(
        btbh_gpu_window_present_clear(
            window,
            18u,
            52u,
            86u,
            255u,
            &present_report
        ) == BTBH_GPU_OK
    );
    assert(present_report.claimed != 0);
    assert(
        present_report.swapchain_format != BTBH_GPU_TEXTURE_FORMAT_INVALID
    );
    assert(present_report.composition == BTBH_GPU_SWAPCHAIN_SDR);
    assert(present_report.present_mode != BTBH_GPU_PRESENT_MODE_INVALID);

    const char *capture_delay = getenv("BTBH_GPU_CAPTURE_DELAY_MS");
    if (capture_delay != NULL) {
        SDL_Delay((uint32_t)strtoul(capture_delay, NULL, 10));
    }

    BTBHGPUWindowReport resize_report = {0};
    assert(
        btbh_gpu_window_resize(window, 800, 450, &resize_report) ==
        BTBH_GPU_OK
    );
    assert(resize_report.logical_width == 800);
    assert(resize_report.logical_height == 450);
    assert(resize_report.drawable_width > 0);
    assert(resize_report.drawable_height > 0);
    assert(resize_report.claimed != 0);
    assert(resize_report.swapchain_format == present_report.swapchain_format);
    assert(resize_report.present_mode == present_report.present_mode);
    assert(resize_report.composition == BTBH_GPU_SWAPCHAIN_SDR);

    BTBHGPUWindowReport minimize_report = {0};
    assert(
        btbh_gpu_window_minimize(window, &minimize_report) == BTBH_GPU_OK
    );
    assert(minimize_report.minimized != 0);
    assert(minimize_report.claimed != 0);
    assert(minimize_report.swapchain_format == present_report.swapchain_format);
    assert(minimize_report.present_mode == present_report.present_mode);
    assert(minimize_report.composition == BTBH_GPU_SWAPCHAIN_SDR);

    /* A minimized window may have no swapchain image; presenting still submits the frame
       and succeeds instead of failing the window. */
    BTBHGPUWindowReport minimized_present_report = {0};
    assert(
        btbh_gpu_window_present_clear(
            window,
            18u,
            52u,
            86u,
            255u,
            &minimized_present_report
        ) == BTBH_GPU_OK
    );

    BTBHGPUWindowReport restore_report = {0};
    assert(
        btbh_gpu_window_restore(window, &restore_report) == BTBH_GPU_OK
    );
    assert(restore_report.minimized == 0);
    assert(restore_report.claimed != 0);
    assert(restore_report.logical_width == 800);
    assert(restore_report.logical_height == 450);
    assert(restore_report.drawable_width > 0);
    assert(restore_report.drawable_height > 0);
    assert(restore_report.swapchain_format == present_report.swapchain_format);
    assert(restore_report.present_mode == present_report.present_mode);
    assert(restore_report.composition == BTBH_GPU_SWAPCHAIN_SDR);

    BTBHGPUWindowReport final_present_report = {0};
    assert(
        btbh_gpu_window_present_clear(
            window,
            86u,
            52u,
            18u,
            255u,
            &final_present_report
        ) == BTBH_GPU_OK
    );
    assert(final_present_report.claimed != 0);
    assert(
        final_present_report.swapchain_format ==
        present_report.swapchain_format
    );
    assert(final_present_report.present_mode == present_report.present_mode);
    assert(final_present_report.composition == BTBH_GPU_SWAPCHAIN_SDR);

    BTBHGPUFrameTelemetrySample telemetry[3] = {0};
    const uint32_t telemetry_count =
        btbh_gpu_window_finish_frame_telemetry(
            window,
            telemetry,
            3u
        );
    assert(telemetry_count == 3u);
    assert(telemetry[0].frame_serial == 1u);
    assert(telemetry[1].frame_serial == 2u);
    assert(telemetry[2].frame_serial == 3u);
    assert(telemetry[0].cpu_submit_milliseconds >= 0.0);
    assert(
        telemetry[0].fence_completion_milliseconds >=
        telemetry[0].cpu_submit_milliseconds
    );

    /* Alt+Enter enters borderless desktop fullscreen and leaves it again; the key press never
       reaches the project key state, so Enter bindings do not fire with it. */
    int window_count = 0;
    SDL_Window **windows = SDL_GetWindows(&window_count);
    assert(windows != NULL && window_count == 1);
    SDL_Window *sdl_window = windows[0];
    SDL_free(windows);
    for (int toggle = 0; toggle < 2; ++toggle) {
        SDL_Event alt_enter;
        SDL_zero(alt_enter);
        alt_enter.type = SDL_EVENT_KEY_DOWN;
        alt_enter.key.windowID = SDL_GetWindowID(sdl_window);
        alt_enter.key.scancode = SDL_SCANCODE_RETURN;
        alt_enter.key.key = SDLK_RETURN;
        alt_enter.key.mod = SDL_KMOD_LALT;
        alt_enter.key.down = true;
        assert(SDL_PushEvent(&alt_enter));
        BTBHGPUPlatformSnapshot snapshot = {0};
        assert(btbh_gpu_window_poll_events(window, &snapshot) == BTBH_GPU_OK);
        const bool fullscreen =
            (SDL_GetWindowFlags(sdl_window) & SDL_WINDOW_FULLSCREEN) != 0;
        assert(fullscreen == (toggle == 0));
        assert(btbh_gpu_window_key_pressed(window, 4u) == 0u);
    }

    btbh_gpu_window_destroy(window);
    btbh_gpu_device_shutdown(device);
    btbh_gpu_device_destroy(device);
    return 0;
}
