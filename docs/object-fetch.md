# Object evaluation and fetch timing

Objects are evaluated during one scanline, fetched during its horizontal blanking interval, and displayed on the following scanline. Register and memory writes reach the stage that consumes them.

## Evaluation

The first-object index is latched at line start. Evaluation checks one OAM entry every eight master clocks from H=0 through H=1016, in the latched rotation order. It uses the current position, size, and interlace controls. Up to 32 matching object indices enter the fetch list; a 33rd match records range overflow.

Evaluation retains membership rather than a copy of the attributes. Moving a selected object before fetching changes the attributes used by the fetch stage without removing it from the list. Forced blank suppresses evaluation. The main-screen and sub-screen OBJ enable bits do not stop it.

## Pattern fetches

Fetching starts at H=1080 and visits the selected indices in reverse order. Each object latches its geometry and character page when its fetch begins. Palette, priority selector, and horizontal flip are read when each tile begins. The two pattern words are read four clocks apart, with eight clocks consumed per fetched tile.

The fetch stage retains up to 34 tile rows. It also counts excess tile requests and sets the sticky time-over flag. Range-over and time-over become visible when the fetch list finishes, then remain set until the next field. A blanked or vertically inactive selected object consumes eight clocks without reading its tiles. Blanking between pattern words can suppress one read while retaining an earlier word or data left in the reused buffer.

Two tile buffers alternate between fetching and display. At line start, the display buffer is decoded into palette indices and raw object priority selectors. Screen enables, windows, palette colors, and the mode's priority table remain live at the pixel clock. The compositor retains the resulting layer samples for color math and brightness. It no longer needs per-line OAM copies or full VRAM snapshots.

Evaluation and fetching update the OAM bus address latch. Active-display CPU reads and writes follow that latch. Forced blank and vertical blanking return addressing to the CPU port.

## Validation and limits

`snes_ppu_object_tests` checks overflow before the next scanline, independent pattern-word reads, retained membership, first-object rotation, OAM bus ownership, blanking between words, address reset at the VBlank boundary, and live palette changes. Existing rendering tests cover object sizes, wrapping, interlace, overlap, palettes, windows, and color math.

A local audit matched 4,194,304 screen samples and 16,384 status/address results across generated scenes. It includes all eight size settings, both fields, priority rotation, display enables, blanking, and changes to OAM, VRAM, and fetch controls during a line.

These checks do not establish complete game compatibility. [PPU memory ports](ppu-memory-ports.md) covers palette lookup ownership and CPU port behavior. Memory collisions within a clock and optical gun sensing still need separate validation.
