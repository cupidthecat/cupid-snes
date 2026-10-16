# GSU instruction state during waits

See [GSU master-clock synchronization](gsu-master-clock.md) for the CPU access boundary around these waits.

Instructions can yield between fetching their operands, publishing results, and completing their remaining clocks. CPU register writes during those intervals must interact with the state visible at that point.

## Results and prefixes

MULT, UMULT, FMULT, and LMULT publish their result registers and flags before their internal delay. They also clear their instruction prefixes before waiting. A CPU register or prefix write during that delay therefore survives its completion. The selected clock cost still applies.

The instruction completes after that delay. A pending R14 ROM request is consumed then, and the normal PC increment occurs then unless R15 was written. Every pipeline fetch consumes the PC-write marker after the fetched byte returns. A later CPU write to R15 suppresses the instruction's final increment.

Conditional branches sample their condition before fetching the displacement. CPU flag changes during that fetch remain visible in the status register but do not change the decision already made.

## Buffered operands

GETBH reads the low source byte after the buffered ROM read completes. GETBL retains the high source byte selected before that wait. ROMB and RAMB sample their source register after synchronizing the corresponding buffer.

STW and LDW select their remaining byte width after the first buffered access. STW, SBK, SMS, and SM select the low source byte before synchronizing any older write. They select the high byte after queuing the low write, before waiting for it to commit. A CPU source-register change during the first wait can therefore affect the high byte without changing the low byte already selected.

RAMB synchronizes an existing buffered write. Once that write has committed, changing RAM ownership during the remaining clock wait does not require a second grant to select the bank.

## Uncached fetches

An uncached opcode fetch first synchronizes its ROM or RAM buffer. It then starts a separate memory step using the clock mode active at that point. Combining these waits would miss buffered accesses started by a CPU write between the steps.

The opcode bank is selected after the memory clock wait. That selected address remains fixed while a denied port inserts six-clock polls. Cache fills retain their initial bank and source address through the fill.

## Validation limits

Instruction programs check result and flag visibility, branch decisions, CPU PC writes, buffered source operands, load/store widths, bank selection, and fetch stages. A local audit covers 524,288 timelines of 64 GSU clocks across all 256 opcode bytes, four ALT modes, both clock modes, CPU write kinds and times, and preceding buffered stores. It compares all general registers, status, bank registers, cache base, selected RAM bytes, and the final RAM hash while CPU writes interrupt instruction waits.

These tests cover the exercised instruction sequences. Physical bus measurements and complete game compatibility remain unverified.
