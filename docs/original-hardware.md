# Original SNES hardware support

The core executes SA-1 and Super FX programs, supplied DSP-3/4 firmware, and Cx4 cartridge programs with a supplied data ROM. It also exposes SPC7110 decompression and clock hardware. BS-X uses its own memory controller and packet receivers. These devices connect to the console bus, cartridge storage, and interrupt sampler; detection alone is not considered an implementation.

Existing LoROM, HiROM, extended and special boards, DSP-1/2, OBC1, S-RTC, S-DD1, ST010, Sufami Turbo, controllers, and PPU rendering paths remain in use. This work concerns original hardware compatibility. It does not add frontend features or change the renderer into a per-dot model.

## Console startup and video hardware

`Emulator::Config::entropyMode` selects deterministic startup (`None`), a correlated memory pattern with sparse bit changes (`Low`), or independent byte generation (`High`). The default is `Low`. `entropySeed` and `entropySequence` make startup reproducible; an omitted console seed uses the process CPU clock. WRAM, progressive video memory and registers, and ST018 work RAM consume one shared stream in startup order.

The progressive video path retains WRAM, APURAM and VRAM on warm reset. It clears OAM and initializes palette memory and register latches through a fresh startup stream. Deterministic mode clears palette memory and retains defined fallback bits, including the PPU bus values and VRAM increment selection. Reset preserves the selected video hardware configuration.

Handheld startup uses an independent noise stream owned by each `Emulator` instance. Both `Low` and `High` enable the same model-specific WRAM, high RAM, object-memory and wave-memory patterns. `None` clears those regions except for the second adapter's `$55` WRAM pattern. VRAM stays clear, and the final wave byte stays zero. Console and bridge resets, cartridge replacement and adapter model changes continue the stream; Camera capture-control writes draw from it too. With an explicit seed, `entropySequence` supplies the upper 32 bits and `entropySeed` the lower 32 bits. An omitted handheld seed uses wall-clock time when the console instance is created. Battery data retains its own lifecycle.

ST018 initializes its 16 KiB work RAM after video startup on cold and warm console resets. Deterministic mode clears it. Reset through the cartridge bridge retains work RAM and firmware writes. Battery RAM remains separate and survives both resets.

`Emulator::Config::gameCompatibilityFixes` enables cartridge startup and audio rules by default. The complete header title `DIRT RACER` fills WRAM with `$FF` after normal startup initialization, including on warm reset. `MAGICAL DROP` writes `$FF` to every DSP register after power or reset; the ENDX register still clears on a write. `RENDERING RANGER R2` selects a 32,000 Hz DSP input clock. Output remains at 32,040 stereo frames per second through cubic conversion. The Hurricanes and Nichibutsu Arcade Classics select deterministic startup. Disabling the setting uses the requested entropy and the ordinary 32,040 Hz input clock. Loading another cartridge restores its own settings.

`Emulator::Config::ppu` selects PPU2 revision 1, 2 or 3 and a VRAM capacity of 65,536 or 131,072 bytes. Revision 3 and 64 KiB are the defaults. The progressive model's status register reports the selected revision. VRAM register addresses retain the upper address bit; the memory interface applies the capacity mask. At 64 KiB the two banks alias, while the progressive model's 128 KiB setting exposes both banks to CPU ports, backgrounds and objects. The default [scanline model](scanline-video.md) reports revision 3, uses 64 KiB, and retains video, palette and sprite memory on warm reset.

`Emulator::Config::cpuRevision` selects CPU revision 1 or 2, with revision 2 as the default. Values outside that range select the nearest supported revision. Revision 1 refresh starts at horizontal clock 530 on every scanline. Revision 2 follows the DMA divider, starting at clock 538 or 534. HDMA setup also follows the selected revision: revision 1 uses clock 20 minus the divider phase, while revision 2 uses clock 12 plus that phase. The `$4210` version nibble and warm reset retain the selected CPU hardware.

