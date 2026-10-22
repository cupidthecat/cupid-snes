# Desktop controls

Run `snes_frontend` without arguments to open an idle window. Ctrl+O opens a console cartridge. Dropping a cartridge file into the window opens it immediately. You can also pass a game path and controller or firmware options on the command line.

On Windows, the File menu also opens Super Game Boy, Sufami Turbo, broadcast, Campus Challenge 92, and PowerFest 94 cartridges. Each picker names the asset it needs. Cancelling any step keeps the current game. Sufami Turbo offers one or two game slots, and broadcast cartridges offer a pack or an empty slot. Existing command-line launch modes remain available.

| Control | Action |
| --- | --- |
| Ctrl+O | Open a console game |
| Ctrl+W | Save and unload the current game |
| Ctrl+Q or Escape | Save and quit |
| P | Pause or resume |
| N | Pause and advance one field |
| F5 | Reset the loaded machine |
| Game > Power Cycle | Restart the hardware while retaining cartridge storage |
| F11 or Alt+Enter | Toggle fullscreen |
| F12 | Save a screenshot |

Shortcuts ignore key repeat. Press N several times to queue that many field advances. Reset and power cycle retain the pause setting and do not advance a paused game. Resize the window to fit your display; the picture keeps its current aspect and integer scale when space allows. Windows provides native menus. Other SDL hosts use the keyboard and file drop controls.

Pausing clears queued host audio. Frame advance runs silently. Resuming collects a fresh audio buffer before playback starts. Opening a picker pauses the current game and restores its previous pause setting when cancelled. Quitting with a picker open cancels it before releasing the window on Windows; on other hosts, dismiss the picker to finish quitting.

Screenshots are BMP files at the generated picture's native dimensions. They exclude window borders, scaling, and light-gun sights. They are saved under the SDL preference directory in `screenshots`, normally `%APPDATA%\Cupid\Cupid-SNES\screenshots` on Windows. The window caption reports the saved path. A screenshot requires a generated game picture.

## Battery files

Opening a replacement first flushes the current game's saves. A load error leaves the current machine available. A save error keeps the game loaded so you can fix the file or directory and retry. Normal unload, exit, and power cycle flush saves; running games also flush changed data every 300 fields.

Console battery RAM uses a neighboring `.srm` file. Clock storage uses `.rtc`. A separate broadcast pack owns `<pack-path>.flash` and `<pack-path>.flash.meta`; without a separate pack, those files belong to the console cartridge path. Super Game Boy uses a `.sav` beside the handheld game. Each Sufami Turbo game owns its own `.srm`, even when moved between slots. The frontend rejects two slots that would overwrite the same save file.

Game paths, firmware directories, save files, and picker results use UTF-8, including on Windows. The frontend preserves full Unicode paths when opening files and reporting errors.

These files contain battery storage, clocks, and pack identity. They do not capture a running machine's complete execution state.
