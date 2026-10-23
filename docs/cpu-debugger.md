# CPU debugger

Open **Tools > CPU Debugger** or press Ctrl+Shift+D while a game is loaded. The window pauses normal playback and shows the console CPU registers, status flags, master clock, wait/stop state and whether a CPU or DMA operation is unfinished. Reading these values does not run the machine.

Use **Step** (F10) to complete one CPU operation. This may execute an instruction, reset sequence, interrupt entry, wait cycle or stopped cycle. If the window opened during an instruction or DMA transfer, Step completes that pending operation. Video fields crossed during the operation are still delivered. You can pause a long operation and continue it later.

**Run** (F9) executes until a breakpoint, watchpoint or manual pause. **Run to address** stops before the console CPU fetches an instruction at the entered address. Addresses are hexadecimal, from `000000` through `FFFFFF`; `$` and `0x` prefixes are accepted. Run is silent and unthrottled. Input used in the tools does not reach the game. Closing the window restores the pause state from before it opened.

## Breakpoints and watchpoints

Enter the first address and an optional inclusive last address, then choose any combination of Execute, Read and Write. A blank last address selects one address. Up to 256 entries can be enabled, disabled or removed.

Execution breakpoints stop before an instruction is fetched. They do not treat reset, interrupt, wait or stopped cycles as instructions. Continuing from an execution breakpoint passes that stop once, so a loop can reach it again. Step ignores execution breakpoints.

Read and write watchpoints observe the console bus, including DMA accesses. They record the first matching address, sampled byte and master clock, then stop after the current CPU operation completes. A long DMA transfer can cross several display fields before that stop. These watchpoints do not observe private coprocessor or handheld buses. Host cheat writes are excluded.

Breakpoints remain available when the window closes and reopens during the same game session. Reset and state loading retain the entries and clear the previous hit and trace. Opening another game starts a new debugger session.

## Trace capture

Pause the debugger and enable **Capture trace** on the Trace tab. The next Step or Run records CPU boundary register snapshots and console bus reads/writes. Boundary records include reset, interrupt entry, WAI and STP cycles. They are not a count of fetched instructions.

The trace retains the most recent 16,384 entries and reports how many older entries were overwritten. Clear trace resets both counters. Capture runs only while debugger controls execute the machine.

**Export trace** writes a tab-separated file in the current game's state folder under `traces`. It contains master clocks, addresses and bytes, plus registers for boundary rows. Rows run from the oldest retained entry to the newest. Failed exports leave the window available for retry.