For example, a deterministic 128 KiB console with PPU2 revision 2 uses:

```cpp
snes::core::Emulator::Config config;
config.entropyMode = snes::core::EntropyMode::None;
config.entropySeed = 0;
config.ppu = {2, 131072};
snes::core::Emulator emulator(config);
```

These settings cover the progressive renderer. The default scanline model has separate cold and warm graphics-memory rules; see [scanline rendering](scanline-video.md).

## Super Game Boy

Both adapter models run an SM83 processor with DMG video and sound, interrupts, timers, OAM DMA, boot-ROM mapping and cartridge storage. The console BIOS handles borders and adapter commands through the cartridge registers, packet queue and four tile buffers. Controller selection supports one, two or four handheld players.

The register windows occupy `$6000-$67FF` and `$7000-$7FFF` in system banks `$00-$3F` and `$80-$BF`. `$6000` reports the video row and write buffer; `$6001` selects the read buffer. `$6002` dequeues a command packet into `$7000-$700F`, `$6003` controls execution and clock division, and `$6004-$6007` hold controller input. `$600F` reports revision `$21`; `$7800` streams the selected 512-byte tile buffer.

Console bus intervals accrue handheld clock credit under the active divider. Scanline handoffs resume instruction execution. The first model derives its oscillator from the console master clock; the second uses 20,971,520 Hz before division. Divider writes, bridge reset and audio register accesses preserve their ordering. Handheld, console and optional expansion audio mix through retained stream histories.

Supply a console BIOS, handheld game and matching 256-byte boot firmware:

```powershell
.\snes_frontend.exe --super-game-boy ".\adapter.sfc" ".\game.gb" ".\boot.bin"
```

These files may occupy separate directories. `--region=pal` or `--region=ntsc` selects the console hardware before power; `--region=auto` uses the BIOS header. Game and firmware assets are not bundled.

Cartridge support includes ROM-only, MBC1/2/3/5/7, MMM01, HuC1/HuC3, Camera and TPP1 mapping. Cartridge components model clocks, motion, infrared, camera capture and rumble state. The SDL frontend does not provide physical camera, motion, infrared or rumble integration for handheld cartridges. Battery RAM survives console and bridge resets; volatile cartridge RAM follows its mapper's reset rules. See [battery saves](compatibility.md#battery-saves) for file paths and short-file handling.

Generated tests cover processor instructions, memory and clock boundaries, host packets, tile transfer, video, sound, cartridge controllers and battery lifetimes. Supplied BIOS and game sequences cover both models, both console renderers and NTSC/PAL host selection. Fresh device runs measured about 60.12 presentations/s in NTSC and 50.02-50.05 in PAL, with no refills during the 900-presentation measurement windows. Near-silent startup refills were counted separately.

These checks cover DMG execution and the recorded sequences. They do not establish complete game compatibility, color handheld hardware, physical peripheral integration or equality of every internal state and bus access. The save-file checks include CPU-written data in all four MBC1 RAM banks, warm reset, fresh reload and normal frontend exit.

## Cartridge implementations

