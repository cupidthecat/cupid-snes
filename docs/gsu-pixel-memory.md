# Super FX memory transactions

PLOT queues pixels in two row caches. A row records its position, eight colors, and the bits that need writing. RPIX flushes both cached rows before reading pixel memory.

## Plane transactions

A flush writes each bitplane separately. A full row needs one write access per plane. A partial row first reads that plane's existing byte, merges the selected pixel bits, and then writes the merged byte. The read result remains latched through the write wait, so an intervening RAM change cannot alter the retained bits.

Each access takes five GSU clocks with CLSR enabled or six with CLSR disabled. Changing CLSR affects the next access. A denied RAM port inserts six-clock polls until access is granted. Buffered ROM and RAM activity continues through these steps.

Completed planes become visible while later planes are still pending. The cache's pending mask clears after the final plane write. Its address and color depth remain fixed for the flush, including when the CPU changes screen registers during a wait.

## Instruction state

RPIX captures its coordinates before flushing queued pixels. The address and color depth used for the pixel read are selected after those flushes complete. Each plane read uses the captured bit position, and the selected depth remains fixed through the read sequence.

PLOT captures its coordinates and dithered color before a queued flush. It increments the live R1 register after the memory accesses and cache updates finish. A CPU write to R1 during a wait therefore changes the value that PLOT increments. Transparent pixels still advance R1.

## CPU ROM requests

A CPU write to either byte of R14 starts a ROM-buffer read immediately and leaves an instruction-completion request pending. The next instruction completion starts that read again and consumes the request. Fetching an opcode does not discard it. This also applies when the CPU writes R14 while the GSU is stopped.

An uncached ROM fetch or GETB must wait for the restarted read. Clearing the request at opcode fetch would let those operations proceed early and shift later instruction and idle intervals.

## Validation and limits

See [GSU instruction state during waits](gsu-instruction-state.md) for operand sampling, result publication, and uncached fetch stages.

Committed instruction programs check intermediate plane writes, coordinates retained through flushes and reads, the timing of R1 updates, and ROM-buffer restarts after CPU R14 writes in both clock modes. Local audits cover 16,384 timelines of 256 master-clock steps. They compare plane bytes, pending cache masks, R0/R1, final RAM hashes, and cached positions and colors across both regions, color depths, partial and full rows, changing registers, and RAM-port stalls. Separate timelines check CPU R14 writes before GO and during an opcode fetch.

A 6,000-field Yoshi's Island replay matches every video field, 3,198,656 shared stereo samples, and its final 32 KiB of work RAM. The pixel transaction changes also passed a 6,000-field Star Fox replay with the same video, audio, and memory checks. These checks cover the exercised sequences. Physical memory timing measurements and complete game compatibility remain unverified.
