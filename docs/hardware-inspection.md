# Graphics and audio inspection

Open **Tools > Graphics Inspector** (Ctrl+Shift+V) or **Tools > Audio Inspector** (Ctrl+Shift+A) while a game is loaded. Both commands open the same window, on the relevant tab. Shortcuts can be remapped in Preferences.

The window pauses playback and takes a copy of the current hardware state. Opening views, changing previews, refreshing and exporting do not run the machine or read device ports. **Next frame** explicitly advances one display field and captures a new snapshot. Audio stays silent while the tools are open. Closing the window restores the previous pause state.

Keyboard and pointer controls used by this window do not reach the game. Gamepad mappings remain available during explicit field advance, subject to the selected focus policy.

## Graphics

The Palette tab shows all 256 CGRAM colors. Select an index with the slider or click a color. Copy color places its index and raw value on the clipboard. Colors use the stored five-bit channels; display brightness and color math are not applied.

The Tiles tab decodes 2-, 4- and 8-bit planar tiles, or packed Mode 7 pixels from the high VRAM bytes. Each page contains 256 tiles. The base is an even physical byte address inside the configured 64 or 128 KiB of video memory. Tile reads wrap at that memory boundary. Palette bank selects groups of four or sixteen colors for the corresponding planar formats; 8-bit formats use the full palette. Transparent color zero replaces zero-valued pixels with a checkerboard. Invalid bases leave the previous preview available for retry.

The Objects tab lists all 128 OAM entries. It shows signed X, raw coordinates, the displayed Y coordinate, tile, palette, priority, size and flip flags. The preview uses the object palette and selected tile region. Fit object preview enlarges small objects; turn it off to choose a pixel zoom. Apply flips controls whether the preview uses the stored flip flags. These are raw object tiles at full height. Display interlace, scanline limits, priority and color math are not part of this preview.

## Audio

The Audio tab shows the console DSP's eight voices: signed left/right volumes, pitch, source, envelope and current BRR address. Select a voice to inspect its envelope mode, output and current sample-directory start/loop pointers. The plot shows the twelve recent decoded samples in ring order, before interpolation and mixing. It is not an output waveform. Expand DSP registers to read all 128 register bytes.

The global values include DSP phase, counter, noise, main/echo volumes, key-on/off masks and flags. Other audio producers and the host stream mixer are outside this console DSP view.

## Exports

Export view saves a BMP image from a graphics tab or a tab-separated audio snapshot from the Audio tab. Files go in the current game's state folder under `hardware-dumps`. The destination follows the state-directory preference. Each filename identifies the view and has a numeric suffix.

Images retain the raw preview pixels, including the checkerboard when transparency is enabled. Audio exports contain frame/master-clock positions, all voice rows and the complete DSP register table. Addresses and register bytes are hexadecimal; clocks and voice measurements are decimal. Envelope modes are 0 = Release, 1 = Attack, 2 = Decay and 3 = Sustain.

An export error stays in the window so the destination can be repaired and the operation retried. Existing files survive failed writes.
