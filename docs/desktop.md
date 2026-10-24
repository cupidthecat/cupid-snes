# Desktop controls

See [cheats](cheats.md) for code formats, per-game files and the cheat editor.
See [memory tools](memory-tools.md) for physical memory inspection, exports and RAM search.
See [CPU debugging](cpu-debugger.md) for instruction stepping, breakpoints, watchpoints and traces.
See [hardware inspection](hardware-inspection.md) for palette, tile, object and console DSP views.

Run `snes_frontend` without arguments to open an idle window. Ctrl+O opens a console cartridge. Dropping a cartridge file into the window opens it immediately. You can also pass a game path and controller or firmware options on the command line.

The File menu on Windows and Linux also opens Super Game Boy, Sufami Turbo, broadcast, Campus Challenge 92, and PowerFest 94 cartridges. Each picker names the asset it needs. Cancelling any step keeps the current game. Sufami Turbo offers one or two game slots, and broadcast cartridges offer a pack or an empty slot. Existing command-line launch modes remain available.

File > Recent Games remembers the last ten successful launch requests. A multi-file entry retains its cartridge mode, all asset paths, and its competition DIP setting. Reopening a game moves it to the front. Clear Recent Games removes the saved launch history. Missing assets produce a load error and keep the current game loaded.

| Default control | Action |
| --- | --- |
| Ctrl+O | Open a console game |
| Ctrl+W | Save and unload the current game |
| Ctrl+, | Open preferences |
| Ctrl+Q or Escape | Save and quit |
| P | Pause or resume |
| N | Pause and advance one field |
| F2 | Save the selected quick slot |
| F3 | Open the state manager |
| F4 | Load the selected quick slot |
| F5 | Reset the loaded machine |
| F6 / F7 | Select the previous / next quick slot |
| F8 | Toggle all enabled cheats for the current game |
| Ctrl+Shift+C | Open the cheat editor |
| Ctrl+Shift+M | Open the memory viewer |
| Ctrl+Shift+F | Open RAM search |
| Ctrl+Shift+D | Open the console CPU debugger |
| Ctrl+Shift+V | Open graphics inspection |
| Ctrl+Shift+A | Open audio inspection |
| Ctrl+Z / Ctrl+Y | Undo / redo a state load |
| Game > Power Cycle | Restart the hardware while retaining cartridge storage |
| F11 or Alt+Enter | Toggle fullscreen |
| F12 | Save a screenshot |

Shortcuts ignore key repeat. The Hotkeys preference page remaps them. Press N several times to queue that many field advances. Reset and power cycle retain the pause setting and do not advance a paused game. Resize the window to fit your display; the picture keeps its current aspect and integer scale when space allows. Windows and Linux share the same Dear ImGui menus and preference editor. Menu and status bars sit outside the game picture and light-gun input area. Keyboard and pointer input used by the UI is blocked from the game; gamepad input remains independent.

Pausing clears queued host audio. Frame advance runs silently. Resuming collects a fresh audio buffer before playback starts. Opening a picker pauses the current game and restores its previous pause setting when cancelled. Quitting with a picker open cancels it before releasing the window on Windows; on other hosts, dismiss the picker to finish quitting.

Screenshots are BMP files at the generated picture's native dimensions. They exclude menus, status text, window borders, scaling, and light-gun sights. They are saved under the SDL preference directory in `screenshots`, normally `%APPDATA%\Cupid\Cupid-SNES\screenshots` on Windows. The status bar reports the saved path. A screenshot requires a generated game picture.

## Save states

Game > Quick Slot selects one of ten slots. F2 replaces that slot and F4 loads it. The selection survives a restart. The state manager offers named states, previews, dates, and the same quick slots. Add State creates a named checkpoint. Save replaces the selected entry, Rename changes a managed state's name, and Remove deletes the selected file. Replacement and removal ask for confirmation. Cancel or Escape preserves the file. These controls and their remappable keyboard/gamepad shortcuts work on both desktop platforms.

