# Performance checks

## Scanline video, 2026-10-20

Headless Windows Release replays used normal core video and audio with cartridge video selection enabled. Each replay ran 1,200 warmup fields followed by 3,600 measured fields. Video hashing and PCM-file capture were disabled during timing. Active renderer observations confirmed scanline video for both adapter models and progressive video for Desert Fighter.

| Replay | Fields/s | Median field, ms | p95 field, ms | Stereo frames delivered |
| --- | ---: | ---: | ---: | ---: |
| Kirby's Dream Land, Super Game Boy | 90.97 | 10.900 | 12.103 | 1,919,238 |
| Kirby's Dream Land, Super Game Boy 2 | 91.43 | 10.859 | 11.799 | 1,919,239 |
| Desert Fighter, progressive video | 103.03 | 9.597 | 10.460 | 2,306,512 |

All three sequences exceeded full speed and delivered every generated native audio frame. Separate capture runs checked complete PCM, sample counts, pixels, processor state, and cartridge memory. These measurements cover core throughput; display presentation and physical audio-device pacing remain separate checks. Other workspace work was active, so field-time distributions include host contention.

An earlier interlaced-hires RPM Racing replay reached 78.14 fields/s and delivered 1,922,880 stereo frames over the same measured duration. That run preceded automatic cartridge video selection.

A later Marvelous replay with selectable SA-1 contention reached 61.85 fields/s in progressive hires video. It used 1,200 warmup fields and 3,600 measured fields with normal video and audio, while other heavy validation was stopped. Median field time was 16.099 ms and p95 was 17.090 ms. Every generated stereo audio frame was delivered, totaling 1,919,194 frames. This sequence exceeded average full speed, though its p95 field time crossed an individual display deadline. Separate 1,800-field captures for four SA-1 games checked complete pixels, PCM, counts, clocks, processor state and cartridge memory.

With deferred GSU execution enabled, Dirt Racer reached 87.80 fields/s and Yoshi's Island reached 122.33 fields/s using the same 1,200-field warmup and 3,600-field measurement. Median field times were 11.588 and 8.142 ms, with p95 times of 12.266 and 8.924 ms. Both used default scanline video and presented all measured fields. Dirt Racer's sequence was silent; Yoshi's Island produced audible output and delivered all 1,919,176 generated stereo frames. Separate 1,800-field captures covered five GSU games, with another Dirt Racer capture checking execution at CPU bus accesses. Other workspace activity remained present during timing.

With low entropy and seed 1, a later Kirby's Dream Land replay reached 97.03 fields/s on the first adapter and 96.28 fields/s on the second. Each used 1,200 warmup fields and 3,600 measured fields with normal scanline video and audible output. Median field times were 10.255 and 10.330 ms, with p95 times of 10.962 and 11.163 ms. Every measured field was presented, and all generated native stereo frames were delivered: 1,919,238 and 1,919,239 respectively. Separate 2,400-field captures checked five games on both models with low entropy, plus two games on both models with deterministic and high entropy. All 270 checkpoints retained complete memory, processor, video, PCM, count, clock and battery checks. Other workspace activity continued during timing.

## Release compiler optimization

Release and RelWithDebInfo builds enable link-time optimization when the compiler and linker support it. This lets the compiler optimize calls across the core's source files. CMake checks support during configuration and uses normal compilation if the check fails. Debug builds and the separately configured SDL dependency keep their existing settings.

Pass `-DSNES_ENABLE_LTO=OFF` when configuring a build to compare throughput or investigate a compiler-specific problem. Use separate build directories and keep the compiler, build type, game inputs, and audio settings the same for both runs.

The Windows CI job keys its SDL cache from `cmake/SDL3.cmake`. Changes to release compiler settings can reuse the dependency build. CMake still configures the pinned SDL version and its static-library options before configuring the core.

An alternating six-run check of Kirby Super Star used GCC 15 Release builds and 300 fields per run. Median throughput increased from 61.42 to 69.54 fields/s with link-time optimization, a 13.2% gain. Each run reached the same master-clock count, produced the same number of audible samples, and retained identical final save RAM. This short startup sequence measures local throughput; its output counts do not establish complete game compatibility.

## ST018 firmware replay, 2026-10-16

A quiet, headless Hayazashi Nidan Morita Shougi 2 replay completed 6,000 fields at 84.20 fields/s. The sequence navigated the menus and selected the computer to make the opening move. Median field time was 11.813 ms, p95 was 12.716 ms, p99 was 13.729 ms, and the maximum was 20.101 ms.

