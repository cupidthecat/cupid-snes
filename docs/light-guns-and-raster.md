# Light guns and raster memory

The frontend accepts Super Scope, one or two Justifiers, and MACS rifle on console port two:

```powershell
.\out\build\release\frontend\snes_frontend.exe --port2=scope "C:\games\game.sfc"
.\out\build\release\frontend\snes_frontend.exe --port2=justifier "C:\games\game.sfc"
.\out\build\release\frontend\snes_frontend.exe --port2=justifiers "C:\games\game.sfc"
.\out\build\release\frontend\snes_frontend.exe --port2=rifle "C:\games\game.sfc"
```

## Controls

| Action | First gun | Second Justifier |
| --- | --- | --- |
| Aim | Mouse pointer | I/J/K/L |
| Trigger | Left mouse button | Right Ctrl |
| Start | Enter | Backspace |
| Offscreen aim | Hold O or move outside the picture | Hold U |
| Scope cursor | Right mouse button | None |
| Scope turbo switch | T toggles | None |
| Scope pause | P | None |

Green and magenta crosshairs show the two aim positions. They are drawn over the displayed picture and are absent from saved probe frames. Unfocused windows release the gun buttons and report offscreen aim. The frontend rejects a relative mouse on port one when port two uses a gun, since both would need the same host pointer.

Aiming follows the centered picture when the window resizes. Letterbox borders count as offscreen. High resolution uses the same 256 horizontal aim positions; interlaced output maps back to 224 or 239 visible rows. Windows smaller than the native picture scale down to fit.

## Controller and beam behavior

Scope packets contain eight button/status bits followed by ones. The first serial read captures the buttons. Cursor stays asserted while held; normal fire and pause are reported once per observed press. Turbo repeats fire while the trigger stays held. Its toggle button changes the controller's switch position when a first serial read observes a new press. Both strobe transitions restart the packet, and reads advance while the strobe stays high. Offscreen aim suppresses the trigger bit while retaining the other buttons and status bits.

Justifier packets contain the identification prefix, separate trigger and Start bits, and a selection bit that alternates on each falling strobe. Both strobe transitions restart the serial packet. Its first read captures the buttons, and reads advance even while the strobe stays high. A pair shares one serial port, with only the selected gun providing aim coordinates. Selecting the absent second gun in single-gun mode produces no counter latch. Reads after the 32-bit packet return one. The rifle exposes its live trigger level without a shift register.

Manual reads and automatic joypad polling share the same packets and positions. Scope and Justifiers capture direct aim coordinates at the display-frame event, when VBlank begins. This counter latch is independent of trigger presses, packet consumption and WRIO bit 7. These devices do not pulse RDIO. The rifle retains its raster-position pulse and WRIO input gating. Only console port two has the PPU counter connection. Offscreen positions do not replace the stored counters.

Core input providers override `PollLightGun(port, gun, frame)` and return `LightGunState`. Coordinates use 256 horizontal positions and direct, noninterlaced PPU counter rows before VBlank: 0 through 224, or 0 through 239 in overscan. The frontend maps its visible image rows into this coordinate space. Return current button levels on every call, including the turbo toggle button; the controller retains the switch position. Aim and button sampling occur at different points, and polling can happen more than once per frame.

## Raster fixes

The renderer previously saved registers and palettes per scanline but read tile memory and sprite attributes at VBlank. A later VRAM or OAM update could therefore change rows that had already passed. Backgrounds and Mode 7 now use the [background fetch pipeline](background-fetch.md). Objects use a separate [evaluation and fetch pipeline](object-fetch.md). Both read memory and registers at their consuming stages.

Fetched words and resolved layer samples retain the data needed by each stage. Later uploads cannot replace words already fetched. This removes the full VRAM and OAM snapshots formerly retained per scanline. A retained mutable `VramData()` pointer remains valid across reset; each fetch reads the memory present at its own clock.

Raster register writes are kept in separate lists for each scanline. Rendering a line no longer searches every event in the frame or allocates a temporary event list. Writes are ordered by their pixel positions, preserving write order at a shared position. Screen and background effects use their respective sampling clocks.

Palette access also used incompatible timing units: the scheduler supplied horizontal dots to comparisons expressed in master clocks. It now supplies master clocks, allowing CGRAM access during HBlank and redirecting active-display accesses through the rendering latch. The PPU setter names the clock unit explicitly.

## Validation and limits

Tests cover first-read snapshots, high-strobe shifting, packet endings, turbo-button transitions, gun selection, frame-event coordinates, rifle WRIO gating and beam arrival, offscreen input, automatic polling, viewport edges, and graphics-memory changes between scanlines. Frame-event checks cover both regions, display heights and video field modes, including cold/warm reset. Memory-lifetime tests also change VRAM on all 239 overscan lines, repeat across frames, reject blocked display-time writes, and keep a mutable pointer across reset. An integration test transfers palette data during active display and HBlank to check the clock-unit fix. The full test command and current hardware scope are listed in [original hardware support](original-hardware.md).

Super NES Super Scope 6 and Lethal Enforcers each complete a supplied-image check through 1,800 frames, with moving aim, trigger presses, held cursor/Start and offscreen input. Video matches at the recorded checkpoints, and every delivered stereo frame matches the common generated audio prefix. The audio producer can run ahead of the console frame boundary, so the checks retain both generated and delivered totals. Host crosshairs are excluded from the video observation. These runs cover the scripted paths, not complete playthroughs.

The rifle uses its scheduled aim position, while Scope and Justifiers use the display-frame event. These paths do not simulate optical brightness thresholds or sensor persistence. CPU register accesses advance hardware per bus cycle. Background names, pattern words, offsets, and Mode 7 samples follow their fetch and pixel clocks. Object evaluation and pattern-word reads follow their own clocks. Screen register writes split final composition at their pixel positions. [PPU memory ports](ppu-memory-ports.md) covers CPU access restrictions, byte phases, and palette ownership. Memory collisions within a clock and optical sensor behavior still need separate validation.
