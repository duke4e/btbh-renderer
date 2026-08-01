@echo off
setlocal EnableExtensions DisableDelayedExpansion
title Be The Bullet Hell Launcher

set "SCRIPT_DIR=%~dp0"
if defined BTBH_GAME_DIR (
    set "GAME_DIR=%BTBH_GAME_DIR%"
) else (
    set "GAME_DIR=%SCRIPT_DIR%..\Be-The-Bullet-Hell\City Generator (r3d)"
)
for %%I in ("%GAME_DIR%") do set "GAME_DIR=%%~fI"
set "GAME_EXE=%GAME_DIR%\.build\release\Main.exe"
set "GAME_RUNTIME_DIR=%GAME_DIR%\lib"

if /I "%~1"=="--help" goto :help
if /I "%~1"=="-h" goto :help
if not exist "%GAME_EXE%" goto :missing_exe
if not exist "%GAME_RUNTIME_DIR%\r3d.dll" goto :missing_runtime
set "PATH=%GAME_RUNTIME_DIR%;%PATH%"

set "BTBH_RENDER_PERF_LOG="
set "BTBH_RENDER_DIAGNOSTICS=0"
set "BTBH_GAMEPLAY_PHASE_LOG="
set "BTBH_PROFILE_SEED="
set "BTBH_START_PROFILE_FRAME="
set "BTBH_START_PROFILE_FRAMES="
set "BTBH_START_PROFILE_SECONDS="
set "BTBH_START_PROFILE_UNCAPPED="
set "BTBH_PROJECTILE_BENCHMARK_SHADOWS="
set "BTBH_PROJECTILE_BENCHMARK_MODE="
set "BTBH_COMBAT_RUNTIME="
set "BACKEND="
set "MODE="
set "MODE_ARGS="
set "PROFILE=normal"
set "SHADOWS=enabled"
set "BENCHMARK_MODE=legacy_external"
set "FRAME_RATE=display-refresh"
set "COMBAT_RUNTIME=v2"

if "%~1"=="" goto :interactive

call :set_backend "%~1"
if errorlevel 1 exit /b 2
call :set_mode "%~2"
if errorlevel 1 exit /b 2
if not "%~3"=="" set "PROFILE=%~3"
if not "%~4"=="" set "SHADOWS=%~4"
if not "%~5"=="" set "BENCHMARK_MODE=%~5"
if not "%~6"=="" set "FRAME_RATE=%~6"
if not "%~7"=="" set "COMBAT_RUNTIME=%~7"
goto :validate_and_run

:interactive
cls
echo ============================================================
echo              BE THE BULLET HELL - LAUNCHER
echo ============================================================
echo.
echo Renderer:
echo   [1] Production GPU renderer - Vulkan on Windows
echo   [2] Legacy renderer - raylib/r3d/OpenGL
echo.
choice /C 12 /N /M "Odaberi renderer [1-2]: "
if errorlevel 2 (
    set "BACKEND=legacy"
) else (
    set "BACKEND=gpu"
)

echo.
echo Launch mode:
echo   [1] Game
echo   [2] Combat Lab
echo   [3] Particle Editor
echo   [4] City Block Editor
echo   [5] Unified City Generator
echo   [6] Projectile renderer benchmark
echo   [7] Projectile simulation benchmark
echo   [8] GPU bootstrap smoke
echo.
choice /C 12345678 /N /M "Odaberi mode [1-8]: "
if errorlevel 8 (
    set "MODE=bootstrap"
) else if errorlevel 7 (
    set "MODE=projectile-sim"
) else if errorlevel 6 (
    set "MODE=projectile-render"
) else if errorlevel 5 (
    set "MODE=city"
) else if errorlevel 4 (
    set "MODE=block"
) else if errorlevel 3 (
    set "MODE=particle"
) else if errorlevel 2 (
    set "MODE=combat"
) else (
    set "MODE=game"
)

if /I "%MODE%"=="bootstrap" goto :interactive_profile
if /I "%MODE%"=="projectile-sim" goto :interactive_profile

if /I "%MODE%"=="game" goto :interactive_simulation
if /I "%MODE%"=="combat" goto :interactive_shadows
if /I "%MODE%"=="projectile-render" goto :interactive_benchmark
goto :interactive_profile

:interactive_simulation
echo.
echo Combat runtime:
echo   [1] Runtime V2 ^(default^)
echo   [2] Legacy oracle ^(fallback^)
choice /C 12 /N /M "Odaberi combat runtime [1-2]: "
if errorlevel 2 (
    set "COMBAT_RUNTIME=legacy"
) else (
    set "COMBAT_RUNTIME=v2"
)
goto :interactive_shadows

:interactive_shadows
echo.
echo Projectile shadows:
echo   [1] Enabled
echo   [2] Disabled
choice /C 12 /N /M "Odaberi shadows [1-2]: "
if errorlevel 2 (
    set "SHADOWS=disabled"
) else (
    set "SHADOWS=enabled"
)
goto :interactive_profile

