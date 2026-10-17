# Cartridge devices and region timing

The loader distinguishes cartridge processors using the mapping mode, cartridge device family, extended subtype, and identifying title. Both slow and fast ROM flags retain the same device family. A dedicated SA-1, S-DD1, or SPC7110 mapping mode also selects that controller when the type byte omits it. An unsupported device produces an error with its name. A failed load leaves the current cartridge, RAM, and CPU bus intact. A successful load resets the machine and its frame counters.

DSP firmware identity follows the title: `PILOTWINGS` selects DSP-1, `DUNGEON MASTER` selects DSP-2, `SDガンダムGX` selects DSP-3, and `TOP GEAR 3000` or `PLANETS CHAMP TG3000` selects DSP-4. Other titles in that device family select DSP-1B. Extended subtype $01 selects ST010, except `2DAN MORITA SHOUGI`, which selects ST011. Subtypes $02 and $10 select ST018 and Cx4. These subtype rules apply independently of the maker byte.

Header titles skip zero and $FF padding, trim outer spaces, and decode halfwidth Japanese kana and voiced marks to UTF-8. The same decoded title controls firmware selection and displayed metadata. Unknown title bytes appear as `?`. `YUYU NO QUIZ DE GO!GO` retains LoROM layout despite its title overwriting the mode byte.

[Cartridge memory initialization](cartridge-memory.md) describes erased external RAM, processor work RAM, save loading, and reset retention.

## DSP-2 command model

With cartridge firmware, DSP-1 through DSP-4 and ST010/ST011 use [clocked signal processors](clocked-cartridge-processors.md). The following DSP-2 description applies to the fallback used without firmware.