Opening the manager pauses playback. Closing it restores the previous pause setting. A successful load refreshes the completed game picture immediately, including while paused, and discards audio queued from the previous timeline. Undo Load returns to the execution saved just before the latest successful load; Redo Load reapplies it. Saving a state preserves this undo history. Opening another game or unloading clears it. Undo history belongs to the current session and is not stored across application restarts.

Capture finishes in-flight instructions and DMA while retaining any picture, input, and audio events crossed along the way. A checkpoint includes the resulting partial field and all required cartridge hardware. Loading validates the file and loaded hardware before capturing undo or replacing execution. Invalid versions, damaged files, different cartridges, and changed or missing streamed media leave the game and previous undo history available. State loading does not read or flush battery files. Later battery saves write the restored cartridge data normally.

State operations show progress while checking media, reading or writing files, and preparing the machine. Cancel or Escape stops the operation before replacing a state file or restoring execution. Cancellation preserves the loaded game, existing files, pause setting, and undo history. Capture may already have completed pending fields; those fields and their input and output remain part of the running game. Game commands are unavailable while progress is open. Closing the application cancels the operation, then exits after it unwinds.

Selecting a state validates its preview without restoring or advancing the game. Large previews show progress. Cancel or Escape keeps the manager open; select the state again to retry. Damaged files show an error instead of unchecked pixels. Preview completion preserves errors from a rejected load. Focus settings also apply during capture: fields sampled after focus loss receive blocked input unless background input is allowed. Capture still finishes pending hardware work before saving a checkpoint.

Preferences > Paths selects the state directory. A blank setting uses a `states` folder beside the game. Each launch has its own subfolder, derived from its cartridge mode, all canonical asset paths, and competition DIP setting. Handheld games and different slot/pack combinations therefore keep separate collections. Changing the directory changes future state destinations without moving existing files or replacing the loaded machine. See [State files](state-files.md) for the format and filename rules.

## Battery files

Opening a replacement first flushes the current game's saves. A load error leaves the current machine available. A save error keeps the game loaded so you can fix the file or directory and retry. Normal unload, exit, and power cycle flush saves; running games also flush changed data every 300 fields.

A periodic save error pauses playback and shows the failed path. Repair the destination and close the error to resume; the next periodic save or a normal quit retries the write. The loaded game and its unsaved battery data remain available.

By default, console battery RAM uses a neighboring `.srm` file. Clock storage uses `.rtc`. A separate broadcast pack owns `<pack-path>.flash` and `<pack-path>.flash.meta`; without a separate pack, those files belong to the console cartridge path. Super Game Boy uses a `.sav` beside the handheld game. Each Sufami Turbo game owns its own `.srm`, even when moved between slots. The frontend rejects two slots that would overwrite the same save file.

File > Preferences > Paths sets a shared battery-save folder. Files keep their owning game's or pack's basename and the extensions above. A relative folder starts at the profile's directory. Blank restores each owner's original folder. A new folder is created when selected.

Changing the folder while a game is loaded keeps its live battery data. Future saves write that data to the new folder, replacing matching filenames there. Selecting a folder does not read or write battery files. Previous files remain at their last saved contents. You can choose a new folder even when the old destination cannot be written. Reopening a game reads the selected folder. Invalid folder types, directories occupying save filenames, colliding slot filenames, or a failed preference write leave the current destinations active. Repair the problem and retry Apply.

Game paths, firmware directories, save files, and picker results use UTF-8, including on Windows. The frontend preserves full Unicode paths when opening files and reporting errors.

The UI uses available system fonts. On Linux, install Noto Sans CJK for Japanese and other CJK filenames. Missing display glyphs do not change the stored paths.

These files contain battery storage, clocks, and pack identity. They do not capture a running machine's complete execution state.

## Preferences

