# Cx4 processor execution

With a supplied data ROM, Cx4 cartridges execute their HG51B program from the game ROM. Without it, the existing command adapter remains available. The adapter completes commands immediately and does not run the cartridge program.

## Firmware and clocks

`cx4.data.rom` contains 1,024 little-endian 24-bit words, for 3,072 bytes total. Put it in the adjacent `firmware` directory or select a directory with `--firmware-dir`. Core callers can populate `CartridgeFirmware` with `FirmwareKind::Cx4`. A data ROM appended to the game takes precedence over the catalog and is removed before calculating the cartridge CRC. Firmware assets are not included.

The processor runs at 20 MHz against the console's NTSC or PAL master clock. Execution can suspend within instructions, cache fills, delayed bus accesses, DMA, and timed suspension. CPU accesses observe state at the current clock rather than waiting for a whole operation to finish.

When the worker is suspended in a halted idle cycle with no pending activity, clock intervals advance arithmetically to the same final processor-clock edge. Active operations still execute their individual clocks. A stop request does not discard an instruction already fetched before that request.

## Processor storage and instructions

The engine models a 24-bit accumulator, sixteen general registers, a 48-bit product, data buffers and pointers, a 15-bit program bank and page, an 8-bit program counter, flags, and an eight-entry return stack. Its two instruction-cache pages each hold 256 words. A cache miss loads each byte with the selected ROM or RAM wait state; cache locks and page transitions can halt execution.

Instructions include arithmetic and flags, comparisons, logical operations, shifts and rotation, multiplication, register transfers, data-ROM and data-RAM access, conditional branches and calls, returns, skips, bus waits, clear, and halt. The data ROM is read-only. The processor has 3 KiB of work RAM; instruction accesses mirror its upper address quarter into physical RAM.

Reset clears registers and restores control defaults. It retains work RAM, data ROM, instruction-cache words, and return-stack storage. Loading another cartridge creates new storage.

## CPU map and controls

The standard board uses these CPU windows:

| Window | Behavior |
| --- | --- |
| `$00-$3F/$80-$BF:$8000-$FFFF` | Cartridge ROM |
| `$70-$77:$0000-$7FFF` | Header-sized battery RAM; reads return zero when RAM is absent |
| `$00-$3F/$80-$BF:$6000-$6BFF` and `$7000-$7BFF` | The same 3 KiB work RAM |
| `$00-$3F/$80-$BF:$6C00-$6FFF` and `$7C00-$7FFF` | I/O, repeated every 1 KiB |

Unused I/O locations return zero. Work RAM does not extend into those locations. General registers have aliases at `$7F80-$7FAF` and `$7FC0-$7FEF`.

The CPU and DSP have separate address wiring. CPU ROM addresses remove address bit 15; save accesses mirror the full physical address into the allocated RAM. The DSP also sees ROM at `$C0-$FF:$0000-$FFFF` and reduces ROM and save addresses according to its mapping mode. Those extra ROM banks remain open bus for the CPU. The alternate DSP mapping mode does not change the physical CPU windows.

`$7F40-$7F47` configures DMA. The length is captured when a transfer starts; source and target addresses remain live. A transfer can overwrite its own controls without shortening the captured length. Transfers between two ROM regions or two save-RAM regions lock the processor.

`$7F48-$7F4F` controls cache loading, page locks, program base, bank, and start address. `$7F50` selects RAM and ROM wait states. `$7F53` stops execution and clears a lock. `$7F55-$7F5C` requests indefinite or timed suspension, and `$7F5D` resumes execution.

Status aliases report suspension at bit 0, the interrupt flag at bit 1, running at bit 6, and busy at bit 7. Busy covers cache loading, DMA, and pending bus work. While busy, CPU ROM reads preserve open bus, with the header's final 64 bytes exposing I/O and vector overrides. Processor ROM accesses remain available.

Halting raises the CPU IRQ line when enabled. Writing `$7F5E` acknowledges the internal flag without deasserting that line. Setting bit 0 of `$7F51` disables processor IRQ and clears the CPU line. Fast-ROM flags preserve Cx4 detection, and extended processor subtypes keep Cx4, ARM, and signal-processor cartridges distinct.

## Validation and limits

Generated tests cover cache-fill clocks, delayed bus completion, register widths and aliases, reset storage, DMA length capture, firmware precedence, fast-ROM detection, ROM ownership, vector overrides, IRQ acknowledgment, physical mapping, and saves. A generated console program waits in WRAM while the processor owns ROM, then uses its reply as a palette color.

A local audit matched 2,097,152 instruction cases across every opcode and 32 flag/operand seeds. It compared complete registers, control state, cache words, data ROM, work RAM, return stack, external memory, and ordered bus/clock events. A board audit matched 1,024 timelines of 512 CPU accesses each, including the physical CPU map, both DSP mapping modes, NTSC/PAL clocks, IRQ lines, and final cache, work RAM, and saves. Save allocations included absent, 8 KiB, 64 KiB, and 96 KiB RAM.

Six local Mega Man X2/X3 replays checked 48,000 video fields and 25,589,404 shared stereo samples without mismatches. They covered startup, story and title screens, menu selection, and player-controlled opening stages. Processor mode and all data-ROM bytes were checked before the captures. These replay assets are separate from the generated test suite.

These checks cover the exercised behavior. Complete game compatibility and measured physical chip timings remain unverified.