:interactive_benchmark
echo.
echo Projectile benchmark pipeline:
echo   [1] legacy_external
echo   [2] legacy_fusion_projection
echo   [3] compact_fusion
choice /C 123 /N /M "Odaberi pipeline [1-3]: "
if errorlevel 3 (
    set "BENCHMARK_MODE=compact_fusion"
) else if errorlevel 2 (
    set "BENCHMARK_MODE=legacy_fusion_projection"
) else (
    set "BENCHMARK_MODE=legacy_external"
)
echo.
echo Benchmark shadows:
echo   [1] Enabled
echo   [2] Disabled
choice /C 12 /N /M "Odaberi shadows [1-2]: "
if errorlevel 2 (
    set "SHADOWS=disabled"
) else (
    set "SHADOWS=enabled"
)

:interactive_profile
echo.
echo Logging:
echo   [1] Normal gameplay
echo   [2] Performance telemetry
echo   [3] Detailed renderer diagnostics
choice /C 123 /N /M "Odaberi logging [1-3]: "
if errorlevel 3 (
    set "PROFILE=diagnostics"
) else if errorlevel 2 (
    set "PROFILE=perf"
) else (
    set "PROFILE=normal"
)

if /I "%MODE%"=="bootstrap" goto :validate_and_run
if /I "%MODE%"=="projectile-sim" goto :validate_and_run
echo.
echo Frame pacing:
echo   [1] Display refresh cap ^(normal gameplay^)
echo   [2] Uncapped ^(renderer throughput comparison^)
choice /C 12 /N /M "Odaberi pacing [1-2]: "
if errorlevel 2 (
    set "FRAME_RATE=uncapped"
) else (
    set "FRAME_RATE=display-refresh"
)

:validate_and_run
call :set_backend "%BACKEND%"
if errorlevel 1 exit /b 2
call :set_mode "%MODE%"
if errorlevel 1 exit /b 2
call :set_combat_runtime "%COMBAT_RUNTIME%"
if errorlevel 1 exit /b 2

if /I "%PROFILE%"=="normal" goto :profile_ok
if /I "%PROFILE%"=="perf" (
    set "BTBH_RENDER_PERF_LOG=1"
    goto :profile_ok
)
if /I "%PROFILE%"=="diagnostics" (
    set "BTBH_RENDER_DIAGNOSTICS=1"
    goto :profile_ok
)
echo ERROR: Logging mora biti normal, perf ili diagnostics.
exit /b 2

:profile_ok
if /I "%SHADOWS%"=="enabled" goto :shadows_ok
if /I "%SHADOWS%"=="disabled" goto :shadows_ok
echo ERROR: Shadows mora biti enabled ili disabled.
exit /b 2

:shadows_ok
if /I "%BENCHMARK_MODE%"=="legacy_external" goto :benchmark_ok
if /I "%BENCHMARK_MODE%"=="legacy_fusion_projection" goto :benchmark_ok
if /I "%BENCHMARK_MODE%"=="compact_fusion" goto :benchmark_ok
echo ERROR: Benchmark pipeline mora biti legacy_external, legacy_fusion_projection ili compact_fusion.
exit /b 2

:benchmark_ok
if /I "%FRAME_RATE%"=="display-refresh" goto :frame_rate_ok
if /I "%FRAME_RATE%"=="uncapped" (
    set "BTBH_START_PROFILE_UNCAPPED=1"
    goto :frame_rate_ok
)
echo ERROR: Frame pacing mora biti display-refresh ili uncapped.
exit /b 2

:frame_rate_ok
if /I "%MODE%"=="bootstrap" if /I not "%BACKEND%"=="gpu" (
    echo ERROR: GPU bootstrap smoke podrzava samo gpu renderer.
    exit /b 2
)

if /I "%MODE%"=="game" set "MODE_ARGS=%MODE_ARGS% --projectile-shadows=%SHADOWS%"
if /I "%MODE%"=="game" set "MODE_ARGS=%MODE_ARGS% --combat-runtime=%COMBAT_RUNTIME%"
if /I "%MODE%"=="combat" set "MODE_ARGS=%MODE_ARGS% --combat-runtime=%COMBAT_RUNTIME%"
if /I "%MODE%"=="combat" set "MODE_ARGS=%MODE_ARGS% --projectile-shadows=%SHADOWS%"
if /I "%MODE%"=="projectile-render" (
    set "BTBH_PROJECTILE_BENCHMARK_SHADOWS=%SHADOWS%"
    set "BTBH_PROJECTILE_BENCHMARK_MODE=%BENCHMARK_MODE%"
)

