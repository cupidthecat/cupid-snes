# S-RTC cartridge clock

S-RTC cartridges expose a serial calendar at `$2800-$2801` in CPU banks `$00-$3F/$80-$BF`. Both slow and fast ExHiROM mapping flags retain the device. The read port returns calendar nibbles; reads of the write port preserve open bus. Writes to the read port have no effect.

## Calendar and oscillator

A fresh clock has cleared calendar storage. Fresh battery RAM contains `$FF`. Loading saved RAM replaces those bytes. Console reset retains the calendar and RAM, clears the serial command state, and restarts the oscillator phase.

The clock runs at one Hz against the console master clock: 21,477,272 Hz for NTSC and 21,281,370 Hz for PAL. Its first pending tick runs when a positive clock interval arrives. Subsequent ticks follow the one-second divider. Running calendars advance with emulated time; pausing console execution also pauses this advancement.

Calendar fields store numeric seconds, minutes, hours, day, month, year, and weekday. Serial programming replaces decimal places within those fields. A value above nine in a units position can therefore affect the next place. Incomplete dates retain their programmed storage and still accept time carries.

The year is stored relative to 1000 and wraps at twelve bits. February carry logic uses that stored year counter. Completing a calendar write calculates weekday using the full year and clamped month/day inputs. Time and date carries retain the weekday register.

## Serial commands

All writes use the low nibble. `$D` selects reading and restarts its cursor. A read stream contains a leading `$F`, thirteen calendar nibbles, and a trailing `$F`; the next read starts another stream. Each nibble reads the current calendar, so a clock edge can occur between successive digits.

`$E` selects command mode. Command zero begins a twelve-nibble calendar write, after which weekday is calculated. Command four clears all calendar fields and enters the ready state. Other commands enter the ready state without clearing the calendar. `$F` writes leave the current command and cursor unchanged. Reset returns to read mode.

## Persistence

The existing 24-byte `.rtc` format contains thirteen calendar bytes, the three ASCII bytes `RTC`, and an eight-byte little-endian Unix timestamp. Saving captures calendar storage without moving the serial cursor or advancing time. Loading applies elapsed host time in day, hour, minute, and second groups, then restarts the serial interface and oscillator phase.

Invalid sizes, signatures, non-nibble calendar bytes, and negative timestamps are rejected. Host clock rollback leaves saved calendar values intact. Elapsed intervals beyond 315,576,000,000 seconds are ignored. These file checks retain the existing save contract.

The loaded cartridge selects the clock format. The [SPC7110 RTC](spc7110.md) also uses 24 bytes, with sixteen device registers followed by its timestamp. Its fresh state has a cleared calendar and controls with the battery-failure flag set.

## Validation and limits

Generated regressions cover numeric programming, live reads, clock edges in both regions, calendar carries, reset, save loading, CPU mirrors, erased RAM, and slow/fast board detection. Local audits compare complete calendar and port state, oscillator balances, invalid programmed values, and elapsed-time loading.

These checks cover the exercised behavior. Complete game compatibility and physical RTC measurements remain unverified.