| Device | Implemented behavior | Important limits |
| --- | --- | --- |
| SA-1 | Reuses the 65816 instruction core; reset/vector registers, mailbox IRQ/NMI, timers, 2 KiB IRAM and protection, shared BW-RAM and bitmap modes, ROM banking, arithmetic, variable-length bit reads, progressive DMA, both character-conversion modes, and master-clock advancement. | Instruction execution suspends at bus and idle clocks, with selectable contention waits for shared memory. ROM cache timing, exact character-conversion latency, DMA priority arbitration, and short/interlaced scanline coupling need further validation. |
| Super FX/FX2 | GSU instruction and ALT encodings, prefetched pipeline and delay slots, register file, 512-byte cache, plotting/pixel rows, ROM/RAM buffers, screen modes, bus ownership, clock modes, IRQ, declared RAM capacities, and physical CPU ROM/RAM windows. | Cache fills and pixel writes publish at transaction completion rather than each constituent byte. Commercial GSU game and hardware traces remain unverified. |
| DSP-3 | Clocked uPD7725 execution with supplied firmware, byte/word host protocol, board addressing, and internal data RAM. | Firmware is required. Generated processor checks and recorded game sequences cover the tested behavior; complete game compatibility remains unverified. |
| DSP-4 | Clocked uPD7725 execution with supplied firmware, byte/word host protocol, board addressing, and internal data RAM. | Without firmware, a command model handles multiplication, road projection, lighting, polygon windows, objects, and OAM. Fallback reciprocal rounding and edge inputs need hardware measurements. |
| Cx4 | HG51B cartridge-program execution at 20 MHz with supplied data ROM, 24-bit registers and instructions, cache pages, delayed bus transfers, DMA, work RAM, ROM ownership, CPU IRQ and vector controls, and board addressing. | Without data firmware, the immediate command adapter remains. Generated instruction and board checks cover the tested behavior; complete game compatibility and physical chip timings remain unverified. See [Cx4 processor execution](cx4.md). |
| SPC7110/RTC | Clocked decompression and arithmetic, three compressed graphics modes, prefetched data ports, physical ROM/RAM maps, queued operations, and the serial RTC with oscillator timing and calendar controls. | Generated timelines cover processor, coding, memory, and RTC state. Complete game compatibility and measured physical-chip timings remain unverified. See [SPC7110 and cartridge RTC](spc7110.md). |
| S-DD1 | Physical ROM/RAM windows, masked bank registers, program mirrors, DMA source/count observation, and a shared byte stream consumed through CPU, DMA, and HDMA bus reads. Interrupted transfers retain decoder state. | Generated board and stream checks and three game sequences cover the exercised behavior. Internal input FIFO timing, underflow behavior, complete gameplay, and physical chip measurements remain unverified. See [S-DD1 and DMA timing](dma-video-and-sdd1.md). |
| S-RTC | Numeric calendar programming, live serial reads, a one-Hz oscillator driven by console clocks, reset retention, erased battery RAM, slow/fast ExHiROM board detection, and persistent clock saves. | Generated calendar, clock, port, and elapsed-time checks cover the exercised behavior. Complete game compatibility and physical RTC measurements remain unverified. See [S-RTC cartridge clock](srtc.md). |
| BS-X | Identifier-based BIOS and slot selection, BIOS overlay, latched controller bits, 512 KiB PSRAM, declared battery SRAM, shared slot flash commands, protection and persistent metadata, clocked block erase, radio registers, serial time, and optional recorded receivers. | Live reception, SoundLink audio, and measured physical chip timings remain unverified. See [broadcast controllers and memory packs](broadcast-memory.md). |
| ST011 | Clocked uPD96050 execution with supplied firmware, host ports, and shared 4 KiB processor RAM with battery saves. | Firmware is required. Generated processor checks and a recorded game sequence cover the tested behavior; complete game compatibility remains unverified. |
| ST018 | ARM firmware execution at 21.44 MHz, banked registers and status, instruction prefetch, program/data ROM, work RAM, timed CPU mailboxes, reset delay, signal/timer registers, and header-sized battery RAM. | Firmware is required. Generated processor and bridge checks and two 6,000-field game replays cover the tested sequences; complete game compatibility and physical chip timings remain unverified. See [ST018 firmware execution](st018.md). |

