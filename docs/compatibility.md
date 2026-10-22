# Cartridge compatibility and saves

## Contra prototype

The supplied Contra prototype has an intact reset vector and an incomplete header whose mapping byte is $FF. The loader infers the layout from the selected header's position when the mode byte is unrecognized. LoROM, HiROM, ExLoROM, and ExHiROM headers are considered, including images with a 512-byte copier header.

Before the loader fix, a 120-frame run produced no visible frames or audio. Afterward, a 1,800-frame run with Start pressed at frame 600 reached gameplay, produced 1,448 visible frames, and generated audio. This checks boot and early gameplay; it does not establish that every level works.

The frontend displays the selected mapping by name. Frame execution no longer reads memory to print hang diagnostics. Those reads could change bus state, and long block moves could trigger false hang reports.

## Additional cartridge layouts

| Layout | Address behavior | Detection |
| --- | --- | --- |
| ExLoROM | Banks $80-$FF select the first 4 MiB; lower banks select the remaining ROM. Each populated section mirrors independently. | Header at file offset $407FC0. |
| NoMAD-1 LoROM | SRAM occupies both halves of banks $70-$7D and $F0-$FF. | Explicit database mapping override. |
| 24 Mbit board | Banks $00-$1F select the first MiB, $20-$3F and $A0-$BF the second, and $80-$9F the third. | Explicit database mapping override. |
| Large SRAM board | Banks $70-$73 expose overlapping 64 KiB windows at 32 KiB SRAM offsets. | Explicit database mapping override. |

WRAM retains priority in banks $7E-$7F. Synthetic tests cover ordinary and extended ROM mirrors, independent SRAM banks, and upper-half SRAM access. ExLoROM fixtures cover 5, 7, and 8 MiB images. These are address-decoding tests, not game compatibility tests for those boards.

The [loader](cartridge-devices.md#region-timing-and-loader-behavior) preserves supplied bank order and removes a 512-byte copier prefix only before complete 32 KiB banks. Reset instructions and physical header positions determine candidate priority. A title alone does not select the special mapping profiles above.

## Battery saves

The frontend loads a `.srm` file beside the ROM when the cartridge has SRAM. Changed SRAM is saved every 300 frames and on normal exit. Unchanged data does not rewrite the file, and cartridges without SRAM create no save file.

Each save is written to a temporary file beside the destination, then renamed over the previous save. A save with the wrong size stops loading with an error and remains untouched. Tests cover loading, replacement, unchanged saves, size mismatches, and failed replacements.

Super Game Boy uses a `.sav` beside the canonical handheld game path. It does not load or overwrite a save beside the console BIOS. Missing or empty files leave the cartridge's initial battery contents intact. A shorter file supplies its available prefix; the remaining RAM retains its initial bytes. A file larger than the cartridge's battery image stops loading with an error. Cartridge clocks may append mapper-specific state to the RAM payload.

Handheld saves use the same 300-frame autosave interval, normal-exit flush and atomic replacement. Unchanged bytes leave the file timestamp intact. Cartridges without battery data create no save file and leave any existing file untouched. Tests cover both adapter models and host regions, writes across four RAM banks, warm reset and fresh reload. Separate normal frontend runs verify loading and saving when BIOS, game and boot firmware occupy different directories.

## Headless game checks

The smoke tool runs a supplied ROM without opening a window. It reports visible frames, non-silent audio samples, elapsed time, throughput and median/95th/99th-percentile/maximum frame times. It presses Start for 20 frames and can save the final frame as a BMP:

```powershell
.\out\build\release\tools\snes_rom_smoke.exe ".\game.sfc" 1800 ".\out\game.bmp" 600
```

The arguments are ROM path, frame count, optional output path, and optional Start frame (default 240). Exit code 0 means at least one frame contained nonblack pixels; code 1 means none did; code 2 means loading, argument parsing, or output failed. A successful result does not prove correct gameplay or sound. The tool does not load or save battery files.

Timing covers each `StepFrame` call, including the tool's video and audio collectors. ROM loading and BMP writing are excluded. This measures core throughput without a display or audio device; it does not measure presentation latency or tearing. Compare the same ROM, frame count and input timing under similar host load.

[Performance checks](performance.md) records the measured baseline, paired run order, frame times and matching output counts for the supplied games.

## Remaining work

[Cartridge devices and region timing](cartridge-devices.md) covers DSP-2, OBC1, S-RTC, Sufami Turbo loading and saves, PAL timing, and loader regression tests.

[Controllers and cartridge slots](controllers-and-slots.md) covers mouse and multitap input, BS slot board mappings and flash saves, and combined Sufami Turbo images.

[S-DD1, DMA timing, and video output](dma-video-and-sdd1.md) covers S-DD1 decompression and banking, expanded cartridge images, DMA/HDMA interaction, high resolution, and interlace.

[Light guns and raster memory](light-guns-and-raster.md) covers Super Scope, one or two Justifiers, MACS rifle, beam-counter input, and preserving graphics memory across scanlines.

[ST010 cartridge support](st010.md) covers its board map, math commands, shared RAM, and command-level accuracy limits.

[Original hardware support](original-hardware.md) covers the implemented SA-1 and Super FX processors, DSP-3/4 firmware and Cx4 processor execution, SPC7110 decompression and clock, BS-X controller and streams, and ST011/ST018 firmware execution. DSP-3, ST011, and ST018 require firmware. It records the remaining processor, timing, broadcast, and game-validation gaps. A recognized chip or passing synthetic test is not a claim that every game using that chip works.
