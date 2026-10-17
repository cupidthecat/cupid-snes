# Timed competition cartridges

Campus Challenge '92 and PowerFest '94 use four ROM chips, save RAM, a countdown controller, and DSP-1. Select the board explicitly and supply the boot image followed by the three game images:

```powershell
snes_frontend --competition92 boot.sfc game1.sfc game2.sfc game3.sfc
snes_frontend --competition94 boot.sfc game1.sfc game2.sfc game3.sfc --competition-dip=5
```

Each image must contain complete 32 KiB banks and be at most 4 MiB. The loader accepts copier headers and reads the console region from the boot image. It validates all four images before replacing the running cartridge.

The frontend uses 8 KiB of save RAM and stores it beside the boot image with a `.srm` extension. The core loading interface accepts a separate save capacity. Reset retains that RAM and restores the boot ROM selection.

## ROM and control maps

Each chip mirrors its own populated address blocks. Game selection never changes the fixed boot windows.

| Board | Status read | Game-selection write | Game selectors |
| --- | --- | --- | --- |
| 1992 | `$C0:0000` | `$E0:0000` | `$09`, `$05`, `$03` |
| 1994 | `$10:6000` | `$20:6000` | `$09`, `$0C`, `$0A` |

Other selection values choose the boot chip. The 1992 lower ROM window selects games while banks `$80-$9F` retain the boot chip. The 1994 board has full-bank ROM windows and a HiROM offset arrangement for its second game.

The 1992 DSP window spans `$20-$3F,$A0-$BF:$8000-$FFFF` and selects status throughout. The 1994 DSP uses `$00-$0F,$80-$8F:$6000-$6FFF` for data and `$7000-$7FFF` for status.

## Countdown

The low four DIP bits select a duration of 3 through 18 minutes. The upper bits do not affect the countdown. For example, `--competition-dip=5` selects eight minutes.

Writing `$09` starts or restarts the countdown. Its one-second phase continues while the timer is inactive and follows the console's PAL or NTSC master clock. Expiration sets status bit 1 and starts a five-second score interval. Restarting the countdown retains the time-over flag; reset clears it.

## DSP execution

Use `--firmware-dir=<directory>` or a `firmware` directory beside the boot image to provide the [cartridge processor firmware](clocked-cartridge-processors.md). Loading prefers `dsp1b.rom`, then `dsp1.rom`. The processor runs at 7.6 MHz and retains its internal data RAM across reset.

Without firmware, the board uses the DSP-1 command model. Its status accesses return the zero low byte followed by the high status byte. Command data has an independent byte phase. Processor firmware exposes its status byte directly.

## Checks

`snes_competition_cartridge_tests` covers game selection, fixed boot windows, ROM mirrors, every countdown setting under both video regions, save retention, firmware ports, processor RAM retention, and failed reloads. Generated 65816 programs execute from the boot mirror and read the games, status, and saves through the console bus.

`snes_cartridge_command_tests` also checks DSP-1 status byte phases across bank and address mirrors, command transfers, and reset.
