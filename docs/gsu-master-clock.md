# GSU master-clock synchronization

The cartridge passes console master clocks directly to the GSU module. The module keeps the pending native step and its clock balance together, so regional conversion cannot resume an instruction before the CPU's bus edge.

## Step boundaries

A native step settles its buffered ROM and RAM activity before yielding its clocks. Its caller resumes only when the console advances beyond that step's end. At an exact boundary, the CPU can read or write registers while the instruction remains suspended. A zero-clock synchronization leaves that state unchanged.

This ordering also applies to cache-fill bytes, pixel-memory accesses, and STOP's IRQ update. For example, a CPU cache write at the end of a fill step occurs before that pending fill byte commits. An IRQ produced after a step becomes visible on the first later master clock.

A positive interval can start a native step even when less than one oscillator clock has elapsed. Its buffered activity is already visible while the step's clock wait remains pending.

## Clock balance and reset

The balance tracks native clocks multiplied by the selected console frequency, less console clocks multiplied by the 21.44 MHz cartridge oscillator. NTSC uses 21,477,272 master clocks per second and PAL uses 21,281,370. A nonnegative balance returns control to the CPU before the pending step resumes.

Instruction tests can still advance whole oscillator clocks through the same balance. This form exposes completion at whole-clock edges. Cartridge integration uses console master clocks and retains CPU access before instruction continuation at an exact edge.

Reset clears the balance and execution state while retaining the selected region and shared cartridge RAM. Starting GO at the end of a stopped six-clock interval does not add another idle interval.

## Validation limits

Cartridge tests cover exact register and IRQ boundaries in both regions and clock modes, CPU stop and PC writes, cache-fill writes and ROM denial, stopped restarts, initial fractional intervals, zero-clock calls, and regional reset. Instruction-level tests also check shared pending time across the two timing entry points.

These checks cover the exercised sequences. Physical clock measurements and complete game compatibility remain unverified.