Open File > Preferences or press Ctrl+, to change video, audio, focus, input bindings, paths, connected devices, and hotkeys. Apply saves and activates the changes. OK also closes the editor. Cancel or Escape discards changes made since the last Apply. Opening the editor pauses emulation; closing it restores the previous pause setting. A rejected change leaves the editor open with an error and retains the previous settings.

Preferences use a UTF-8 `preferences.cfg` in the SDL preference directory, normally `%APPDATA%\Cupid\Cupid-SNES` on Windows and `$XDG_DATA_HOME/Cupid/Cupid-SNES` on Linux (`~/.local/share` when unset). Use `--preferences=<file>` to choose a separate configuration for portable setups or different controller layouts. You can also edit the file while the application is closed.

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
path.saves ""
```

Scale accepts 1 through 6. Aspect correction uses an 8:7 pixel aspect. Integer scaling and linear filtering affect presentation; screenshots retain the generated pixels. Volume accepts 0 through 200 percent. Mute and volume leave the emulated sound samples and timing unchanged. An empty or unavailable audio device name selects the system default.

Focus mode 0 pauses while the window is unfocused, hidden, or minimized. Mode 1 continues emulation while blocking controller input. Mode 2 continues with controller input enabled. Returning focus resumes an automatic pause and preserves a manual pause. Relative mouse capture is released while paused or unfocused.

`path.firmware` supplies cartridge processor firmware when the command line does not specify a directory. Changing it in the editor applies to the next game opened. An empty screenshot path uses the default `screenshots` directory. `path.saves` overrides the [battery-file directory](#battery-files), including for a loaded game. Relative directory paths start at the folder containing the preferences file. Recent entries store complete launch requests and absolute asset paths.

The Input page selects one of eight players and one of four alternative bindings per console button. Click a binding, then press a key, gamepad button, signed axis, or mouse button. Escape cancels capture. Clear removes that alternative; Restore player defaults resets the selected player. The Gamepad selector chooses an automatic player slot, a specific connected device, or Disabled. Refresh updates the device list. An unavailable saved selection stays released until it reconnects. Keyboard and mouse alternatives still work when the gamepad is disabled. Only player one has keyboard bindings by default.

Check Turbo to edit separate rapid-fire controls for buttons other than the directional pad. Holding a turbo control takes precedence over the button's normal bindings. The General page's Turbo period sets the number of fields released, then the number held, from 1 through 8 (default 4). All turbo controls share this cycle. It follows completed emulated fields, including frame advance and fields completed while preparing a checkpoint. Paused host time does not advance it. Reset, power cycle, and changes to other input settings retain the cycle; changing its period restarts it. No turbo controls are assigned by default.

Input records use `player.<1..8>.device` (`auto`, `none`, or a saved host identity) and `.threshold` (1 through 32767, default 16000). Each `player.<1..8>.<button>.<0..3>` record has `.type` and `.control`. Types are 0 for unassigned, 1 for an SDL keyboard scancode, 2 for an SDL gamepad button, 3 for the positive half of an SDL gamepad axis, 4 for its negative half, and 5 for a mouse button numbered 1 through 5. Unassigned controls store 0. Button names are `up`, `down`, `left`, `right`, `a`, `b`, `x`, `y`, `l`, `r`, `start`, and `select`. Legacy `key.<button>` entries supply player one's first alternative when no new record overrides it; saves retain those entries for keyboard bindings in that slot.

Turbo bindings use `player.<1..8>.turbo.<button>.<0..3>.type` and `.control`. The period is stored as `input.turbo-frequency`.

The Peripherals page remaps the mouse and each light gun. Buttons have four normal alternatives and four turbo alternatives; offscreen aim has normal alternatives only. The first gun uses the host pointer by default. Turn off Use pointer aim to control it with relative axes. Automatic gamepad selection uses the first host slot for the mouse and first gun, and the second slot for the second gun.

Each horizontal or vertical axis has four sources that add together. A source can use mouse X/Y movement, a full gamepad axis, or a digital control. Gain accepts 1 through 400 percent; Reverse changes its sign. Mouse sources use relative host motion. A gamepad axis supplies its signed value divided by 256, rounded down. A held digital control supplies one unit. Relative gun movement advances once per emulated field and stays within the picture's aim range. Pointer capture by the UI blocks mouse sources while gamepad sources remain available. Absolute pointer aim requires window focus, including when background controller input is allowed.

Peripheral records start with `peripheral.mouse.`, `peripheral.gun1.`, or `peripheral.gun2.`. Each stores `device` and `threshold`; guns also use `pointer` (0 or 1). Mouse button names are `left` and `right`. Gun button names are `trigger`, `cursor`, `turbo`, `pause`, `start`, and `offscreen`. Normal and turbo alternatives use the same suffixes as player bindings. Axis records use `x.<0..3>.` or `y.<0..3>.`, followed by `type`, `axis`, `scale`, `invert`, `digital.type`, and `digital.control`. Axis types are 0 for unassigned, 1 for mouse X, 2 for mouse Y, 3 for a gamepad axis, and 4 for a digital control. Only gamepad sources set `axis`; only digital sources assign `digital.type` and `digital.control`.

The Devices page connects gamepads, mice, multitaps, light guns, and the console expansion device. Applying a change retains the loaded game, execution position, work RAM, and cartridge saves. Reapplying the same configuration keeps current controller polling state. Port 2 moves to player 5 when port 1 has a multitap. One mouse or empty port reserves one player slot. The frontend supports one host pointer, so a relative mouse cannot be paired with another mouse or a light gun. Light guns connect to port 2.

`input.port1` and `input.port2` store 0 for none, 1 for gamepad, 2 for mouse, and 3 for multitap. Port 2 also accepts 4 for Super Scope, 5 for Justifier, 6 for dual Justifiers, and 7 for MACS rifle. `input.expansion` stores 0 for none or 1 for Satellaview. Command-line `--port1`, `--port2`, and `--expansion` flags override only the named choices for that run. The editor shows the active choices; Apply saves them to the profile.

The Hotkeys page provides four alternative keyboard bindings for each desktop action. Select a binding number, find the action, and click its shortcut to capture a key with optional Ctrl, Alt, Shift, or Super modifiers. Escape cancels capture; Use Escape assigns an unmodified Escape key. Clear removes one binding, and Restore hotkey defaults resets all actions. Modifier keys alone trigger on release and are cancelled when used as part of another chord or mouse click. A chord assigned to two different actions is rejected. Menus show the active shortcuts.

Check Gamepad to edit four additional alternatives per action. Each shortcut accepts up to four buttons or signed axes held on the same gamepad. Choose Any gamepad or a specific host device, then capture each control and select Done. Shortcuts trigger once when the full chord becomes held. Releasing and pressing it again rearms it. Gamepad shortcuts work while paused or idle and require window focus. Controls held during UI capture stay disarmed until released. Conflicting chords, including one action's chord contained in another's, are rejected. No gamepad shortcuts are assigned by default.

Each binding uses `hotkey.<action>.<0..3>.key` for its SDL scancode and `.modifiers` for a bit mask: Shift = 1, Ctrl = 2, Alt = 4, Super = 8. Add modifier values to combine them. Caps Lock and Num Lock state are ignored. In a chord, left and right versions of a modifier are equivalent. For example, these records assign Ctrl+Shift+F8 to pause:

```text
hotkey.pause.0.key "65"
hotkey.pause.0.modifiers "3"
```

A gamepad alternative uses `hotkey.<action>.<0..3>.pad.device` (`any` or a saved host identity), `.pad.threshold`, and `.pad.<0..3>.type`/`.control`. The four inner controls form a chord and use input types 0, 2, 3, or 4. Thresholds use the same range as player inputs.
