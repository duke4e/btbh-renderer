# BTBHRenderer

The SDL3 GPU renderer for Be The Bullet Hell. The game consumes it as a SwiftPM
package (`BTBHRenderer`, `BTBHRendererGPU`).

- `Sources/` - Swift render contracts and the SDL_GPU backend.
- `src/gpu/`, `include/btbh_gpu/` - the native SDL_GPU runtime, compiled by SwiftPM
  through `Sources/BTBHRendererGPUCore`.
- `shaders/gpu/` - HLSL production shaders, compiled to SPIR-V, MSL and metallib by
  SDL_shadercross.
- `assets/` - lookup tables staged next to the compiled shaders.
- `external/` - pinned SDL, SDL_shadercross and Assimp. The raylib submodule only
  provides the single-header cgltf, stb_image and stb_truetype loaders and some test
  fixtures.

Build and stage the native dependencies and shaders through the game's
`tools/build-gpu-game.ps1` (Windows) or `tools/build-gpu-game.sh` (macOS, Linux). Native tests
build with `-DBTBH_BUILD_GPU_SPIKE=ON -DBTBH_BUILD_GPU_NATIVE_TESTS=ON -DR3D_ASSIMP_VENDORED=ON`
(not every test source is committed) and run with `ctest`;
Swift tests run through `.diagnostics/run-renderer-swift-test.ps1`.

The repository started as a fork of [r3d](https://github.com/Bigfoot71/r3d) (zlib, see
`LICENSE`). The raylib/r3d OpenGL renderer has been removed.
