# ROM archives

Open an archive with Ctrl+O, drop it onto the window, or pass its path on the command line. ZIP, 7z, TAR and RAR are supported, including compressed RAR 5. TAR can be compressed with gzip, bzip2, XZ, Zstandard or LZ4. Other supported containers include CAB, CPIO, PAX, ISO, XAR, LHA and AR. Single compressed ROMs work with names such as `game.sfc.gz` or `game.sfc.xz`.

An archive containing one compatible ROM opens immediately. When several match, **Choose ROM in Archive** lists their full names. Use **Find ROM** to narrow the list, select a row and choose **Open Selected**, or double-click a row. Cancel or Escape returns to the current game. Recent games remember the exact member and reopen it directly.

Console ROM candidates use `.sfc`, `.smc`, `.bin`, `.rom`, `.fig` or `.swc`. Handheld, slot and broadcast dialogs also accept the extensions appropriate to their selected role. Each component of a multi-cartridge launch can come from a separate archive or the same collection.

ROM data is read in memory. The source archive stays unchanged, and no members are extracted. By default, battery files go in `archive-saves` beside the archive, using a name derived from the archive path and complete member name. A configured save directory still applies. Two members with the same basename have separate battery files, state folders, cheats and movies. Moving or renaming an archive changes its save identity; retain the old saves when reorganizing a collection.

Firmware and MSU-1 files remain external. Place `firmware` beside the archive, or configure the firmware directory in Preferences. MSU-1 sidecars beside the archive use the selected ROM's basename, even when the member is in a nested folder.

Encrypted entries, split-volume archives and archives nested inside other archives are not supported. Invalid data, duplicate ROM names, missing remembered members and archives without compatible ROMs produce an error while retaining the current game. The reader limits an archive to 8 GiB, 16384 entries and 4 GiB of declared expanded data; a selected ROM is limited to 128 MiB. Long reads show progress and can be cancelled.

Archive decoders are included in the build. An external archiver installation is unnecessary. The executable's `licenses` directory contains the decoder and codec notices.
