# Performance checks

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
| Super Mario World | 8.633–8.669 | 7.385–7.735 |
| Kirby Super Star | 11.697–12.823 | 10.518–11.018 |
| Super Mario RPG | 12.044–12.536 | 10.401–10.437 |
| Super Mario All-Stars + Super Mario World | 8.079–10.020 | 7.147–8.585 |

Timing varied between runs. A preliminary unpaired Mario World run reached 49.001 ms for its slowest frame; the paired changed runs peaked at 9.433 and 11.562 ms. Mario RPG still reached 22.078 ms in one changed run. The reduced typical cost does not eliminate occasional spikes or establish the same performance on another machine.

## Output checks

Visible-frame counts, audible-sample counts and final CPU positions matched the baseline in every paired run. Separate 1,200-frame captures for these four games and the supplied Contra prototype had identical BMP hashes before and after the changes. The images were also inspected for their expected title and demo scenes.

The Contra prototype needs a later scripted Start to exercise gameplay in this check. At 1,800 frames with Start at frame 600, both binaries produced 1,441 visible frames and 353,459 audible channel samples, ended at `01:A61E`, and saved identical gameplay images. A shorter 1,200-frame run with Start at frame 240 produced no audible samples in either binary. That script difference is not evidence of an audio regression.

These checks cover the scripted boot and demo paths. Matching one final frame and output counts does not establish complete game compatibility or pixel-for-pixel equivalence throughout every run.

The smoke tool prints elapsed time, throughput, median, p95, p99 and maximum frame time. Its arguments are documented in [Cartridge compatibility and saves](compatibility.md).
