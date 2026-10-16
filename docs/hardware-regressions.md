# Cartridge, PPU, and DMA regression checks

`snes_hardware_regression_tests` covers cartridge address decoding, beam-counter latching, palette access, OAM mirrors, overscan timing, and indirect HDMA termination. It uses synthetic ROMs and explicit value checks that remain active in Release builds.

## Cartridge addressing

DSP-1 data and status windows depend on the cartridge board:

| Layout | Banks | Data offsets | Status offsets |
| --- | --- | --- | --- |
| LoROM, up to 1 MiB | $20-$3F, $A0-$BF | $8000-$BFFF | $C000-$FFFF |
| LoROM, over 1 MiB | $60-$6F, $E0-$EF | $0000-$3FFF | $4000-$7FFF |
| HiROM | $00-$1F, $80-$9F | $6000-$6FFF | $7000-$7FFF |

Odd and even addresses in a data window both access data. Status reads expose the ready flag without consuming command data, and status writes are ignored. Tests send a multiplication command through each layout, including its upper-bank mirror.

ExHiROM banks $80-$BF and $C0-$FF select the first 4 MiB; $00-$3F and $40-$7D select the second half in their ROM windows. ROM sizes that are not powers of two mirror their populated address blocks. For example, a 3 MiB LoROM repeats its last MiB at logical offsets $300000-$3FFFFF. Tests cover 1 MiB and 3 MiB LoROM images and 7 MiB and 8 MiB ExHiROM images. Existing core tests cover SRAM and WRAM precedence.

## PPU registers and timing

The horizontal timing counter uses master clocks. Beam-counter latching converts that value to dots, accounting for the two longer dots on normal scanlines and uniform four-clock dots on the short NTSC line. Both the $2137 software latch and the falling edge of WRIO bit 7 use this conversion.

WRIO writes also update the PPU's latch-enable state. Tests check that clearing bit 7 blocks subsequent software latches, that setting it enables them again, and that $213F reports and clears the latch flag correctly.

CGRAM reads and writes share one low/high byte phase and a palette address. Writing $2121 resets the phase while retaining the low write-data latch. Tests interleave reads and writes, check address wrapping, and verify the open-bus bit on high-byte reads. [PPU memory ports](ppu-memory-ports.md) describes rendering ownership and prefetch behavior.

OAM addresses $200-$3FF repeat the 32-byte high table at $200-$21F. Tests read and write every mirror, then cross the address wrap into the low table and check its paired-write behavior.

SETINI overscan writes update the VBlank boundary used by Timing. Normal mode has 224 visible scanlines and enters VBlank at line 225. Overscan has 239 visible scanlines and enters VBlank at line 240. Tests verify $4212, rendering and HDMA callback counts, VBlank and NMI callback positions, the bottom rendered row, and switching back to normal mode.

## Indirect HDMA termination

When the last active indirect HDMA channel reads a zero terminator, it reads one trailing address byte into the high half of DAS and clears the low half. A trailing byte of $56 therefore leaves DAS at $5600. If a later channel is still active, the terminating channel reads both address bytes normally. Tests cover termination during setup and after a transfer, checking DAS, the table pointer, completion state, and cycle counts.

## Run the checks

From the repository directory:

```powershell
cmake --build --preset build-release
ctest --test-dir out/build/release --output-on-failure
```

To run only this suite, add `-R snes_hardware_regression_tests` to the CTest command. These checks verify the behaviors above; they do not establish compatibility with every game or enhancement chip.

## Console reset

`Emulator::Reset()` resets a loaded console between emulation steps and returns false when no cartridge is loaded. It cancels an unfinished CPU instruction or DMA transfer before resetting device registers and clocks. Existing video, audio and input attachments stay connected.

Warm reset retains WRAM, audio RAM, VRAM and cartridge RAM. OAM, palette memory and SA-1 internal RAM clear. DSP registers retain their values except FLG, which becomes $E0. The PPU starts in forced blank with both data latches at $FF and VRAM incrementing after the high-byte port. Region settings, mounted media and battery clocks survive. MSU-1 playback stops and its data stream rewinds.

OBC1 uses the cartridge's 8 KiB save memory for its object data and selector bytes. Loading a save or resetting the console restores the object and base selection from that memory. Indexed object writes leave the physical register bytes untouched. Both object-controller aliases in banks $70-$71 and $F0-$F1 share these registers; unused board windows return open bus.

`snes_console_reset_tests` covers both regions, retained memory, DSP registers, cancelled instructions and DMA, cartridge processors, clocks, controller configuration and repeated reset. The MSU-1 integration suite checks reset while media is mounted and playing.
