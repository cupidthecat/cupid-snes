# Controllers and cartridge slots

## Controller setup

Each console port accepts `pad`, `mouse`, `multitap`, or `none`. Both default to `pad`. File > Preferences > Devices saves the choices and changes connected devices without restarting the game. Port two also accepts `scope`, `justifier`, `justifiers`, and `rifle`; see [light-gun setup and controls](light-guns-and-raster.md). Command-line flags override the named choices for that run; Apply in the editor saves the active choices.

```powershell
.\out\build\release\frontend\snes_frontend.exe --port1=mouse "C:\games\game.sfc"
.\out\build\release\frontend\snes_frontend.exe --port2=multitap "C:\games\game.sfc"
```

By default, the keyboard and first host gamepad control player one. Additional host gamepads control subsequent players in connection order. The keyboard uses arrow keys, Z/X for B/A, A/S for Y/X, Q/W for L/R, Enter for Start, and Right Shift for Select. Host gamepad face buttons map by position: south/east/west/north become B/A/Y/X. The left stick also controls the directional pad. File > Preferences > Input remaps all eight players, with four alternative controls per button. Each alternative accepts a key, gamepad button, signed gamepad axis, or mouse button.

A multitap takes four consecutive player slots. With a pad on port one and a multitap on port two, players two through five use the multitap. Two multitaps expose eight players. A mouse or empty port reserves one player slot. Disconnecting a host gamepad releases its buttons and preserves the other assignments; a new device takes the first vacant slot.

Each player can use its automatic gamepad slot, disable gamepad input, or select a particular connected gamepad. Explicit selections use the device GUID and its serial number or system path when available. Devices with neither use an ordinal; identical devices without a unique identity may need reselection after an application restart. A missing selected gamepad leaves its controls released. Keyboard and mouse alternatives remain independent of gamepad selection. Axis thresholds accept 1 through 32767; a control activates only when its magnitude exceeds the selected threshold.

Mouse mode captures relative movement in the window. Escape exits. The frontend supports one host mouse on either console port; the core can receive separate mouse input for both ports.

## Serial behavior

Manual reads at $4016/$4017 and automatic polling at $4218-$421F share the same controller packets and read positions. Automatic polling consumes sixteen bits per port. A subsequent manual read continues where polling stopped, which lets software read a mouse's movement after its button word.

Gamepads serialize B, Y, Select, Start, Up, Down, Left, Right, A, X, L, R, then four zero bits. Further reads return one. Opposing directions cancel. Each port receives independent input, including when an older input provider only supplies player one.

The CPU latch and automatic latch are combined. Gamepads capture buttons on the falling edge; reads while the latch is high return the live B button without advancing the packet. Multitaps return a two-bit identification value of 2 while latched. WRIO bit 6 selects the pad pair on port one, and bit 7 selects it on port two. Each pair keeps its own read position. An empty multitap socket returns zero even after the other socket finishes its packet.

Mouse packets contain eight zero bits, a button/sensitivity/signature byte, signed Y movement, and signed X movement. Both latch transitions sample movement and buttons. Reads with the latch high cycle the three sensitivity settings, which scale movement by 1, 1.5, or 2 before truncating fractional counts and clipping each magnitude to 127. Excess movement is discarded. The frontend keeps one host movement snapshot per frame so both latch transitions can sample it.

Core callers configure devices through `GetAutoJoypad().Ports().Configure(...)`. An explicit array assigns up to four player IDs to a port, with `-1` for an empty socket. Input providers can override `PollController(player, frame)` and `PollMouse(port, frame)`. Mouse providers return the current host input snapshot rather than clearing movement on each call. Reset preserves device assignments and clears protocol state.

## BS cartridge slots

```powershell
.\out\build\release\frontend\snes_frontend.exe --broadcast "C:\games\base.sfc" "C:\games\pack.bs"
```

Use `-` to leave the memory-pack slot empty. Supply a blank image to insert erased flash. Valid extended `Z??J` identifiers select LoROM, HiROM, or SA-1 slot boards; `ZBSJ` selects the BIOS controller. Maker byte $33 and a save-RAM declaration are required. Supported packs are 512 KiB mask ROMs and 1, 2, or 4 MiB flash or mask ROM images. A 512-byte copier header is accepted.

LoROM boards expose the pack at $C0-$EF with full 64 KiB bank addressing. HiROM boards expose pack data in $20-$3F/$A0-$BF:$8000-$FFFF, $60-$7D, and $E0-$FF. Their aliases share one device state. SA-1 MMC pages 4-7 select a populated pack; an absent pack retains base-ROM mirroring. Base ROM, save RAM, and console WRAM retain their own address windows.

Flash supports programming, status and identifier modes, page buffers, protection, and clocked block erase. SA-1 exposes pack reads through its ROM controller and does not forward writes as flash commands. Mask ROM packs remain read-only and have no flash save. See [broadcast controllers and memory packs](broadcast-memory.md) for commands, timing, and block addressing.

The frontend saves flash contents to the supplied pack filename with `.flash` appended, such as `pack.bs.flash`, and device metadata in `.flash.meta`. Saves load before execution and flush every 300 frames and on normal exit. They use the existing temporary-file replacement mechanism. A save of the wrong size stops loading. Empty slots have no flash save. Base cartridge SRAM remains in its `.srm` file.

The BS-X BIOS has a separate controller with 512 KiB PSRAM, header-sized battery RAM, latched mapping, a serial clock, and a clocked memory pack. The normal BIOS declares 32 KiB battery RAM. Core callers can attach recorded payloads through `LoadBroadcastStream` and inject a clock through `SetBroadcastTimeSource`. See [broadcast controllers and memory packs](broadcast-memory.md) for register behavior, save files, and limits.

## Combined Sufami Turbo images

The ordinary ROM command also accepts a combined image containing a base BIOS at offset zero, slot A at $100000, and optional slot B at $200000. The image must be 2 or 3 MiB. The loader checks the BIOS and game signatures and treats a slot filled entirely with $00 or $FF as empty. Truncated slots are rejected before replacing the loaded cartridge.

Combined images use one 32 KiB `.srm`: the first 16 KiB belongs to slot A and the second to slot B. The separate-file `--sufami` command still saves each game beside its own image. A standalone 256 KiB base BIOS maps the BIOS windows and leaves both game slots empty.

## Validation

The controller tests exercise manual/automatic polling through CPU I/O, mouse packets and sensitivity, independent multitap pair counters, empty sockets, and reads past packet end. SDL tests attach virtual gamepads and verify input, disconnects, and replacement devices. Cartridge tests cover the slot maps, flash commands, save round trips, combined images, and failed-load preservation. These tests use generated data and do not require game files.

Local 1,800-frame runs with Start at frame 600 produced 1,633 visible frames for Super Mario World and 1,448 for the Contra prototype. Both generated non-silent audio; the saved Contra frame showed gameplay. These runs check existing game behavior, not commercial mouse, multitap, or slot-cartridge compatibility.

[Original hardware support](original-hardware.md) describes the remaining cartridge devices and their validation limits. [S-DD1, DMA timing, and video output](dma-video-and-sdd1.md) covers the decompressor and transfer timing. Neither mapper coverage nor recorded packet delivery establishes full commercial-game or broadcast-service compatibility.
