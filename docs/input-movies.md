# Input movie format

An input movie pairs a complete initial machine checkpoint with an ordered sequence of input and host clock queries. The `.cmv` file contains the checkpoint, timeline endpoints, effective cheat identity and returned values. ROMs, firmware and streamed media remain external. The checkpoint identifies the required hardware and assets and is validated separately before execution is replaced. See [playback controls](playback.md) for recording, replay and rewind.

Queries retain their field index, device identity and every returned value. Repeated queries within a field remain separate records. Mouse motion and gun coordinates retain all 32 signed bits. A checkpoint can already contain latched input for a partial next field; its continuation starts with the next recorded query, without sampling the retained input again.

Readers check the exact file length, complete checksum, query types, device ranges, unused bits and nondecreasing field indices. Unknown versions and malformed data are rejected. Reads, validation, checksums and writes support cancellation. A cancelled or failed write removes its own temporary file and preserves the prior destination. The completed movie remains owned in memory for a later save attempt.

## Version 2 envelope

All integers use little-endian encoding. Signed coordinates and movement use 32-bit two's-complement values. The header is 128 bytes.

| Offset | Size | Field |
| --- | --- | --- |
| 0 | 8 | ASCII `CUPIDMV` followed by a zero byte |
| 8 | 4 | Format version, currently 2 |
| 12 | 4 | Initial checkpoint length |
| 16 | 8 | Number of input query records |
| 24 | 8 | Initial emulated field index |
| 32 | 8 | Final emulated field index |
| 40 | 8 | Initial elapsed master clocks |
| 48 | 8 | Final elapsed master clocks |
| 56 | 32 | Effective cheat identity |
| 88 | 8 | Reserved, zero |
| 96 | 32 | SHA-256 of header bytes 0 through 95 followed by the entire payload |
| 128 | Variable | Initial checkpoint, followed immediately by the query records |

The initial checkpoint has its own version and checksum and is limited to 128 MiB. Each query occupies 24 bytes. At most `floor(512 MiB / 24)` queries are supported. An empty query sequence is valid. The file has no trailing bytes or padding between the checkpoint and records. Initial coordinates cannot exceed final coordinates. Every query field lies within the inclusive endpoint range. The initial checkpoint must match both initial coordinates; successful playback must reach both final coordinates after consuming every query.

Cheat identity is SHA-256 of the one-byte cheat system followed by the effective codes in application order. Each code contributes its 32-bit address, one-byte value, one-byte comparison-present flag and one-byte comparison value, using zero when absent. Code labels and disabled entries do not affect this identity. The fingerprint requires matching effective settings without modifying the user's cheat files.

## Query records

| Offset | Size | Field |
| --- | --- | --- |
| 0 | 8 | Emulated field index at the query |
| 8 | 1 | Query type: 0 primary, 1 controller, 2 mouse, 3 light gun, 4 handheld clock, 5 receiver clock |
| 9 | 1 | Controller player 0 through 7, or mouse/gun port 0 through 1 |
| 10 | 1 | Gun 0 through 1; zero for other query types |
| 11 | 1 | Reserved, zero |
| 12 | 4 | Button flags |
| 16 | 4 | Mouse X movement or gun X coordinate; zero for controllers |
| 20 | 4 | Mouse Y movement or gun Y coordinate; zero for controllers |

The primary query uses index zero and remains distinct from a query for controller player zero. Controller button bits 0 through 11 are Up, Down, Left, Right, A, B, X, Y, L, R, Start and Select. Mouse bits 0 and 1 are Left and Right. Light-gun bits 0 through 5 are Trigger, Cursor, Turbo, Pause, Start and Offscreen. All remaining button bits must be zero.

Clock queries use zero for index, gun and buttons. The two 32-bit coordinate slots retain a raw 64-bit value, low word first. Handheld time uses seconds since the Unix epoch. Receiver time packs year into bits 0 through 15, then month, day, weekday, hour, minute and second into successive eight-bit fields. Battery-file timestamp queries occur outside emulated execution and are not part of this stream.
