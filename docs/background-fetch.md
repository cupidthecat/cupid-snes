# Background fetch timing

Backgrounds retain fetched tile data and shift it out at the pixel clock. A register or VRAM write changes future fetches without replacing data that has already entered the pipeline.

## Fetch and shift stages

Each mode assigns name, pattern, and offset fetches to a fixed 32-master-clock schedule. Fetch slots run every four clocks from H=0 through H=1052. A name fetch retains the character address, palette group, priority, and flip state. Each pattern-word fetch replaces only its pair of bitplanes.

The shifter keeps the fine-scroll phase latched at line start. Name fetches use the current scroll values. Changing scroll after a name fetch therefore waits for a later character column. Large characters wrap their ten-bit character number before the pattern base is added.

Low-resolution backgrounds produce one sample per dot. Modes 5 and 6 produce separate sub-screen and main-screen samples. The first pair is sampled at H=56 and H=58; later pairs follow every four master clocks. Mode 7 samples its current matrix registers at H=58 and every four clocks thereafter.

Changing a layer to an inactive mode leaves its fetched columns intact. Unconsumed pattern bits can still leave the shifter. Reset restarts control and mosaic state while retaining fetched column data. A new PPU instance starts with empty columns.

Mode 6 shares the preceding offset lookup across each pair of character columns. The first pair has no offset entry. Horizontal offsets use source-pixel coordinates and retain the latched fine-scroll phase.

## Register changes and composition

The core advances background fetches before a PPU register access, at the scanline sampling point, and before finishing a line. BG mode, tile maps, pattern bases, scroll, mosaic, and VRAM updates reach the stages that consume them. Forced blank hides output while background fetching and shifting continue.

Background palette colors and windows are resolved at their sample clocks. The final compositor combines those retained pixels with object rows and applies screen windows, color math, and brightness. Output-mode writes can change hires sampling within a line; earlier low-resolution dots remain doubled in the expanded field.

Mosaic reloads its vertical count to size plus one when it changes from fully disabled to enabled. Changing size or moving mosaic between enabled layers preserves the current count. Disabling the last layer preserves the count until it expires or line 1 of the next field.

## Validation and limits

`snes_ppu_background_tests` checks a retained three-pixel tile tail across a mode switch, separate name and plane latches, scroll-fetch latency, ten-bit character wrapping, mosaic reloads, reset retention, and a midline pseudo-hires change. It also checks live object priority against a background priority retained by an earlier name fetch. The general PPU suite covers all background modes, fine scroll, interlace, both screens, mosaic, windows, and Mode 7 transforms.

A local audit matched 4,194,304 generated layer samples across all eight modes, including mode, scroll, pattern-base, tile-size, mosaic, and VRAM changes within a line. A 6,000-field F1 ROC II replay matched all video fields and 3,198,656 shared stereo samples. The earlier implementation differed on 214 fields in that replay. An interlaced RPM Racing capture matched 1,800 fields and 961,379 shared stereo samples.

Objects use a separate [evaluation and fetch pipeline](object-fetch.md). [PPU memory ports](ppu-memory-ports.md) covers CPU access ownership and palette lookup addresses. Memory collisions within a clock and optical gun sensing still need separate validation. Zero brightness produces black, including in overscan fields.
