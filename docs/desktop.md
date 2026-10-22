# Desktop controls

Run `snes_frontend` without arguments to open an idle window. Ctrl+O opens a console cartridge. Dropping a cartridge file into the window opens it immediately. You can also pass a game path and controller or firmware options on the command line.

On Windows, the File menu also opens Super Game Boy, Sufami Turbo, broadcast, Campus Challenge 92, and PowerFest 94 cartridges. Each picker names the asset it needs. Cancelling any step keeps the current game. Sufami Turbo offers one or two game slots, and broadcast cartridges offer a pack or an empty slot. Existing command-line launch modes remain available.

File > Recent Games remembers the last ten successful launch requests. A multi-file entry retains its cartridge mode, all asset paths, and its competition DIP setting. Reopening a game moves it to the front. Clear Recent Games removes the saved launch history. Missing assets produce a load error and keep the current game loaded.

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

## Preferences

Preferences use a UTF-8 `preferences.cfg` in the SDL preference directory, normally `%APPDATA%\Cupid\Cupid-SNES` on Windows. Use `--preferences=<file>` to choose a separate configuration for portable setups or different controller layouts. Edit the file while the application is closed; changes apply at the next launch.

The first line is `Cupid-SNES preferences 1`. Each record contains a key and a quoted value. Backslashes and quotation marks inside values are escaped with a backslash. Unknown keys survive a save. Unsupported versions, duplicate keys, and invalid values produce an error and preserve the original file.

```text
Cupid-SNES preferences 1
video.scale "3"
video.aspect "0"
video.integer "1"
video.linear "0"
audio.volume "100"
audio.mute "0"
audio.mute-unfocused "0"
audio.device ""
input.focus "0"
path.firmware ""
path.screenshots ""
```

Scale accepts 1 through 6. Aspect correction uses an 8:7 pixel aspect. Integer scaling and linear filtering affect presentation; screenshots retain the generated pixels. Volume accepts 0 through 200 percent. Mute and volume leave the emulated sound samples and timing unchanged. An empty or unavailable audio device name selects the system default.

Focus mode 0 pauses while the window is unfocused, hidden, or minimized. Mode 1 continues emulation while blocking controller input. Mode 2 continues with controller input enabled. Returning focus resumes an automatic pause and preserves a manual pause. Relative mouse capture is released while paused or unfocused.

`path.firmware` supplies cartridge processor firmware when the command line does not specify a directory. An empty screenshot path uses the default `screenshots` directory. The `key.up`, `key.down`, `key.left`, `key.right`, `key.a`, `key.b`, `key.x`, `key.y`, `key.l`, `key.r`, `key.start`, and `key.select` entries store SDL scancode numbers for the keyboard layout. Recent entries store complete launch requests and absolute asset paths.
