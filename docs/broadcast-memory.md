# Broadcast controllers and memory packs

The BIOS cartridge controller provides 512 KiB PSRAM, header-sized battery RAM, and a memory-pack slot. The normal BIOS declares 32 KiB battery RAM. LoROM, HiROM, and SA-1 slot boards use the same memory-pack device. The radio receiver attaches to the console expansion port. BIOS, game images, and recorded broadcasts are supplied separately.

A valid extended identifier selects the board before processor detection. Maker byte $33 enables four uppercase letters or digits: `ZBSJ` selects the BIOS controller, while other `Z??J` identifiers select the slot variant of the header's LoROM, HiROM, or SA-1 map. The BIOS title alone does not select a controller. These profiles need a save-RAM declaration; unsupported combinations remain unconnected.

## Controller and slot maps

Controller registers occupy `$00-$0F:$5000-$5FFF`. Reads return only D7; reserved registers return zero. Register 1 applies immediately. Registers 2 through 13 stage mapping changes, and writing register 14 with D7 high commits them. A write with D7 low leaves the applied map unchanged. Register 12 enables flash command writes, while register 13 controls the external write pin. Flash completion does not raise a controller IRQ.

PSRAM mapping includes full windows and packed 8 KiB snippets. The ordinary LoROM slot keeps its three base-ROM windows separate from the pack and exposes SRAM only in the lower halves of its SRAM banks. HiROM pack aliases share one command state. Unmapped banks retain open bus.

Supported packs are 512 KiB mask ROMs and 1, 2, or 4 MiB flash or mask ROM images. An omitted pack leaves the slot empty. Supply an erased image to insert blank flash. Mask ROMs ignore flash commands. Flash array accesses use a four-bit physical block identity, so larger flash allocations alias the first MiB; mask ROMs retain their full capacity.

SA-1 MMC pages 4-7 select a populated pack. Low-bank ROM windows use these pages only when the corresponding mapping-mode bit is set. Without a pack, every page retains base-ROM mirroring. The CPU's BW-RAM window remains $40-$4F, and SA-1 ROM writes do not issue flash commands. Loading a flash save and resetting the console preserve the attached storage.

## Flash commands and timing

The device maintains array, identifier, page-buffer, compatible-status, and extended-status read modes. Reading status does not return the device to array mode. Commands include byte and word programming, page transfers, status clearing, identifier and information uploads, lock-status refresh, and block erase. Page transfers can carry up to 65,536 bytes.

Programming only clears bits. A protected write leaves the array unchanged and records failure status. Physical block locks and the external write pin both affect write permission. Reset clears the command protocol and pending erase work while preserving image data, page buffers, erase counters, and physical locks.

Block erase completes after a 300 ms device wait, driven by the console master clock for the selected region. Reads observe busy status while the old array contents remain present. Completion erases one 64 KiB block, increments its erase counter, and unlocks it. Permission is checked when the erase finishes.

## Save files

The frontend saves the array in `.flash` and the physical device metadata in `.flash.meta`, beside the cartridge save. Keep both files together when moving or backing up a pack. Battery SRAM remains in its separate `.srm` file.

Metadata stores capacity, identifiers, serial number, block erase counts, and lock flags. Invalid metadata is rejected before changing device state. A missing metadata file starts with factory identifiers, one recorded erase per block, and locked blocks. Protocol queues, busy operations, and the external write pin are not battery state.

## Radio and recorded reception

The expansion port starts disconnected. Select `--expansion=satellaview` in the frontend, or set `Emulator::Config::expansionDevice` in a core caller, to attach the receiver. It works with ordinary cartridges and BS-X BIOS cartridges. `SetExpansionDevice` changes the attachment during execution. Reconnection, console reset, and cartridge replacement clear receiver registers and the serial cursor while retaining the selection, supplied assets, and time provider. A disconnected port returns open bus and ignores writes.

The radio interface occupies `$00-$3F,$80-$BF:$2188-$219F`. Stored registers retain all eight bits, unreadable ports return open bus, and `$2193` reads with bits 2 and 3 clear. Writing `$2192` sets `$2190` to `$80`. Writing `$2191` restarts the serial clock record.

Reading `$2192` cycles through eighteen bytes. Bytes 5 and 6 contain 1; bytes 10, 11, and 12 contain seconds, minutes, and hours. Other bytes are zero. Time is sampled once per record. The console supplies local time by default; core callers can inject a provider with `SetBroadcastTimeSource` while the receiver is attached.

Installing a payload with `LoadBroadcastStream` explicitly enables recorded reception. Its two receivers keep separate packet cursors and prefix flags; channel zero supplies the existing 23-byte calendar record. Standalone callers can use `Bsx::ClearStreams` to remove payloads and restore the default radio registers. Live broadcasts and SoundLink audio are not implemented.
