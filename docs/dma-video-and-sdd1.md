# S-DD1, DMA timing, and video output

S-DD1 cartridges load with a decompressor, bank registers, and board-specific SRAM mapping. CPU, DMA, and HDMA accesses advance connected hardware during their bus cycles. Video output retains the 512 dots of high-resolution modes and both interlaced fields.

## S-DD1 cartridge support

Registers $4800 and $4801 enable decompression per channel. The cartridge observes writes to each channel's source address and 16-bit count registers. A read in $C0-$FF that matches an enabled channel's stored source consumes one decoded byte. This applies to CPU reads as well as DMA and HDMA. The first matching channel has priority. A zero stored count wraps through 65,536 reads; the final read clears only that channel's bit in $4801.

Registers $4804-$4807 select four independent 1 MiB ROM windows with their low four bits. Only bits 7 and 0-3 retain written values. Bit 7 of $4805 mirrors the lower program MiB into banks $20-$3F; bit 7 of $4807 does the same for $A0-$BF. The decoder reads through the current window selections, including at input boundaries. Register aliases occupy $4800-$480F in banks $00-$3F and $80-$BF. Unused ports read the corresponding physical ROM byte at $4800-$480F and ignore writes. Reset selects banks 0, 1, 2, and 3 and clears both enable masks and the stored DMA addresses and counts.

| Address | Mapping |
| --- | --- |
| $00-$3F/$80-$BF:$8000-$FFFF | Fixed LoROM pages |
| $00-$3F/$80-$BF:$6000-$7FFF | SRAM, when populated |
| $70-$73:$0000-$FFFF | SRAM, with the upper half mirroring the lower half |
| $C0-$FF:$0000-$FFFF | Four switchable ROM windows |

WRAM retains banks $7E-$7F. Unpopulated SRAM returns open bus. Battery RAM uses the existing `.srm` load and save path.

The decompressor handles all four bitplane layouts and all four context models described in the [S-DD1 algorithm documentation](https://wiki.superfamicom.org/s-dd1). Channels share one active decoder. Reprogramming a channel or changing an enable mask retains its progress. Completing the stored count invalidates the stream so the next matching read initializes a new one. Planar output retains the second byte of a decoded pair until its own read. An interrupted DMA leaves unread bytes and the remaining cartridge count intact; the CPU's DMA count remains separate.

The chip header selects this board independently of ROM size. Images of 8, 12, or 16 MiB retain S-DD1 registers, bank switching, decompression, and populated SRAM. ROM size alone does not select a pre-decompressed layout.

Selections beyond the supplied ROM capacity use bounded address mirroring. In a 6 MiB image, bank 6 mirrors bank 4, while banks 7 and 15 mirror bank 5. A console guest checks all four switchable windows through both register aliases, both regions and renderers, and cold/warm startup. These checks specify the retained mapping policy; physical behavior for unpopulated bank selections remains unmeasured.

## DMA and field timing

Writing $420B queues DMA. The controller allows one CPU bus cycle before taking the bus, aligns to its eight-clock divider, and charges global and per-channel startup intervals. Each byte has two four-clock read phases followed by the destination write. HDMA table reads and data transfers use the same clock path. WRAM refresh pauses transfers, and HDMA can interrupt general DMA between bytes. HDMA on the same channel cancels that general DMA transfer; another channel's HDMA lets it resume. Source offsets wrap within their original bank.

The scheduler returns DMA control at a CPU cycle boundary and samples interrupts through the CPU's normal instruction boundary logic. HDMA initializes on scanline zero at a divider-dependent position. DRAM refresh follows that divider, alternating positions 538 and 534 on ordinary scanlines and preserving its phase across a short line. Odd-sized clock batches retain their unfinished half tick. SETINI controls field timing, and `StepFrame` targets the next field boundary.

CPU register accesses see hardware events reached during earlier accesses of the same instruction. Memory fetches and cartridge processors have their own clock phases and validation limits. An instruction or DMA operation crossing a field boundary can make `StepFrame` return after that boundary.

## Video output

Modes 5 and 6 and pseudo-hires produce 512 output dots. Even dots use the sub screen and odd dots use the main screen, with color math and brightness applied during composition. If a frame mixes low and high resolution lines, each low resolution dot is repeated horizontally. Forced blanking clears the full output width.

Interlace writes alternating row parities and retains the preceding field. Normal and overscan output heights are 224 and 239 rows, or 448 and 478 in interlace. The SDL frontend uploads the complete frame and keeps the same display proportions when its pixel dimensions change.

## Validation and remaining gaps

Committed tests retain decoder digests for every header mode and check bank boundaries, register masks and fallthrough, SRAM aliases, save round trips, all eight DMA channels and modes, enable masks, reverse DMA, count wrapping, partial transfers, reset, and large ROM images. Generated board checks cover 54 ROM/RAM configurations. Stateful stream checks cover all 16 layouts, shared channels, mid-stream register changes, and resets, including 14,162,672 decoded or ordinary reads and 780,432 controller snapshots.

Timing tests check divider alignment, separate source/destination timestamps, startup and CPU resumption, refresh stalls, HDMA interruption on the same and different channels, APU progress, bank wrapping, and NTSC/PAL interlaced field lengths. CPU bus tests place HBlank and NMI edges inside instructions. PPU tests check alternating main/sub dots, mixed resolutions, blanking, field retention, and returning to normal dimensions.

The full test command and the current cartridge validation scope are listed in [original hardware support](original-hardware.md). Tests keep the transfer data-path checks separate from integration checks that advance the complete machine.

Three 6,000-field replays cover the supplied Star Ocean image and US and PAL Street Fighter Alpha 2 images. All tested video fields, shared audio samples, and saved bytes agree. Six ordinary-game controls retain their previous video, audio, and saves. These boot, title, and demo sequences do not establish complete gameplay coverage or measured physical chip timing. No game images are included in the tests.

[Original hardware support](original-hardware.md) describes the cartridge processors, command engines, broadcast hardware, and their remaining gaps. [Light guns and raster memory](light-guns-and-raster.md) covers gun protocols and graphics-memory timing. CPU multiply/divide results advance through internal arithmetic cycles. Backgrounds and objects use clocked fetch state; [PPU memory ports](ppu-memory-ports.md) covers CPU access ownership and byte phases. Memory collisions within a clock still need separate validation. S-DD1 decoding does not model its internal input FIFO timing or hardware underflow behavior.