DSP-1 through DSP-4 use the physical layouts in [clocked cartridge processors](clocked-cartridge-processors.md) for both firmware execution and command models. LoROM windows depend on SRAM presence and ROM capacity; HiROM has separate wiring. Device mirrors share one command stream, while unused ROM banks remain undriven. [DSP-2 command state](cartridge-devices.md#dsp-2-command-model) covers retained output, length phases, and reset behavior.

SA-1 slot boards with valid extended `Z??J` IDs accept a separate mask ROM or flash pack through the broadcast loader. MMC pages 4-7 select a populated pack; an empty slot retains base-ROM mirroring. The CPU BW-RAM window remains `$40-$4F`. Mask ROM stays read-only; flash retains its array and metadata for save loading, but SA-1 ROM writes do not issue flash commands.

The SA-1 bitmap window selects a physical 2 KiB BW-RAM page before unpacking two-bit or four-bit pixels. Division uses a signed quotient truncated toward zero and a nonnegative remainder, including the measured divide-by-zero register values. The bit reader discards address bit zero, reads only ROM, mirrors non-ROM 32 KiB blocks to bank-zero LoROM, and follows Super MMC projection. Writing `$225B` loads the address and clears `$2258`, so automatic advance must be enabled afterward. Regression vectors follow [SA-1 hardware measurements](https://sneslab.net/wiki/SA-1_Hardware_Behavior). Those division measurements cover `$2306-$2309`; the existing upper-byte behavior at `$230A` remains unverified.

Super FX CPU RAM windows occupy $00-$3F/$80-$BF:$6000-$7FFF, $70-$71, and $F0-$F1. Other CPU banks remain unconnected even when the RAM declaration is larger. ROM occupies the upper halves of $00-$3F/$80-$BF and the complete $40-$5F/$C0-$DF windows. Connected address lines follow the supplied ROM capacity; a size hint does not add apertures or rearrange ROM regions. The ordinary RAM declaration takes priority over the expansion declaration, and both mask reserved bits and stop at 256 KiB.

GSU registers occupy `$3000-$34FF` in banks `$00-$3F` and `$80-$BF`. The unused `$3300-$33FF` ports return zero and ignore writes; `$3400-$34FF` mirrors the first register page. Console execution defers graphics-processor clock debt until a register access or scanline handoff by default. Set `Emulator::Config::superFxDelayedSync` to `false` to execute at CPU bus accesses instead. Both modes retain normal register access timing and reset behavior. An unmasked STOP asserts the cartridge IRQ output, and an acknowledge read clears it. Writing the SFR flag alone neither raises nor acknowledges an interrupt.

The GSU RAMB instruction selects bank `$70` or `$71`, and `$303C` returns zero in its reserved upper bits, as specified in [Nintendo's development manual, Book II, section 4.7](https://archive.org/stream/SNESDevManual/book2_djvu.txt). Tests exercise all 256 low-byte values supplied to RAMB and verify that banked loads/stores stay within the connected RAM banks. Invalid internal GSU banks are not treated as extra ROM aliases.

SPC7110 calendar state uses a separate 24-byte `.rtc` representation: sixteen nibble-valued clock registers followed by an eight-byte little-endian timestamp. This differs from S-RTC's format; the loaded cartridge selects the decoder. Tests cover emulated-time advancement, leap days, hold and stopped clocks, elapsed-time loading, and rejection of invalid state.

Console startup disables SA-1 shared-bus contention by default. `Emulator::Config::sa1BusContention` enables conflict waits for ROM, BW-RAM and IRAM. Cold power, warm reset and cartridge reload apply the selection. Direct `Sa1` users keep contention enabled unless they call `SetBusContention(false)`.

Without contention, console bus intervals accumulate SA-1 clock credit. The processor runs at scanline handoffs and before CPU or DMA accesses to its registers, IRAM or BW-RAM. Ordinary ROM reads retain the deferred work. Contention mode runs the processor after each console bus interval. Standalone `Sa1::Advance` calls synchronize by default; callers can defer execution explicitly without losing elapsed time. Generated console programs check interrupt raster counters and shared-memory DMA reads across both regions, timing modes and cold or warm starts.

## CPU, DMA, and audio timing

CPU reads, writes, and internal cycles advance hardware individually. Registers can observe an event reached during an earlier access in the same instruction. IRQ entry uses the interrupt-mask value sampled before the final cycle, including CLI, SEI, REP, SEP, and PLP. Masked IRQs still release WAI.

Multiply and divide run for eight and sixteen CPU cycles. The result registers expose intermediate values, reads sample before the next arithmetic step, and trigger writes cannot restart an active operation, including its final busy cycle. Ordinary DMA pauses arithmetic; refresh contributes five steps. PLB uses a full-width stack address increment: in emulation mode, a stack pointer of `$01FF` reads `$0200` before returning to page one.

DMA requests allow a CPU cycle before bus takeover. Transfers align to the DMA divider, charge startup, place source and destination accesses on separate phases, and resume the CPU at its cycle boundary. HDMA table and data accesses advance hardware as they occur and can interrupt general DMA. Refresh positions follow the divider instead of staying fixed on every scanline. The existing eight-channel modes, source-bank wrapping, and same-channel HDMA cancellation remain covered.

The S-DSP advances through 32 phases per output sample. Register latches, BRR/directory reads, envelopes, noise, pitch modulation, voice mixing, echo RAM, and output timing have separate phase behavior. The normal complete-cycle rate is 1.02528 MHz; cartridge compatibility settings can select 1.024 MHz.

`Emulator::Config::dspMode` defaults to `Smp::DspMode::Batch32`. At SMP memory-access boundaries, the DSP runs complete 32-clock batches and retains its clock lead for later accesses. Idle time accumulates until the next memory access. This scheduling affects register readbacks, shared echo RAM and the number of samples emitted at a field boundary. `PerCycle` advances on odd SMP half-cycles, including stretched waits. Both modes execute the same DSP pipeline. Standalone `Smp` callers default to `PerCycle` and can select a mode before power or reset.

The complete titles `KOUSHIEN_2` and `RENDERING RANGER R2`, plus `BUBSY II` in PAL regions, select per-cycle execution at launch. These rules apply independently of optional startup fixes. Warm reset retains the selected profile, while another cartridge restores its own selection.

Audio RAM is initialized when the console instance is created. Reset and cartridge replacement retain its complete 64 KiB while resetting the sound processor's ports, timers and execution state. Cartridge replacement also resets DSP registers and video memory according to the selected execution model. Standalone `Smp::Power(false)` remains available to clear audio RAM explicitly.

The SPC700 can pause inside an instruction or a stretched memory access. `RunUntil` stops at its requested clock count. Communication reads hold their sampled input until the read completes; pending stores do not reach the console early. Port writes reach backing audio RAM before the console handoff, then update the port latch when execution resumes. CONTROL clears and timer read-and-clear operations occur on their access boundary. `Step` still completes one instruction for direct processor users. Reset cancels pending work, and coroutine storage belongs to the processor so sequential thread handoff does not invalidate a suspended instruction.

The SMP TEST register selects internal and external wait states separately. I/O, idle cycles, and the enabled IPL overlay use the internal setting; other RAM accesses use the external setting. The implemented clock model uses 1, 2, 5, or 10 SMP clocks and advances the timer divider by 2, 4, 8, or 16 ticks. A TEST write finishes under the old setting. The model samples port reads midway through a stretched access, rounded to the next SMP clock, and ordinary RAM reads at its end.

The frame collector drains full DSP blocks during long DMA operations. Transfers spanning several fields retain all generated audio instead of stopping at the ordinary 2,048-sample buffer limit. Video presentation still returns the latest completed field after such an operation.

A refresh penalty is drained after the current clock interval. SA-1 cache/contention and DMA arbitration, finer coprocessor transactions, and analog audio behavior remain accuracy work. [Hardware measurements of SMP TEST](https://forums.nesdev.org/viewtopic.php?start=75&t=16140) show much longer internal waits and lockups on some revisions. Those differences, sub-clock port sampling, and read/write collisions are not modeled.

## Raster rendering

The console defaults to [scanline rendering](scanline-video.md), which retains screen registers and palette memory once per line. The descriptions below cover the optional progressive model.

Progressive backgrounds and objects use clocked fetch state. Name, bitplane, offset, OAM, and geometry changes reach the stages that consume them; fetched words remain retained across later writes. Layer samples resolve palette colors, windows, screen enables, and live priorities at the pixel clock. Brightness and color math also resolve at each dot, retaining the preceding main dot's decisions for the next sub dot. The complete 480-row raster retains border pixels and interlaced fields; presentation extracts its visible region. See [background fetch timing](background-fetch.md) and [object fetch timing](object-fetch.md). [PPU memory ports](ppu-memory-ports.md) covers active palette addresses, shared byte phases, and CPU access restrictions. Memory collisions within a clock still need separate validation.

Progressive lower-border output updates a separate lookup for 8,192 dim-color entries. Each lookup precedes its stores, including during forced blank, so brightness zero can retain earlier border colors. Disabling overscan clears the first 1,024 entries; a reset that ends overscan makes the same clear. Other entries survive cold and warm reset and cartridge replacement. Display pixels clear on reset. The default scanline model keeps fixed brightness tables.

Mode 7 BG1 supports direct color with and without mosaic. EXTBG BG2 continues to use CGRAM, and BG1's mosaic enable controls vertical sampling for both Mode 7 layers. Hires and pseudo-hires output pair each sub pixel with the preceding main pixel's color-math and window decisions, including at a raster-state boundary. An empty sub screen displays its palette backdrop, while main color math uses the fixed-color register without halving.

Modes 5 and 6 measure horizontal mosaic blocks in logical dots. Mosaic selects the even source sample for both screens, including size one. Mode 6 shares the preceding offset lookup across a pair of character columns; the initial pair has no offset entry. Horizontal offsets use source-pixel coordinates and retain the latched fine-scroll phase. Regression fixtures cover flips, windows, tile heights and independent horizontal/vertical offset enables.

Raster events are ordered by pixel position within each scanline, preserving write order at a shared position. Their storage is reused across frames, so rendering a line does not allocate a temporary event list or scan events belonging to every other line.

Sprite overflow is evaluated during the current frame, even when OBJ is absent from the main and sub screens or the display is blanked. The 32-object and 34-tile flags remain sticky until the next frame, and `$213E` preserves PPU1 open-bus bit 4. Deferred drawing no longer changes the global overflow flags.

## BS-X host interface

The ordinary loader recognizes the valid extended `ZBSJ` identifier and leaves the memory-pack slot empty. `LoadBroadcastCartridge` accepts a separate 512 KiB mask ROM or a 1, 2, or 4 MiB pack. Battery RAM follows the header declaration. BIOS and broadcast files are not included.

```cpp
std::string error;
if (!emulator.LoadBroadcastCartridge(biosBytes, packBytes, &error)) {
    throw std::runtime_error(error);
}
if (!emulator.LoadBroadcastStream(0x0123, 0, recordedPayload)) {
    throw std::runtime_error("Invalid broadcast channel");
}
emulator.SetBroadcastTimeSource([] {
    return snes::core::BroadcastTime{2026, 9, 14, 2, 12, 34, 56};
});
```

The default radio interface supplies an eighteen-byte serial time record at `$2192`. Installing a payload opts into recorded reception: channel zero supplies a calendar record, and other installed channels expose 22-byte packets, including padding on a partial final packet. Receivers keep independent cursors and prefix flags. Standalone callers restore the default interface with `Bsx::ClearStreams`. The cartridge clock uses host local time, while the standalone device and tests can inject a deterministic clock.

The memory controller exposes D7-only registers, pending and applied mapping bits, BIOS mirrors, PSRAM snippets, and expansion holes. A commit requires D7 high. Flash protection, status modes, page buffers, erase timing, and `.flash.meta` persistence are described in [broadcast controllers and memory packs](broadcast-memory.md).

## Validation scope

The current Windows Release build passes all 78 bundled CTest suites. The earlier Clang 21 Release and AddressSanitizer audit covered 36 targets: 33 bundled tests and three optional DMA/IRQ, CPU, and SPC diagnostics. The Windows legacy test executable reserves an 8 MiB stack so sanitizer instrumentation can retain its large fixtures.

Run the complete test set with:

```powershell
cmake --build --preset build-release
ctest --test-dir out/build/release --output-on-failure
```

Committed tests use generated programs, command streams, known output vectors, and deterministic clocks. Native 65816 programs exercise cartridge ports through the real bus. Processor tests run SA-1 and GSU programs, check their IRQs, and verify that save loading retains shared storage. Bus tests place events within instructions and verify DMA timestamps. Audio tests change registers and shared RAM across individual DSP phases.

The CPU arithmetic tests check all 65,536 eight-bit products and 657,664 division cases, plus partial results, busy writes, reset, refresh, and DMA behavior. Final-cycle trigger tests cover six-, eight-, and twelve-master-clock accesses and a DMA transfer that holds the pending write across refresh. SMP tests cover all sixteen internal/external wait combinations, timer and DSP clocks, held port reads, delayed stores, IPL mapping, reset during a wait, and halted execution. PAL and NTSC console tests also verify stretched port-store deadlines.

The sound-port integration tests execute console loads and stores through `$2140-$2143` and their mirrors under both PAL and NTSC clock ratios. They check deadlines in both communication directions. In the earlier audit, these tests exposed an SMP store becoming visible too early and console accesses failing to reach Super FX RAM banks `$72-$73`.

The earlier audit compared SPC700 traces for 2,048 cases: all 256 opcodes, four deterministic RAM/register/flag seeds, and two branch paths. Ordered read/write/idle events, cycle counts, registers, halt state, and complete RAM hashes matched. The committed segmented-execution tests also cover reset during an instruction, CONTROL clears, timer reads, halt deadlines, DSP phase boundaries, and sequential cross-thread resume/destruction.

DSP scheduling tests upload an audible program through the IPL ports and check both execution modes through cold and warm starts. Field traces include full work RAM, audio RAM, both processor states, DSP registers, clocks, output counts and PCM digests. Separate fixtures cover port-read midpoints, retained clock lead, backing RAM before a port handoff, and exact-title launch rules with optional startup fixes enabled and disabled.

For external text diagnostics, `snes_rom_diagnostic` reads a 32-column ASCII tilemap directly from backing VRAM without touching PPU read latches. It prints stable result pages. `--expect` requires exact result lines and returns a failing exit status when any are missing; execution alone is not a diagnostic pass. For example, with an existing [SnesTests checkout](https://github.com/SourMesen/SnesTests):

```powershell
cmake --preset release -DSNES_DIAGNOSTIC_ROM_DIR=C:/path/to/SnesTests
cmake --build --preset build-release
ctest --test-dir out/build/release -R snes_external_dma_irq --output-on-failure
```

The DMA/IRQ fixture checks all 19 source-defined results from checkout `71c50e3272cb43d9042b1630e4dbbe7ac2f0ca29`. Its two SEI sentinels are `$00FF`, as written by the ROM's 8-bit test routine. The opcode timing ROM produced 839 measurements across 54 result pages. Comparisons exposed differences in stack-relative indexed and PER timing, so dedicated tests retain the seven/eight-cycle indexed accesses and six-cycle PER specified by the [W65C816S datasheet, table 5-4](https://www.westerndesigncenter.com/wdc/documentation/w65c816s.pdf). Absolute counter/startup-phase differences remain unclassified; the timing ROMs are not claimed as fully passing.

The independent [CPU/SPC diagnostic release v1.4](https://github.com/gilyon/snes-tests/releases/tag/v1.4) adds 1,610 65C816 cases and 1,368 SPC700 cases. Both ROMs reach their final `Success` line. The full CPU ROM exposed the PLB stack-boundary error, which now has a separate regression. These ROMs test instruction results, flags, and addressing behavior; they do not establish cycle timing, hardware interrupts, or DSP accuracy. Their SLEEP/STOP and WAI/STP exclusions have separate core tests.

With that release extracted locally, enable both ROMs without downloading anything during the build:

```powershell
cmake --preset release -DSNES_PROCESSOR_DIAGNOSTIC_DIR=C:/path/to/snes-tests
cmake --build --preset build-release
ctest --test-dir out/build/release -R 'snes_external_(cpu|spc)' --output-on-failure
```

The earlier cartridge and DSP audit exercised these paths:

| Device | Comparison scope and result |
| --- | --- |
| SA-1 | Current arithmetic checks pass 177,096 byte comparisons across multiplication, division, 40-bit accumulator wrap, partial operand writes, and overflow flag retention across mode changes. Separate tests cover bitmap access, variable-length bit reads, memory mapping, DMA, character conversion, and processor execution. Fine timing and contention still need broader validation. |
| Super FX | 8,660 programs; register, flag, bank, and RAM results matched at the time of that audit. The current RAMBR reserved-bit behavior follows the hardware specification above. Plotting, cache storage, and timing also have independent tests. |
| DSP-3 | 935,231 byte/status/result comparisons matched, including coefficient ROM, geometry, planar data, 1,500 generated decode streams, and 250 terrain/path streams. |
| DSP-4 | 1,620,513 comparisons matched, including fixed commands, OAM budgets, 500 road/lighting streams, 300 polygon streams, and 300 object streams. |
| Cx4 | 8,956 command runs matched complete work-RAM snapshots across arithmetic and graphics paths. |
| SPC7110 | 1,536 decoder timelines of 512 rows match full coding state. Another 1,024 CPU timelines of 512 accesses match registers, clocks, tile storage, and final saves across standard and expansion layouts. RTC checks cover 262,144 calendar/control cases and 1,024 port timelines of 256 accesses. |
| S-DSP | 1,228,800 phases in 32 scenarios matched registers, decoder state, shared RAM, and 38,400 stereo output frames. |
| BS-X | 256 mapping configurations, 1,068 flash transactions, 371,783 reads, and 36,222 writes matched on common paths, including two packet receivers. The independently documented MCC differences, injected calendar, and malformed/exhausted stream handling were checked separately. |
| ST011/ST018 | 11,527,200 RAM/status/result comparisons matched on the implemented board-packet and startup-handshake paths. No internal processor or game-logic behavior was compared. |

Committed tests generate their own cartridge programs and use fixed expected results. Coverage is limited to the recorded fixtures.

Each supplied game completed a 9,000-frame integration run from reset, with Start held for 20 frames beginning at frame 600:

| Game | Visible frames | Non-silent sample values | Captured state |
| --- | ---: | ---: | --- |
| Super Mario World | 8,833 | 8,052,131 | File selection menu |
| Contra IV - The Alien Wars prototype | 8,141 | 7,012,696 | Attract-mode action |
| Kirby Super Star | 8,010 | 7,922,048 | SA-1 attract-mode action |
| Super Mario RPG | 7,945 | 7,279,463 | SA-1 opening sequence |
| Super Mario All-Stars + Super Mario World | 8,877 | 8,477,772 | Game selection menu |

These runs check boot, sustained execution, output generation, and limited Start input. They do not establish complete playthroughs, every controller interaction, or commercial compatibility for every enhancement chip. Generated tests cover additional processor, bus and peripheral cases. Diagnostic BMP capture now converts the core's ABGR pixels to BMP channel order, so capture colors can be compared correctly.
