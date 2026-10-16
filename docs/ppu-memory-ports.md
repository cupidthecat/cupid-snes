# PPU memory ports

CPU accesses to VRAM, OAM, and CGRAM consume their register address and byte phase even when rendering blocks or redirects the memory access. The graphics pipeline advances to the access clock before the register is consumed.

## Access ownership

| Memory | Display enabled | Forced blank or vertical blank |
| --- | --- | --- |
| VRAM | Reads fetch zero and writes are discarded from scanline zero through the last visible line. | The CPU address selects memory. |
| OAM | Evaluation and object fetching supply the memory address. | The CPU address selects memory. |
| CGRAM | The palette lookup address owns accesses on lines 1 through `vdisp - 1`, from H=88 through H=1094. | The CPU address selects memory. |

Horizontal positions use master clocks. CGRAM returns to CPU addressing at H=1096 and during the rest of horizontal blanking. Vertical blank starts at line 225, or line 240 with overscan enabled. Scanline zero blocks VRAM and redirects OAM, but leaves CGRAM under CPU control.

VRAM reads return the prefetched word before fetching again and incrementing the address on the selected byte port. Enabling display therefore preserves a word fetched during blanking; a later blocked fetch replaces it with zero. Discarded writes still perform the configured increment. Address remapping and the 64 KiB physical mirror apply to permitted memory accesses.

## Palette lookup address

Line start reads backdrop color zero. Each output dot then resolves the sub screen before the main screen. A selected background or object that uses CGRAM leaves its palette address on the bus. An empty screen reads backdrop zero. A layer window can remove a candidate; color-window clipping still consumes the selected palette word.

Direct color on BG1 in modes 3, 4, and 7 bypasses CGRAM. A direct-color main screen retains the sub screen's preceding palette address. If both screens use direct color, they retain the address from the preceding lookup. Forced blank suspends pixel lookups while allowing the CPU to use its own palette address.

CGRAM reads and writes share one low/high byte phase. `$2121` resets that phase and sets the CPU address, while retaining the write-data latch. A low read followed by a high write therefore uses the low byte from the preceding low write. A high read preserves PPU2 open-bus bit 7. Completed pairs advance the CPU address, including redirected accesses.

## Validation and limits

`snes_ppu_port_tests` exercises palette ownership, screen order, direct color, layer and color windows, object lookups, blanking, clock and line boundaries, mixed CGRAM reads and writes, and VRAM prefetch retention. The existing hardware regression suite also checks OAM mirrors, address wrapping, and open-bus bits.

A local register-stream audit matched 524,288 operations, 3,145,728 port-state values, and 34,373,632 final memory cells. It covered VRAM remapping and increments, OAM mirrors and rotation, palette byte phases, forced blank, overscan, and access boundaries. A separate audit matched 65,536 palette-address results across generated layer priorities, windows, direct color, and display states.

These checks cover the modeled register and pixel clocks. They do not establish physical memory collisions within a clock, optical gun sensing, or complete game compatibility.
