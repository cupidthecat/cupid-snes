# Cheats

Open Tools > Cheats or press Ctrl+Shift+C while a game is loaded. The editor pauses playback. Closing it restores the previous pause setting. Add creates a group, Remove deletes the selected group, and each group has its own checkbox. Enter one code per line or separate codes with `+`.

Enable cheats controls all checked groups for the current game. F8 toggles this flag during gameplay. Both shortcuts can be remapped under Preferences > Hotkeys, including gamepad bindings. Apply validates every group, saves the file and updates the loaded machine. Close discards edits made since the last successful Apply. Reload file discards edits and reads the saved groups again.

Invalid codes and failed file writes keep the editor open for correction and retry. A damaged file leaves the game available with its cheats inactive. The status bar reports the problem. The editor can reload a repaired file, or replace it after you select Replace unreadable file when applying.

## Code formats

Letters are case insensitive. Spaces and tabs around each code are allowed.

| System | Format | Meaning |
| --- | --- | --- |
| Console | `XXXX-XXXX` | Game Genie |
| Console | `AAAAAADD` | Pro Action Replay |
| Console | `AAAAAA=DD` | Write a byte at a 24-bit CPU address |
| Console | `AAAAAA=CC?DD` | Write when the original byte matches `CC` |
| Super Game Boy | `XXX-XXX` or `XXX-XXX-XXX` | Handheld Game Genie |
| Super Game Boy | `01DDLLHH` | GameShark type 01, low address byte before high byte |
| Super Game Boy | `AAAA=DD` or `AAAA=CC?DD` | Substitute a read at a 16-bit handheld address |

For example, `7E1234=AB` writes `AB` to console work RAM at `7E:1234`. This example describes the address format; it is not a game-specific cheat.

Console codes write on activation and after each completed display field. A conditional code samples its comparison byte when first activated. It stays inactive until removed and enabled again if that initial comparison fails. Removing active codes restores their saved bytes. Group and code order determine write order; later writes win when addresses overlap. Identical active codes are applied once. Reapplying an unchanged list retains its original bytes and conditional decisions.

ROM patches affect memory in the running process. They do not rewrite game files. Mapped cartridge ports retain their write behavior, including ROM windows that reject writes. The loaded image's checkpoint identity stays stable while ROM patches are active. Codes that address device registers use those registers' normal read and write side effects.

Super Game Boy codes target the handheld machine. Reads use the first code whose address and optional comparison match the current byte. Underlying memory stays unchanged. Console and handheld code files are distinct formats.

Reset and power cycle retain selected cheats. Opening another game loads that game's file. Save states store machine execution and memory; cheat settings remain attached to the current desktop session. Loading a state retains current ROM patches and handheld substitutions. Active console RAM writes resume at the next completed field. Disabling a console code still restores the byte saved when it was activated.

## Files

The editor displays its full file path. Each launch request gets `cheats.ccht` under its default game-specific folder in `states` beside the primary game. The folder identity includes all cartridge, slot and firmware paths and the cartridge mode. Changing the save-state directory preference does not move cheat files.

Files use UTF-8 text. Version 1 begins with `CupidCheats 1 console` or `CupidCheats 1 handheld`, followed by `enabled 0` or `enabled 1`. Each following line has `cheat`, its enabled flag, a quoted name and quoted codes. Backslashes and quotation marks inside quoted strings are escaped with a backslash. Writers normalize codes to uppercase address notation and join each group's codes with `+`.

```text
CupidCheats 1 console
enabled 1
cheat 0 "Example group" "7E1234=AB+7E1235=00?CD"
```

A file supports at most 1024 groups, 4096 codes in total and 1 MiB of text. Names must contain 1 through 4096 bytes without control characters. Unknown versions, wrong systems, malformed entries and invalid disabled codes are rejected. Successful writes replace the destination through a temporary file. A failed write preserves the prior file and removes its temporary file.
