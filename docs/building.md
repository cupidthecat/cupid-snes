# Building and testing

Build from the repository directory with Ninja and a C++20 compiler. The provided presets require CMake 3.25 or newer; manual configuration requires 3.21 or newer. Windows and Linux share the SDL3 frontend and Dear ImGui desktop UI.

CMake uses an installed SDL3 when available. Otherwise it fetches SDL3 release 3.2.8 and builds a static library. Dear ImGui 1.92.5 comes from a release archive with a checked SHA-256 digest. The first configure needs network access for missing dependencies.

## Presets

The repository provides Debug and Release presets. A Release build and test run uses:

```powershell
cmake --preset release
cmake --build --preset build-release
ctest --test-dir out/build/release --output-on-failure
```

Use the `debug` configure preset, `build-debug` build preset and `out/build/debug` test directory for Debug. When configuring a Windows build with Clang AddressSanitizer, keep the matching `clang_rt.asan_dynamic-x86_64.dll` on `PATH` or beside the executable. LLVM installs this runtime; the Release output does not include it.

`SNES_ENABLE_TESTS` defaults to `ON`. `SNES_ENABLE_LTO` enables Release and RelWithDebInfo link-time optimization when the compiler and linker support it. Set it to `OFF` in a separate build directory when measuring ordinary compilation or using incompatible instrumentation.

## Checks and tools

CTest uses generated ROMs, small processor programs, command streams, known output vectors and deterministic clocks. Committed checks do not need commercial games or firmware. They cover CPU and interrupt behavior, bus scheduling, rendering, DMA, audio, controllers, cartridge devices, persistent storage, checkpoints and desktop workflows.

`SNES_BUNDLE_OPTIMIZED_TESTS=ON` places selected checks in a shared executable to reduce repeated link-time optimization. Build the `snes_optimized_checks` target and run CTest with `-L optimized`. The regular configuration keeps separate executables.

Windows CI uses MSBuild's multi-tool scheduler with a shared limit of four compiler processes across projects. This lets large libraries compile multiple source files at once while keeping project-level parallel builds within the same limit.

Native Linux application checks use a private X11 display through `xvfb-run` and `tests/run_x11_application.py`. The ordinary application checks use SDL's dummy drivers. Native checks add real window creation, focus and cursor positioning to the application's SDL event tests.

The build also provides two command-line tools:

- `snes_rom_smoke` runs a ROM without a window, counts visible frames and audible samples, reports frame-time percentiles, presses Start for a scripted interval and can save the last frame as a BMP. [Compatibility and saves](compatibility.md) documents arguments and exit codes.
- `snes_trace_dump` demonstrates CPU bus, PPU register and DMA trace sinks.

[Performance checks](performance.md) records paired measurements and the limits of the game smoke tests. [Source layout](source-layout.md) maps implementation folders to hardware and desktop services.
