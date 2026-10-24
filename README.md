# cupid-snes

A C++20 SNES emulator with an SDL3 desktop for Windows and Linux. The core can also run from tests and command-line tools.

<img src="img/smw.png" alt="smw" width="500"/>
<img src="img/rpg.png" alt="rpg" width="500"/>

The desktop provides game loading, recent games, controller and hotkey remapping, save states, cheats, RAM search, CPU debugging and graphics/audio inspection. The core models the console CPU, bus, DMA, clocked graphics and audio, cartridge mapping and enhancement hardware. MSU-1 data and PCM sidecars are supported.

This is a work in progress. Passing focused hardware tests or displaying a game does not establish complete compatibility. See [hardware support and remaining gaps](docs/original-hardware.md) and [game checks](docs/compatibility.md).

## Build

Use CMake 3.25 or newer, Ninja and a C++20 compiler for the provided presets. CMake fetches missing SDL3 and Dear ImGui dependencies during the first configure.

```powershell
cmake --preset release
cmake --build --preset build-release
ctest --test-dir out/build/release --output-on-failure
```

[Building and testing](docs/building.md) covers dependency versions, Debug builds, sanitizers and optimized checks.

## Run

Launch without arguments for the desktop, or pass a cartridge path:

```powershell
.\out\build\release\frontend\snes_frontend.exe "C:\path\to\game.sfc"
```

Ctrl+O or a dropped file opens a game. P pauses, N advances one field, F2/F4 save/load the selected quick slot, F3 opens the state manager and F11 toggles fullscreen. Preferences remaps keyboard/gamepad controls and selects controllers, display/audio options, firmware and save paths. See [desktop controls](docs/desktop.md) for the complete shortcuts and save behavior.

Default player-one keys are arrows for the D-pad, Z/X for B/A, A/S for Y/X, Q/W for L/R, Enter for Start and Right Shift for Select. Both console ports default to gamepads; connected host gamepads fill player slots in connection order. Escape saves and quits.

## Documentation

| Area | Details |
| --- | --- |
| Desktop and tools | [Controls, preferences and save states](docs/desktop.md), [cheats](docs/cheats.md), [memory and RAM search](docs/memory-tools.md), [CPU debugger](docs/cpu-debugger.md), [graphics/audio inspection](docs/hardware-inspection.md) |
| Games and cartridges | [Compatibility and saves](docs/compatibility.md), [hardware support](docs/original-hardware.md), [controllers and slots](docs/controllers-and-slots.md), [cartridge devices and regions](docs/cartridge-devices.md), [competition cartridges](docs/competition-cartridges.md) |
| Graphics and timing | [Rendering and LoROM](docs/accuracy-notes.md), [background fetches](docs/background-fetch.md), [object fetches](docs/object-fetch.md), [PPU ports](docs/ppu-memory-ports.md), [light guns](docs/light-guns-and-raster.md), [DMA and S-DD1](docs/dma-video-and-sdd1.md) |
| Cartridge processors | [Clocked processors](docs/clocked-cartridge-processors.md), [Cx4](docs/cx4.md), [SPC7110](docs/spc7110.md), [ST010](docs/st010.md), [ST018](docs/st018.md), [S-RTC](docs/srtc.md) |
| Audio and validation | [Audio playback](docs/audio.md), [MSU-1 packs](docs/msu1.md), [performance checks](docs/performance.md), [CPU regressions](docs/cpu-regressions.md), [cartridge/PPU/DMA regressions](docs/hardware-regressions.md) |
| Development | [Build and test](docs/building.md), [source layout](docs/source-layout.md) |

The emulator is not cycle exact. SA-1 cache timing and PPU memory collisions need more validation. Some cartridge devices use command or board protocols. BS-X live reception and SoundLink audio remain unimplemented; light guns use scheduled beam positions. The linked pages describe each subsystem's validation limits.