The Windows Release build used the supplied ST018 program and data firmware. Active processor mode and firmware bytes were checked before timing. Core audio generation remained enabled; video hashing and PCM capture were disabled, and other heavy validation was stopped. The run exceeded full speed for this sequence. It does not establish complete game compatibility.

An earlier 300-field quiet run reached 56.21 fields/s. Batching independent ARM work between CPU bridge accesses raised the same short replay to 87.67 fields/s. Every bridge access still synchronizes firmware execution before reading or changing shared state.

## Clocked graphics pipeline, 2026-10-16

A quiet, headless Kirby Super Star replay ran 3,600 fields at 61.96 fields/s after the palette-port changes. Median field time was 16.078 ms, p95 was 17.474 ms, p99 was 18.664 ms, and the maximum was 23.921 ms. The Windows Release build used GCC 15 on the same local workstation, with other heavy validation stopped during the run.

The replay exceeded the console's average field rate. Its slowest fields still exceeded a display deadline, and this result does not establish full speed for every game or measure presentation and audio-device latency. Video hashing and PCM-file capture were disabled for this timing run; separate capture checks validate output.

The September measurements below evaluated VRAM snapshot sharing and per-line raster event lists. The CPU bus fix and optional MSU-1 mixer were included in that comparison. Clocked background and object fetches have since replaced the full VRAM snapshots; raster event lists still retain screen-register changes for composition. Those older measurements do not describe the current core's throughput.

## Local measurements, 2026-09-15

The baseline was commit `8224ace`, with the same timing instrumentation added to `snes_rom_smoke`. Both binaries used Clang 21.1.5, CMake Release and the same Windows host with an Intel Core i7-13700H. Each game ran 1,200 frames with Start held for 20 frames beginning at frame 240. The run order was baseline, changed, changed, baseline. The workstation was in use during the measurements.

Throughput below is 2,400 frames divided by the combined duration of each pair of runs. These are headless measurements with the tool's video and audio collectors attached. They exclude ROM loading and image-file writing, and do not measure display presentation or audio-device latency. None of these runs used MSU-1 tracks.

| Game | Baseline frames/s | Changed frames/s | Throughput change |
| --- | ---: | ---: | ---: |
| Super Mario World | 137.88 | 160.34 | +16.3% |
| Kirby Super Star | 94.66 | 109.36 | +15.5% |
| Super Mario RPG | 97.83 | 113.91 | +16.4% |
| Super Mario All-Stars + Super Mario World | 140.96 | 165.27 | +17.2% |

The 95th-percentile frame-time ranges below show both individual runs; they are not percentiles pooled across the two runs.

| Game | Baseline p95, ms | Changed p95, ms |
| --- | ---: | ---: |
| Super Mario World | 8.633 to 8.669 | 7.385 to 7.735 |
| Kirby Super Star | 11.697 to 12.823 | 10.518 to 11.018 |
| Super Mario RPG | 12.044 to 12.536 | 10.401 to 10.437 |
| Super Mario All-Stars + Super Mario World | 8.079 to 10.020 | 7.147 to 8.585 |

Timing varied between runs. A preliminary unpaired Mario World run reached 49.001 ms for its slowest frame; the paired changed runs peaked at 9.433 and 11.562 ms. Mario RPG still reached 22.078 ms in one changed run. The reduced typical cost does not eliminate occasional spikes or establish the same performance on another machine.

## Output checks

Visible-frame counts, audible-sample counts and final CPU positions matched the baseline in every paired run. Separate 1,200-frame captures for these four games and the supplied Contra prototype had identical BMP hashes before and after the changes. The images were also inspected for their expected title and demo scenes.

The Contra prototype needs a later scripted Start to exercise gameplay in this check. At 1,800 frames with Start at frame 600, both binaries produced 1,441 visible frames and 353,459 audible channel samples, ended at `01:A61E`, and saved identical gameplay images. A shorter 1,200-frame run with Start at frame 240 produced no audible samples in either binary. That script difference is not evidence of an audio regression.

These checks cover the scripted boot and demo paths. Matching one final frame and output counts does not establish complete game compatibility or pixel-for-pixel equivalence throughout every run.

The smoke tool prints elapsed time, throughput, median, p95, p99 and maximum frame time. Its arguments are documented in [Cartridge compatibility and saves](compatibility.md).
