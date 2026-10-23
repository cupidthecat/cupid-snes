# Memory and RAM search

Tools > Memory opens a byte viewer. Tools > RAM Search opens the search page. The default shortcuts are Ctrl+Shift+M and Ctrl+Shift+F. Preferences > Hotkeys can remap both commands to keyboard or gamepad controls.

Opening either page pauses the game. Closing it restores the previous pause setting. The tools read storage directly without advancing clocks, sampling input, acknowledging interrupts or reading device ports. Other gameplay commands and dropped games are blocked while the window is open. Quitting remains available.

## Memory viewer

The Region list contains the physical storage present on the loaded hardware. Console regions include work RAM, audio RAM and boot ROM, video RAM, object attributes, color RAM, cartridge ROM, save RAM and inserted packs. Supported cartridge hardware also exposes its internal RAM or instruction cache. Handheld cartridges add their ROM, boot ROM, work RAM, high RAM, video RAM, object memory and wave RAM.

Addresses belong to the selected region. Work RAM uses its fixed CPU address, beginning at `7E0000`. Handheld work RAM begins at `C000` and high RAM at `FF80`. Cartridge ROM and storage with variable bank mapping use physical offsets beginning at zero. Region labels and sizes distinguish these address spaces. These views do not simulate reads from I/O registers.

Enter a hexadecimal address and choose Go. Previous and Next move by 256 bytes. Refresh reads the current page again. The final page includes only bytes that exist in the region. Video and color words appear in little-endian byte order on every host. The video RAM size follows the loaded configuration.

Export region saves the complete selected region to a `.bin` file under `memory-dumps` in the game's configured state folder. The window reports the destination or a file error. An export uses a temporary file and publishes it only after all bytes have been written.

## Searching

Choose 8, 16, 24 or 32 bits per value. Signed interprets the highest bit as the sign bit. Big endian changes byte assembly. Aligned considers only offsets divisible by the selected value width; otherwise, every complete byte position is considered. Changing these settings requires a new search.

New search (unknown) captures an initial sample and retains all positions where a complete value fits. Search can also start this capture automatically before its first filter. The search keeps every candidate, including results outside the visible rows. Scrolling the table does not discard or truncate candidates.

Each filter compares the newly sampled value with one of three operands:

| Compare to | Operand |
| --- | --- |
| Value | A decimal value, or hexadecimal prefixed with `0x` or `$` |
| Previous sample | The value captured by the preceding successful search |
| Initial sample | The value captured when the current search started |

Equal and Not equal against Previous sample find unchanged and changed values. Greater and Less find increases and decreases. Add to compared value accepts a decimal adjustment for previous or initial samples. For example, Equal with an adjustment of `-1` finds values that decreased by one. Arithmetic comparisons do not wrap at the selected byte width.

Signed hexadecimal input accepts raw bit patterns: `$FF` means `-1` in an 8-bit signed search. Decimal values must fit the selected signed or unsigned range. Invalid input preserves the search for correction and retry.

Current, Previous and Initial show captured values rather than ongoing reads of the running game. Close the tools, perform an action in the game, then reopen and refine the retained candidates. Refreshing the byte viewer does not replace search samples. Undo filter restores the preceding candidate set and samples. At least one undo is retained; additional history is limited to 16 filters and 64 MiB. Clearing the search, changing regions or opening another game removes its samples.

## Creating cheats

Select a result and choose Create cheat. Directly mapped work RAM and handheld high RAM can produce codes for the loaded system. Multi-byte values become a group of byte codes in physical address order. The cheat editor opens with the new group disabled for review. Enable it and Apply to save and activate it. Closing the editor leaves the saved cheat file unchanged.

Regions with variable bank mapping retain physical offsets. Their Create cheat button is unavailable because an offset alone does not identify a CPU address. These regions remain readable, searchable and exportable. Enter a mapped code manually in the cheat editor when its address is known.