DSP-1 through DSP-4 use the [same board wiring](clocked-cartridge-processors.md#board-addressing) with or without firmware. ROM size, SRAM presence, and LoROM/HiROM layout select the physical windows. DSP-2 data aliases share one command stream; its command-model status port reads zero and ignores writes. Unused banks return open bus.

The implementation handles packed-pixel conversion to SNES bitplanes, transparent color selection, bitmap overlays, pixel reversal, unsigned 16-bit multiplication, and fixed-point bitmap scaling. Reads consume queued result bytes and return $FF after exhaustion.

Completing a command rewinds the output cursor. Transparency changes, no-op commands, and unknown commands retain the existing output count and bytes. Starting a new command leaves unread output available while its input is pending. Unknown commands retain the preceding input count instead of forcing immediate recovery.

Overlay, reversal, and scaling keep separate length phases. Zero-length headers retain their command's pending phase, which affects a later request for that command. Input cursors wrap within the 512-byte parameter buffer; an active data phase with a zero expected count can require a complete buffer wrap. Console reset clears protocol state while retaining parameter and output RAM. Scaling can observe parameter bytes left by an earlier command or reset.

DSP-1 maintains separate byte phases for status and data accesses. DSP-3 and DSP-4 have separate command engines, described in [original hardware support](original-hardware.md). The [timed competition boards](competition-cartridges.md) combine DSP-1 with selectable ROM chips and a countdown controller.

## OBC1

OBC1 has 8 KiB of RAM at $00-$3F/$80-$BF:$6000-$7FFF. Registers $7FF0-$7FF3 read and write the selected object's four bytes. Register $7FF4 reads its packed attribute byte and writes only the selected object's two bits. Registers $7FF5 and $7FF6 select the object table and object index. Selector writes update their physical RAM bytes. Indexed object and attribute writes update only the selected storage. Banks $70-$71/$F0-$F1 also expose mirrored OBC1 windows.

New cartridge RAM starts at $FF. Loading a save or resetting the console rebuilds selectors from retained RAM bytes $1FF5/$1FF6. [Super Scope input](light-guns-and-raster.md) is available separately. A Metal Combat boot capture exercised the object controller with a standard gamepad; Scope gameplay remains untested.

## S-RTC

The clock reads through $2800 and accepts commands through $2801 in banks $00-$3F/$80-$BF. Commands select read mode, program the calendar, or clear it. Reads contain a leading $F, thirteen calendar nibbles, and a trailing $F. Calendar reads remain live across clock edges, and saving preserves the serial cursor.

A new clock has cleared calendar storage. Games can program its date, and running calendars advance at one Hz from console clocks. Loading a save applies elapsed host time. [S-RTC cartridge clock](srtc.md) describes numeric programming, live reads, calendar carries, and reset behavior.

The frontend stores clock data in a `.rtc` file beside the ROM, using the same temporary-file replacement as battery RAM. It saves every 300 frames and on normal exit. The file contains thirteen calendar bytes, the three ASCII bytes `RTC`, and an eight-byte unsigned Unix timestamp in little-endian order. This 24-byte format is specific to this emulator. Invalid signatures and sizes stop loading without replacing the file.

## Sufami Turbo

Supply a 256 KiB BIOS and one or two game images:

```powershell
.\out\build\release\frontend\snes_frontend.exe --sufami "C:\games\bios.bin" "C:\games\slot-a.st" "C:\games\slot-b.st"
```

Omit the last argument to leave slot B empty. Use `-` for either empty slot, including both slots for BIOS-only operation. BIOS and game signatures are checked, and 512-byte copier headers are removed. Game images must be between 512 KiB and 1 MiB. This command takes separate files. The ordinary ROM command also loads [combined cartridge images](controllers-and-slots.md#combined-sufami-turbo-images).

| Address window | Device |
| --- | --- |
| $00-$1F/$80-$9F:$8000-$FFFF | BIOS, mirrored within 256 KiB |
| $20-$3F/$A0-$BF:$8000-$FFFF | Slot A ROM |
| $40-$5F/$C0-$DF:$8000-$FFFF | Slot B ROM |
| $60-$63/$E0-$E3:$8000-$FFFF | Slot A SRAM, mirrored within 16 KiB |
| $70-$73/$F0-$F3:$8000-$FFFF | Slot B SRAM, mirrored within 16 KiB |

Empty slots return open bus and have no save file. Each populated slot loads and saves a `.srm` beside its game image. A save follows the game when it moves between slots. The frontend rejects slots that resolve to the same save path. BIOS and game images are not included.

The core also exposes `LoadSufamiTurbo`, `LoadSufamiTurboFromFiles`, `SlotSramData`, and `LoadSlotSram` for callers that manage files themselves.

## Region timing and loader behavior

Country codes $00, $01, $0B, $0D, $0F, and $10 select NTSC. Other values select PAL. PAL uses 312 scanlines and a 21,281,370 Hz master clock; NTSC uses 262 scanlines and a 21,477,272 Hz clock. Timing, the PPU region flag, and SMP synchronization use the same selection. Audio output remains 32,040 Hz.

RAM declarations use their low four bits and stop at 256 KiB. GSU cartridges use an ordinary RAM declaration when present, otherwise the expansion declaration or the older board's 32 KiB default. Fixed processor memory remains device-specific. Database overrides can explicitly disable ordinary SRAM with a size of zero.

Ordinary LoROM drives only the upper half of each ROM bank. With RAM and at most 2 MiB of physical ROM, banks $70-$7D and $F0-$FF expose RAM in both halves, regardless of RAM capacity. Larger LoROM boards use only the lower half for RAM. A board without RAM keeps ROM in the upper half of bank $70. LoROM and HiROM retain connected address lines for larger images and mirror only unpopulated storage. MEMSEL controls fast accesses independently of the speed advertised in the header.

The loader considers headers at $7FC0, $FFC0, $407FC0, and $40FFC0. Each candidate is ranked by the instruction at its physical reset target, checksum complement, and agreement between mode and location. A positive extended-header score receives a size bias. Equal scores retain the earlier candidate in that order. If every score is zero, the first header remains the fallback, including blank or erased full-bank images. Images shorter than 32 KiB have no usable header.

The supplied bank order is retained. A 512-byte copier prefix is removed only when the remaining image consists of complete 32 KiB banks. Mapping hints do not trigger a bank permutation or chip rotation. A declared HiROM mode remains HiROM at $40FFC0; an unrecognized mode falls back to its physical header position. The header parser is shared by ordinary and competition cartridge loading.

## Validation and limits

`snes_cartridge_device_tests` covers device detection, rejected loads, bank boundaries, save separation, DSP-2 command results, OBC1 attribute packing, clock persistence and calendar rollover, and PAL frame/audio timing. Loader regressions preserve supplied ROM bytes, verify reset paths and connected bank addresses, and check competing headers, RAM caps, and region timing. Tests use generated ROMs and an injected clock, so they need no game files.

Local 1,800-frame checks produced visible output and non-silent audio for Super Mario World and the Contra prototype. The final Contra frame showed gameplay. These checks do not prove complete gameplay, correct sound, or compatibility with commercial games that use the new devices.

[Controllers and cartridge slots](controllers-and-slots.md) describes mouse, multitap, BS slot boards, and combined Sufami Turbo support. [S-DD1, DMA timing, and video output](dma-video-and-sdd1.md) covers streaming decompression, DMA register observation, physical ROM/RAM windows, and video output.

[Original hardware support](original-hardware.md) covers SA-1, Super FX/FX2, DSP-3/4, Cx4, SPC7110 and its RTC, and BS-X. ST011 executes its internal program when firmware is supplied and otherwise retains a limited packet interface. ST018 can execute supplied ARM firmware through its clocked CPU bridge; [ST018 firmware execution](st018.md) describes its memory map and validation limits. [Cx4 processor execution](cx4.md) describes its supplied data ROM, program execution, DMA, and CPU bus ownership. Register and mapper coverage does not establish full system compatibility.

[SPC7110 and cartridge RTC](spc7110.md) covers clocked decompression, arithmetic, data ports, physical board maps, and RTC handshakes.
