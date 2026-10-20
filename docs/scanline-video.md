# Scanline video

The console defaults to `Ppu::RenderModel::Scanline`. Set `Emulator::Config::ppu.model` to `Progressive` for clocked fetches and midline display changes. A standalone `Ppu` keeps the progressive default.

Scanline rendering samples screen registers and palette memory at H=512. CPU writes at that clock complete before the sample. Later brightness, scroll, window, palette, and color-math changes affect later lines. Each retained line renders against stable VRAM and OAM; a permitted write to either memory draws pending lines before changing the shared data. Palette writes keep the earlier line's copied palette intact.

Background and object decoding share the clocked renderer's fetch and composition components. Each scanline supplies a constant register snapshot. Background modes 0 through 7, offset tiles, mosaic, direct color, object dimensions, layer windows, and color windows use the retained state. Object overflow becomes visible when a batch renders, and disabled object screens skip evaluation in this model.

Scanline video uses 64 KiB VRAM, including when the configuration requests expanded memory. Status reports PPU2 revision 3 and clears PPU1's reserved bits. Active-display OAM writes select `$0218`; active OAM reads and palette accesses retain address zero. Cold power clears video and palette memory and initializes decoded sprites to zero, so their raw Y bytes read `$FF`. Warm reset retains all three memories while restoring registers and latches. This model consumes no graphics startup entropy.

The default `ppu.deinterlace` setting renders both fields of an interlaced frame. Disabling it updates only the current field. Hires and pseudo-hires apply color math independently to each dot. An empty opposite screen supplies its palette backdrop in hires and suppresses halving. The progressive model retains the preceding main dot's decisions and substitutes fixed color for a transparent color-math operand.

`snes_ppu_scanline_tests` runs normal CPU programs that load graphics through DMA and exercise every background mode, sprites, and repeated scroll changes. It also checks the sampling boundary, retained palette and tile data, warm reset, active OAM writes, and hires backdrop arithmetic. Generated local programs cover windows, mosaic, blending, interlace, overscan, and live graphics writes. These checks do not establish complete game compatibility or physical memory collisions within a clock.
