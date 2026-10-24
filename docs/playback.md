# Movies, rewind and speed

Tools > Input Movies manages recordings for the loaded game. Record Current State captures a complete checkpoint, then records each input query made by the console. Reset and Record starts after a warm reset. The record shortcut starts from the current state. Closing the manager restores the prior pause setting; N advances a paused recording one field at a time.

Stop and Save writes a `.cmv` file under `movies` in the current launch's state folder. Stop, unload, replacement and normal quit save an active recording before releasing the machine. A cancelled or failed write keeps the completed recording in memory. Repair the destination and use Stop again, or open the manager to choose another name. Replacing an existing file in the manager requires confirmation. Discard Recording also requires confirmation and returns to live input.

Select a saved movie and choose Play Selected to restore its initial checkpoint. Close the manager to resume the prior play state. Playback uses the recorded controller, mouse, gun and real-time clock values. Live device polling does not replace those values. Game assets remain external, and the loaded hardware, media and effective cheats must match the recording. Playback validates the start position before replacing execution and pauses at the exact recorded endpoint. Stop Movie resumes live input. Undo State Load returns to the machine from before playback.

Reset, port changes, cheat edits, manual state operations and debugging tools require stopping the movie first. Pause, frame advance, screenshots and host speed controls remain available. Host battery saves keep their usual behavior and do not consume recorded clock queries. A query or endpoint mismatch pauses the game and reports an error; stop playback or rewind to a retained point to recover. Movies are intended for the same compatible emulation build, not as a cross-version interchange format. See [the file format](input-movies.md).

## Rewind

Preferences > Playback enables rewind and selects a capture interval, checkpoint count and memory limit. Rewind is disabled by default. Old checkpoints expire when either limit is reached. The memory setting counts retained checkpoint bytes; one detached capture, its completed bytes and temporary restore work can exist in addition. Capture failure disables further history collection for that session and reports the reason. Existing retained checkpoints remain available.

Hold Backspace to travel backward at roughly four times the covered console time. Release it to continue from the restored point. Shift+Backspace restores one earlier checkpoint. Both controls work while paused. Rewind consumes the restored checkpoint and its newer history. Rewinding a recording truncates its old input future, so new play records a replacement branch. Rewinding playback repositions its query cursor without changing the file. Reset, manual state loads, port changes and cheat changes begin fresh history. Paused frame advances do not create checkpoints.

Capture finishes pending instructions and DMA on the owning thread. Checksums finish on one worker using detached bytes while the game resumes. Every crossed field still delivers its input, picture and sound events. The worker does not access live hardware.

## Speed

Preferences > Playback sets the normal speed, fast-forward limit, slow-motion speed, fast-forward picture skip and mute policy. Normal speed defaults to 100%, fast forward to 400%, and slow motion to 50%. Fast forward also supports uncapped play. The settings are saved in the current profile.

Speed changes scale host deadlines from elapsed console clocks. They do not skip console execution, input polling, generated pictures or PCM samples. Picture skip reduces only host presentations during fast forward. Audio at a finite altered speed plays with the corresponding pitch unless muted. Uncapped play discards host audio. Speed transitions clear queued audio; returning to normal speed primes a fresh buffer. At 100%, the normal audio queue feedback and presentation timing remain active.

| Default control | Action |
| --- | --- |
| Ctrl+Shift+I | Open input movies |
| Ctrl+Shift+R | Record from the current state |
| Ctrl+Shift+S | Stop the movie and save any recording |
| Backspace, held | Rewind |
| Shift+Backspace | Rewind one checkpoint |
| Tab, held | Fast forward |
| Shift+Tab | Toggle fast forward |
| Grave accent, held | Slow motion |
| Shift+Grave accent | Toggle slow motion |
| Ctrl+0 | Clear speed toggles and set normal speed to 100% |

Preferences > Hotkeys remaps these controls, including gamepad chords. Releasing a hold ends it. Focus loss, UI capture and disconnected gamepads clear held controls; a fresh press rearms them. A held slow-motion control overrides the fast-forward toggle. Speed toggles remain selected through pause and focus changes.
