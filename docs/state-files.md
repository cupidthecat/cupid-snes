# State files

The desktop stores checkpoints in `.cst` files. Each file contains a complete machine checkpoint, its capture time, and an optional preview. Quick states use `Quick/1.cst` through `Quick/10.cst`; named states use `Managed/<name>.cst` inside the current launch's state folder. Preferences stores the selected slot as the zero-based `state.slot` value and the optional shared directory as `path.states`.

A state requires the same cartridge hardware, ROM/firmware, and streamed media. It does not package those assets. Windows and Linux use the same byte format. Unsupported format versions and invalid data reject before replacing execution. Writes use a temporary file beside the destination and replace it only after the complete write succeeds.

File reads, writes, checksums, and streamed-media checks support cancellation. A cancelled write closes and removes its temporary file while preserving the destination. A cancelled load leaves the previous undo history available. Media is checked again before a prepared restore releases suspended execution. No cancellation callback runs after a file replacement or state publication.

Preview reads validate the same header, exact file length, and complete checksum as full reads. They stream the machine payload without retaining it and keep only the decoded thumbnail. Cancelling or rejecting a preview does not publish its pixels or change the machine.

## Version 1 envelope

All integer fields use unsigned little-endian encoding. The fixed header is 64 bytes.

| Offset | Size | Field |
| --- | --- | --- |
| 0 | 8 | ASCII `CUPIDSF` followed by a zero byte |
| 8 | 4 | Envelope version, currently 1 |
| 12 | 4 | Machine checkpoint length in bytes |
| 16 | 8 | Capture time, seconds since the Unix epoch |
| 24 | 4 | Preview width, either 0 or 256 |
| 28 | 4 | Preview height, either 0 or 240 |
| 32 | 32 | SHA-256 of header bytes 0 through 31 followed by the entire payload |
| 64 | Variable | Machine checkpoint, then preview pixels |

The machine checkpoint is independently versioned and checksummed. Its maximum size is 128 MiB. Preview pixels follow it with no padding. Each pixel is a 32-bit value with red in bits 0 through 7, green in bits 8 through 15, blue in bits 16 through 23, and alpha in bits 24 through 31. The bytes on disk are consequently RGBA. The preview is scaled from the last completed picture to 256 by 240 pixels. Before the first picture, both dimensions are zero and there are no preview bytes. Readers require an exact length and checksum; trailing bytes are invalid.

## Names and identity

Managed names contain 1 through 120 UTF-8 bytes. Names cannot contain control characters or `/`, `\`, `:`, `*`, `?`, `"`, `<`, `>`, or `|`. Leading/trailing spaces, a final period, and reserved device names such as `CON`, `NUL`, `COM1`, and `LPT1` are rejected. Rename never replaces another state.

The launch folder uses a short filename label and a SHA-256 suffix. The hash covers the launch kind, competition DIP byte, and four canonical UTF-8 asset paths with explicit lengths. Moving assets changes this folder identity. The machine checkpoint separately validates the actual loaded media, so replacing a ROM at the same path cannot make an incompatible checkpoint load successfully.