echo.
echo ============================================================
echo Game folder : %GAME_DIR%
echo Executable  : %GAME_EXE%
echo Renderer    : %BTBH_RENDER_BACKEND%
echo Mode        : %MODE%
echo Logging     : %PROFILE%
echo Frame pacing: %FRAME_RATE%
if /I "%MODE%"=="game" echo Combat RT   : %COMBAT_RUNTIME%
if /I "%MODE%"=="combat" echo Combat RT   : %COMBAT_RUNTIME%
if /I "%MODE%"=="game" echo Visual       : FXAA, SSAO off ^(current fixed Game profile^)
if /I "%MODE%"=="combat" echo Visual       : FXAA, SSAO off ^(current fixed Game profile^)
if /I "%MODE%"=="game" echo Proj shadows : %SHADOWS%
if /I "%MODE%"=="combat" echo Proj shadows : %SHADOWS%
if /I "%MODE%"=="projectile-render" echo Bench pipe   : %BENCHMARK_MODE%
if /I "%MODE%"=="projectile-render" echo Bench shadow : %SHADOWS%
echo ============================================================
echo.

pushd "%GAME_DIR%"
"%GAME_EXE%" %MODE_ARGS%
set "GAME_EXIT=%ERRORLEVEL%"
popd

echo.
echo Main.exe zavrsio je s kodom %GAME_EXIT%.
if "%~1"=="" pause
exit /b %GAME_EXIT%

:set_backend
if /I "%~1"=="gpu" (
    set "BACKEND=gpu"
    set "BTBH_RENDER_BACKEND=gpu"
    exit /b 0
)
if /I "%~1"=="legacy" (
    set "BACKEND=legacy"
    set "BTBH_RENDER_BACKEND=legacy"
    exit /b 0
)
echo ERROR: Renderer mora biti gpu ili legacy.
exit /b 1

:set_mode
if /I "%~1"=="game" (
    set "MODE=game"
    set "MODE_ARGS=--game"
    exit /b 0
)
if /I "%~1"=="combat" (
    set "MODE=combat"
    set "MODE_ARGS=--combat-lab"
    exit /b 0
)
if /I "%~1"=="particle" (
    set "MODE=particle"
    set "MODE_ARGS=--particle-editor"
    exit /b 0
)
if /I "%~1"=="block" (
    set "MODE=block"
    set "MODE_ARGS=--city-block-editor"
    exit /b 0
)
if /I "%~1"=="city" (
    set "MODE=city"
    set "MODE_ARGS=--unified-city"
    exit /b 0
)
if /I "%~1"=="projectile-render" (
    set "MODE=projectile-render"
    set "MODE_ARGS=--projectile-render-benchmark"
    exit /b 0
)
if /I "%~1"=="projectile-sim" (
    set "MODE=projectile-sim"
    set "MODE_ARGS=--projectile-simulation-benchmark"
    exit /b 0
)
if /I "%~1"=="bootstrap" (
    set "MODE=bootstrap"
    set "MODE_ARGS=--gpu-bootstrap-smoke --game"
    exit /b 0
)
echo ERROR: Nepoznat mode "%~1".
exit /b 1

:set_combat_runtime
if /I "%~1"=="legacy" (
    set "COMBAT_RUNTIME=legacy"
    set "BTBH_COMBAT_RUNTIME=legacy"
    exit /b 0
)
if /I "%~1"=="v2" (
    set "COMBAT_RUNTIME=v2"
    set "BTBH_COMBAT_RUNTIME=v2"
    exit /b 0
)
echo ERROR: Combat runtime mora biti legacy ili v2.
exit /b 1

:missing_exe
echo ERROR: Release executable nije pronadjen:
echo   %GAME_EXE%
echo.
echo Iz game foldera ga izgradi s:
echo   powershell -ExecutionPolicy Bypass -File tools\build-gpu-game.ps1 -Configuration release -RunBootstrapSmoke
exit /b 1

:missing_runtime
echo ERROR: Game runtime DLL folder nije potpun:
echo   %GAME_RUNTIME_DIR%
echo.
echo Ponovno izgradi release bundle s:
echo   powershell -ExecutionPolicy Bypass -File tools\build-gpu-game.ps1 -Configuration release -RunBootstrapSmoke
exit /b 1

:help
echo Usage:
echo   %~nx0
echo   %~nx0 ^<gpu^|legacy^> ^<mode^> [normal^|perf^|diagnostics] [enabled^|disabled] [benchmark-pipeline] [display-refresh^|uncapped] [combat-runtime]
echo   Combat runtime defaults to v2; pass legacy for the rollback path.
echo.
echo Modes:
echo   game, combat, particle, block, city,
echo   projectile-render, projectile-sim, bootstrap
echo.
echo Benchmark pipelines:
echo   legacy_external, legacy_fusion_projection, compact_fusion
echo.
echo Frame pacing:
echo   display-refresh ^(default^), uncapped
echo.
echo Combat runtimes:
echo   v2 ^(default^), legacy ^(fallback/oracle^)
echo.
echo Examples:
echo   %~nx0 gpu game normal enabled legacy_external display-refresh v2
echo   %~nx0 legacy game perf disabled legacy_external uncapped legacy
echo   %~nx0 gpu projectile-render perf disabled compact_fusion
echo.
echo Optional:
echo   Set BTBH_GAME_DIR to override the auto-detected game folder.
exit /b 0
