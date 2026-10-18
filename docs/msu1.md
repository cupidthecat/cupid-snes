# MSU-1 data and audio

The file loader enables MSU-1 when it finds an `.msu` file beside the ROM with the same stem. For example:

```text
game.sfc
game.msu
game-1.pcm
game-2.pcm
```

Run `game.sfc` through the normal frontend command. No extra flag is needed. An empty `.msu` file is valid for an audio-only pack. Track filenames use decimal numbers without leading zeroes, including track 0 and track 65535. Loading another cartridge closes the old pack and removes its register mapping. Loading a ROM from an in-memory byte array does not search for sidecars.

## Registers and files

The eight registers occupy `$2000-$2007` in banks `$00-$3F` and `$80-$BF`. Reads expose revision 2, playback status, the data stream and the `S-MSU1` signature. Writes select a 32-bit data position, a 16-bit track, volume, play/repeat state and a resume point. The highest byte commits a data seek or track selection. Data reads advance the stream, including repeated DMA reads from a fixed source address. Reads past EOF return zero; a later seek can read the file again.

PCM files start with `MSU1`, followed by a little-endian 32-bit loop position measured in stereo frames. The payload is signed little-endian 16-bit stereo at 44,100 frames per second. Missing tracks and invalid headers set the audio-error flag. Power and reset attempt track zero, so packs without that track initially report an error. A valid header permits an empty payload, and a partial final stereo frame pads missing bytes with zero.

Loop byte offsets use 32-bit arithmetic. An offset beyond EOF falls back to the first PCM frame. An offset exactly at EOF remains valid and repeats silence. Every EOF seek consumes one silent input tick. Stopping with the resume bit set saves the current track register and byte offset. A matching track selection consumes that saved position; interpolation history keeps advancing through pauses and selections.

Data and PCM each use an 8 KiB file cache. Seeks use 64-bit file offsets, so large data files do not require equally large allocations. File operations complete synchronously and leave the busy flags clear.

## Audio timing

PCM has an independent 44.1 kHz input clock. Scanline boundaries and register accesses synchronize playback with completed console bus intervals. Paused playback emits silence without moving the file cursor. DSP mute and zero volume silence the input while the cursor continues advancing.

Cubic interpolation converts each active audio stream to 32,040 stereo frames per second. Volume uses normalized floating-point samples. Console, handheld and expansion audio add before the final clamp. Stream history persists through timed volume, mute and track changes. Playback and mixing continue without a host audio sink and through DMA transfers spanning several video fields.

Loose files are supported; compressed `.msu1` archives and alternate manifest naming are not. The handheld adapter file loader also checks for a pack beside its console BIOS image.

## Checks

`snes_msu1_tests` uses generated data and PCM files. It checks sparse data through the 4 GiB boundary, cache boundaries, register mirrors, malformed and partial tracks, loop ticks and offset wrapping, resume registers, DSP mute and all 256 volume levels.

`snes_msu1_integration_tests` runs generated console and sound-processor programs in NTSC and PAL. Complete PCM signatures cover cold starts, warm reset, timed mute and volume changes, noise mixed with music, EOF, partial tracks, resume and long DMA. Headless runs must deliver the same later samples after a sink attaches. A three-stream cancellation check detects clipping before all inputs have been added. These tests require no commercial ROM or soundtrack files.
