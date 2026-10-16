# Clocked cartridge signal processors

DSP-1, DSP-1B, DSP-2, DSP-3, DSP-4, ST010, and ST011 can execute their cartridge firmware. The processor runs alongside the console CPU. Firmware instructions control data requests, status, arithmetic, and work RAM.

## Supplying firmware

Put firmware in a `firmware` folder beside the game, or select a directory explicitly:

```powershell
.\out\build\release\frontend\snes_frontend.exe --firmware-dir="C:\games\firmware" "C:\games\game.sfc"
```

| File | Bytes | Processor clock |
| --- | ---: | ---: |
| `dsp1.rom` | 8,192 | 7.6 MHz |
| `dsp1b.rom` | 8,192 | 7.6 MHz |
| `dsp2.rom` | 8,192 | 7.6 MHz |
| `dsp3.rom` | 8,192 | 7.6 MHz |
| `dsp4.rom` | 8,192 | 7.6 MHz |
| `st010.rom` | 53,248 | 11 MHz |
| `st011.rom` | 53,248 | 15 MHz |

Each file contains little-endian 24-bit program words followed by little-endian 16-bit data words. The smaller files contain 6,144 program bytes and 2,048 data bytes. ST010 and ST011 contain 49,152 program bytes and 4,096 data bytes. Firmware files are separate assets and are not included.

A directory may contain any subset of these files. A present file with an incorrect size or a read error stops loading and preserves the running game. An explicit directory takes precedence over adjacent files. Automatically discovered files apply only to games in that directory.

The loader selects DSP-1 for Pilotwings and DSP-1B for other DSP-1 cartridges. Cartridge header fields select the other families. A combined ROM image may append its processor firmware after the game bytes. The loader separates that trailer before calculating the cartridge CRC. Embedded firmware takes precedence over external files.

Without the selected firmware, the existing command models remain available. Their behavior and limits are described in [cartridge devices](cartridge-devices.md), [original hardware support](original-hardware.md), and [ST010 support](st010.md). Supplying firmware enables instruction execution and timed host handshakes instead of immediate command completion.

## Board addressing

The firmware path uses the physical board layout:

| Board | Data and status ports | Battery or work RAM |
| --- | --- | --- |
| LoROM without SRAM | `$30-$3F/$B0-$BF:$8000-$FFFF` | Internal processor RAM |
| LoROM with SRAM and at most 1 MiB ROM | `$20-$3F/$A0-$BF:$8000-$FFFF` | `$70-$7D/$F0-$FF:$0000-$FFFF` |
| LoROM with SRAM and larger ROM | `$60-$6F/$E0-$EF:$0000-$7FFF` | `$70-$7D/$F0-$FF:$0000-$7FFF` |
| HiROM | `$00-$1F/$80-$9F:$6000-$7FFF` | `$20-$3F/$A0-$BF:$6000-$7FFF` |
| ST010/ST011 | `$60-$67/$E0-$E7:$0000-$3FFF` | `$68-$6F/$E8-$EF:$0000-$7FFF`, mirrored every 4 KiB |

LoROM address bit 14 selects status. HiROM uses address bit 12. ST010/ST011 use even addresses for data and odd addresses for status. Status writes have no effect. Console WRAM retains banks `$7E-$7F`.

ST010/ST011 saves contain the processor's physical 4 KiB data RAM in little-endian byte order. There is no separate battery copy for CPU access. A whole-console reset clears processor registers and restarts the fractional clock while retaining this RAM. Loading another cartridge starts new RAM before restoring its save.

Core callers can populate `CartridgeFirmware` and set `Emulator::Config::cartridgeFirmware`. Span-based ROM loading uses that catalog or embedded firmware; file loading also supports adjacent discovery.

## Validation and limits

Generated tests cover instruction ordering, flags, branches, stack wrapping, pointer masks, multiplication, byte and word host transfers, firmware sizes, board mirrors, PAL and NTSC clock ratios, reset, and save persistence. Loader tests cover embedded trailers, directory isolation, and preservation of a running machine after failed loads.

A local instruction audit checked two million generated instruction cases across both processor revisions, including complete register, flag, stack, and RAM results. A 5,400-field Pilotwings flight capture matched every video field and 2,878,783 shared stereo samples. DSP-3 and DSP-4 game checks each matched 6,000 video fields and their shared audio samples. A 9,000-field Super Mario Kart attract sequence also matched video and shared audio samples.

A 1,800-field PAL DSP-4 capture matched video and 1,153,100 shared stereo samples. A 6,000-field ST011 overscan run exercised its firmware and shared RAM. ST010 racing also ran for 6,000 fields; [prefetched background pixels at mode switches](https://github.com/cupidthecat/cupid-snes/issues/32) still need work.

These checks cover the exercised sequences. They do not establish complete game compatibility, serial-pin behavior with external devices, or every raster transition. The committed tests generate their own firmware and ROM fixtures.
