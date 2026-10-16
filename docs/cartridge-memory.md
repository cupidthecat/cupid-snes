# Cartridge memory initialization

Fresh external cartridge RAM starts at `$FF` before the CPU executes. Loading saved bytes replaces that initial state. This covers ordinary mapping families, SA-1 BW-RAM, Super FX RAM, firmware-board battery RAM, OBC1, S-RTC, SPC7110, broadcast save and download RAM, and populated Sufami Turbo slots.

Processor-owned work RAM has separate rules:

| Storage | Fresh state | Console reset |
| --- | --- | --- |
| External cartridge save RAM | `$FF` | Retained |
| Super FX external RAM | `$FF` | Retained |
| Broadcast download RAM | `$FF` | Retained |
| Sufami Turbo slot RAM | `$FF` | Retained independently for each slot |
| SA-1 internal RAM | `$00` | Cleared |
| ST018 processor work RAM | `$00` | Cleared |
| Clocked ST010/ST011 data RAM | `$00` | Retained |
| ST010 command-model work RAM | `$00` | Cleared |
| Clocked Cx4 data RAM | `$00` | Retained |

ST018 bridge reset retains work RAM. It restarts the processor and bridge controls without performing a console reset. The console's own WRAM also remains separate from cartridge storage.

## Save loading and cartridge replacement

`Cartridge::LoadSram` copies the supplied prefix up to the physical storage size. An empty input leaves memory unchanged, and an oversized input truncates at the end of the allocation. Untouched bytes retain their current values. `LoadSlotSram` applies the same prefix rule to one occupied slot. An empty slot exposes no save data.

The frontend validates save-file sizes before calling these APIs. Replacing or reloading a cartridge creates fresh memory, then callers can restore its saved data. A console reset retains external save bytes and applies the processor-specific rules above. Shared storage allocations remain stable for their device's lifetime.

## Validation

Generated cartridges test external RAM defaults, partial and oversized save loads, CPU-visible memory, console reset, cartridge replacement, and both Sufami Turbo slots. Signal-processor fixtures distinguish their data RAM from ordinary save allocations. An ARM program increments a private RAM counter and reports it through the CPU mailbox, so the console and bridge reset paths can be checked through the running machine.

These checks cover the exercised memory policies. They do not establish complete game compatibility or measured physical power-on patterns.
