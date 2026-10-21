#include "cartridge/BroadcastFixture.hpp"
#include "snes/core/Bsx.hpp"
#include "snes/core/Emulator.hpp"
#include "snes/core/Timing.hpp"

#include <algorithm>
#include <array>
#include <cstdio>
#include <stdexcept>
#include <vector>

using snes::core::BroadcastTime;
using snes::core::Bsx;

namespace {
void Check(bool condition, const char *description) {
  if (!condition)
    throw std::runtime_error(description);
}
struct Fixture {
  std::vector<uint8_t> bios = std::vector<uint8_t>(0x100000);
  std::vector<uint8_t> pack;
  std::vector<uint8_t> sram = std::vector<uint8_t>(0x8000);
  Bsx chip;
  explicit Fixture(size_t packSize = 0x100000) : pack(packSize, 0xff), chip(bios, pack, sram) {
    for (size_t i = 0; i < bios.size(); ++i)
      bios[i] = uint8_t(i / 0x8000);
    for (size_t i = 0; i < chip.Psram().size(); ++i)
      chip.Psram()[i] = uint8_t(0x40 + i / 0x8000);
  }
  void Mmc(unsigned index, uint8_t value) {
    chip.WriteCpu((index << 16) | 0x5000, value);
  }
  void Commit(uint8_t value = 0x80) {
    Mmc(14, value);
  }
  void Bare(bool high = true) {
    for (unsigned n = 1; n <= 13; ++n)
      Mmc(n, 0);
    Mmc(2, high ? 0x80 : 0);
    Mmc(12, 0x80);
    Commit();
  }
  void EnableFlashWrites() {
    Mmc(13, 0x80);
    Commit();
  }
  void Receiver(unsigned stream, uint16_t channel) {
    const unsigned base = 0x2188 + stream * 6;
    chip.WriteCpu(base, uint8_t(channel));
    chip.WriteCpu(base + 1, uint8_t(channel >> 8));
    chip.WriteCpu(base + 3, 1);
    chip.WriteCpu(base + 4, 1);
  }
};

void ControllerAndFlashRegressions() {
  Fixture f;
  Check(f.chip.ReadCpu(0x025000, 0x25) == 0x80,
        "Controller reads return the register bit with zero low bits");
  f.Mmc(2, 0);
  f.Commit(0);
  Check(f.chip.ReadCpu(0x025000, 0) == 0x80,
        "A zero commit write leaves the active mapping unchanged");
  f.Commit();
  Check(f.chip.ReadCpu(0x025000, 0) == 0, "Bit seven commits the pending controller mapping");
  f.chip.Reset();
  f.chip.Psram()[0x42123] = 0x9c;
  Check(f.chip.ReadCpu(0x216123) == 0x9c,
        "The small HiROM PSRAM windows pack each bank into eight KiB");
  f.Bare();
  f.chip.WriteCpu(0xc05555, 0x40);
  f.chip.WriteCpu(0xc11234, 0xa5);
  Check(f.pack[0x11234] == 0xff,
        "External write protection prevents programming a locked flash block");
  f.chip.WriteCpu(0xc00000, 0x70);
  Check(f.chip.ReadCpu(0xc00000) == 0xb0 && f.chip.ReadCpu(0xc00000) == 0xb0,
        "A protected write reports persistent program failure status");
  Check(f.chip.ReadCpu(0x005000, 0) == 0 && !f.chip.CpuIrqPending(),
        "Flash completion does not fabricate a controller interrupt");
}

void ColdStorage() {
  std::vector<uint8_t> bios(0x100000), pack(0x100000, 255), sram(0x8000, 255);
  Bsx chip(bios, pack, sram);
  Check(chip.Psram().size() == 0x80000 && std::all_of(chip.Psram().begin(), chip.Psram().end(),
                                                      [](uint8_t byte) { return byte == 255; }),
        "Fresh download RAM starts erased");
  chip.Psram()[0x123] = 0x64;
  chip.Reset();
  Check(chip.Psram()[0x123] == 0x64, "Reset retains downloaded RAM");
}
void MapperLatchesAndStorage() {
  Fixture f;
  for (unsigned reg = 0; reg < 16; ++reg) {
    const uint8_t expected = (0x0bec & (1u << reg)) ? 0x80 : 0;
    Check(f.chip.ReadCpu((reg << 16) | 0x5abc, 0x25) == expected,
          "MMC reset state drives zero in the low data-bus bits");
  }
  f.Mmc(2, 0);
  Check(f.chip.ReadCpu(0x025000, 0) == 0x80, "Reading MMC returns the applied bit before commit");
  f.Commit(0);
  Check(f.chip.ReadCpu(0x025000, 0) == 0x80, "Commit ignores writes with bit seven clear");
  f.Mmc(2, 0x7f);
  f.Commit();
  Check(f.chip.ReadCpu(0x025000, 0) == 0, "MMC writes sample only bit seven");
  f.Mmc(2, 0x80);
  f.Commit();
  for (unsigned bank = 0; bank < 64; ++bank) {
    Check(f.chip.ReadCpu((bank << 16) | 0x8123) == (bank & 31) &&
              f.chip.ReadCpu(((bank + 0x80) << 16) | 0x8123) == (bank & 31),
          "BIOS occupies both 64-bank upper-half windows with its physical one-megabyte mirror");
  }
  for (unsigned bank = 0x10; bank < 0x18; ++bank) {
    f.chip.WriteCpu((bank << 16) | 0x5abc, uint8_t(bank));
    Check(f.sram[(bank - 0x10) * 4096 + 0xabc] == bank,
          "SRAM is eight independent four-kilobyte banks");
    Check(f.chip.ReadCpu(((bank + 0x80) << 16) | 0x5abc, 0x73) == 0x73,
          "SRAM has no high-bank alias");
  }
  const uint8_t original = f.chip.Psram()[0x123];
  f.chip.WriteCpu(0x008123, 0xee);
  Check(f.bios[0x123] == 0 && f.chip.Psram()[0x123] == original,
        "BIOS writes cannot leak into underlying PSRAM");
  Check(f.chip.ReadCpu(0x600123, 0x37) == 0x37, "The default expansion-memory hole is open bus");
  f.Mmc(15, 0x80);
  Check(f.chip.ReadCpu(0x025000, 0x7f) == 0x80 && f.chip.ReadCpu(0x0f5000, 0xff) == 0,
        "Reserved controller writes do not select a hidden register page");
  f.Mmc(0, 0x80);
  Check(f.chip.ReadCpu(0x005000, 0xff) == 0,
        "The controller status remains clear after reserved writes");
  f.pack[3] = 0x26;
  f.chip.Psram()[5] = 0x51;
  f.chip.Reset();
  Check(f.pack[3] == 0x26 && f.chip.Psram()[5] == 0x51 && f.sram[0xabc] == 0x10,
        "Controller reset preserves flash, PSRAM and battery RAM");
  Check(Bsx::Selects(0x005fff) && Bsx::Selects(0x0f5000) && Bsx::Selects(0xa06abc) &&
            Bsx::Selects(0x802188) && !Bsx::Selects(0x002180) && !Bsx::Selects(0x7e1234),
        "Device windows include all mapper ports and exclude CPU WRAM accesses");
}

void PsramAndPackMapping() {
  Fixture f(0x200000);
  for (size_t i = 0; i < f.pack.size(); ++i)
    f.pack[i] = uint8_t(0x80 + i / 0x8000);
  for (unsigned high = 0; high < 2; ++high) {
    for (unsigned position = 0; position < 4; ++position) {
      f.Bare(high != 0);
      f.Mmc(3, 0x80);
      f.Mmc(4, 0x80);
      f.Mmc(5, (position & 1) ? 0x80 : 0);
      f.Mmc(6, (position & 2) ? 0x80 : 0);
      f.Commit();
      for (unsigned upper : {0u, 0x80u}) {
        const unsigned start = upper + position * (high ? 16 : 32);
        for (unsigned bank = 0; bank < (high ? 8u : 16u); ++bank) {
          const unsigned logical = start + bank;
          const size_t physical = bank * (high ? 65536 : 32768) + (high ? 0x8123 : 0x123);
          Check(f.chip.ReadCpu((logical << 16) | 0x8123) == f.chip.Psram()[physical],
                "Each selectable PSRAM window resolves the correct physical bank and half");
          if (high)
            Check(f.chip.ReadCpu(((logical + 0x40) << 16) | 0x0123) ==
                      f.chip.Psram()[bank * 65536 + 0x123],
                  "HiROM PSRAM also exposes the complete bank through the full-bank window");
          else if (logical & 0x40)
            Check(f.chip.ReadCpu((logical << 16) | 0x0123) ==
                      f.chip.ReadCpu((logical << 16) | 0x8123),
                  "LoROM PSRAM full banks mirror their lower and upper halves");
        }
        if (high) {
          for (unsigned bank = 0x20; bank < 0x40; ++bank)
            Check(
                f.chip.ReadCpu(((upper + bank) << 16) | 0x6123) ==
                    f.chip.Psram()[bank * 8192 + 0x123],
                "HiROM PSRAM snippets preserve the bank bits above their packed eight-KiB stride");
        } else {
          const unsigned end = upper ? 0x80 : 0x7e;
          for (unsigned bank = 0x70; bank < end; ++bank) {
            Check(f.chip.ReadCpu(((upper + bank) << 16) | 0x0123) ==
                      f.chip.Psram()[(bank & 15) * 32768 + 0x123],
                  "LoROM permanent PSRAM window covers the lower halves of high banks");
            Check(f.chip.ReadCpu(((upper + bank) << 16) | 0x8123) ==
                      f.pack[((bank & 0x3f) * 32768 + 0x123) & 0xfffff],
                  "The upper halves of the permanent LoROM PSRAM window remain memory pack");
          }
        }
      }
    }
  }
  for (bool high : {false, true}) {
    f.Bare(high);
    for (unsigned bank = 0; bank < 256; ++bank) {
      if (bank == 0x7e || bank == 0x7f)
        continue;
      const unsigned offset = bank & 0x40 ? 0x1234 : 0x9234;
      const size_t physical =
          high ? ((bank & 31) * 65536 + offset) : ((bank & 63) * 32768 + (offset & 0x7fff));
      Check(f.chip.ReadCpu((bank << 16) | offset) == f.pack[physical & 0xfffff],
            "Memory pack uses committed HiROM/LoROM address lines");
    }
  }
}

void MappingPriority() {
  Fixture f;
  for (bool high : {false, true}) {
    for (unsigned position = 0; position < 2; ++position) {
      f.Bare(high);
      f.Mmc(9, 0x80);
      f.Mmc(10, 0x80);
      f.Mmc(11, position ? 0x80 : 0);
      f.Commit();
      for (unsigned upper : {0u, 0x80u}) {
        const unsigned bank = upper + position * (high ? 32 : 64);
        Check(f.chip.ReadCpu((bank << 16) | 0x9234, 0x6c) == 0x6c,
              "Enabled expansion memory shadows the pack with open bus");
        if (high)
          Check(f.chip.ReadCpu(((bank + 0x40) << 16) | 0x1234, 0x6c) == 0x6c,
                "HiROM expansion hole also covers its corresponding full-bank region");
      }
    }
  }
  f.Bare(false);
  f.EnableFlashWrites();
  f.Mmc(9, 0x80);
  f.Mmc(3, 0x80);
  f.Commit();
  Check(f.chip.ReadCpu(0x008123, 0x21) == 0x40, "PSRAM overrides an expansion hole");
  f.Mmc(7, 0x80);
  f.Commit();
  Check(f.chip.ReadCpu(0x008123, 0x21) == 0, "BIOS overrides PSRAM and an expansion hole");
  f.Mmc(7, 0);
  f.Mmc(3, 0);
  f.Commit();
  f.chip.WriteCpu(0x008123, 0x40);
  f.Mmc(9, 0);
  f.Commit();
  f.chip.WriteCpu(0x008123, 0x26);
  Check(f.pack[0x123] == 0xff, "Writes to an unmapped expansion hole cannot arm a flash operation");
}

void FlashCommandsAndIrq() {
  Fixture f;
  f.Bare();
  f.EnableFlashWrites();
  f.Mmc(1, 0x80);
  f.chip.WriteCpu(0xc05555, 0x90);
  Check(f.chip.ReadCpu(0xc00000) == 0xb0 && f.chip.ReadCpu(0xc00001) == 0xa8 &&
            f.chip.ReadCpu(0xc00002) == 0x63 && f.chip.ReadCpu(0xc00003) == 0x20 &&
            f.chip.ReadCpu(0xc10000) == 0x20,
        "Chip identifiers appear at their physical offsets with the repeated marker");
  f.chip.WriteCpu(0xc05555, 0x70);
  Check(f.chip.ReadCpu(0xc01234) == 0x80 && f.chip.ReadCpu(0xc01234) == 0x80,
        "Compatible status remains selected until another read mode is requested");
  f.chip.WriteCpu(0xc05555, 0x71);
  Check(f.chip.ReadCpu(0xc00002) == 0x80 && f.chip.ReadCpu(0xc00004) == 0x86 &&
            f.chip.ReadCpu(0xc08002) == 0 && f.chip.ReadCpu(0xc00006) == 0,
        "Extended status separates block, global and reserved offsets");
  f.chip.WriteCpu(0xc05555, 0x40);
  f.chip.WriteCpu(0xc1002b, 0xa5);
  Check(f.pack[0x1002b] == 0xa5 && f.chip.ReadCpu(0xc1002b) == 0x80 &&
            f.chip.ReadCpu(0x005000, 0xff) == 0 && !f.chip.CpuIrqPending(),
        "Programming returns persistent status without creating a controller interrupt");
  f.chip.WriteCpu(0xc05555, 0x10);
  f.chip.WriteCpu(0xc1002b, 0x5a);
  Check(f.pack[0x1002b] == 0, "Flash programming can clear bits but cannot set them");
  f.chip.WriteCpu(0xc05555, 0x40);
  f.chip.WriteCpu(0xc1002b, 0xff);
  Check(f.pack[0x1002b] == 0, "A programmed FF data byte does not become a reset command");
  f.chip.WriteCpu(0xc05555, 0xff);
  Check(f.chip.ReadCpu(0xc1002b) == 0, "Array mode exposes the programmed bytes");
  f.Mmc(12, 0);
  f.Commit();
  f.chip.WriteCpu(0xc05555, 0x40);
  f.Mmc(12, 0x80);
  f.Commit();
  f.chip.WriteCpu(0xc00021, 0x26);
  Check(f.pack[0x21] == 0xff, "A disabled controller write port cannot arm a flash operation");

  f.chip.WriteCpu(0xc00000, 0xfb);
  f.chip.WriteCpu(0xc01235, 0x12);
  f.chip.WriteCpu(0xc01235, 0x34);
  Check(f.pack[0x1235] == 0x34 && f.pack[0x1234] == 0x12,
        "Odd-addressed word input swaps data order and toggles the destination low bit");
  f.chip.WriteCpu(0xc00000, 0xe0);
  f.chip.WriteCpu(0xc00000, 2);
  f.chip.WriteCpu(0xc00000, 0);
  for (auto [address, value] : std::array<std::pair<uint32_t, uint8_t>, 3>{
           {{0xc000fe, 0x11}, {0xc000ff, 0x22}, {0xc00100, 0x33}}})
    f.chip.WriteCpu(address, value);
  f.chip.WriteCpu(0xc00000, 0x75);
  Check(f.chip.ReadCpu(0xc000fe) == 0x11 && f.chip.ReadCpu(0xc001ff) == 0x22 &&
            f.chip.ReadCpu(0xc00200) == 0x33,
        "Sequential page loading wraps its byte address and consumes count plus one bytes");
  f.chip.WriteCpu(0xc00000, 0x0c);
  f.chip.WriteCpu(0xc01200, 2);
  f.chip.WriteCpu(0xc201fe, 0);
  Check(f.pack[0x201fe] == 0x11 && f.pack[0x201ff] == 0x22 && f.pack[0x20200] == 0x33,
        "Page programming uses the queued destination and wraps the page source");
  f.chip.WriteCpu(0xc00000, 0x75);
  Check(f.chip.ReadCpu(0xc000fe) == 0, "Programming swaps to the other retained page buffer");
  f.chip.WriteCpu(0xc00000, 0x72);
  Check(f.chip.ReadCpu(0xc000fe) == 0x11, "An explicit page swap restores the previous buffer");
  f.chip.Reset();
  f.Bare();
  f.chip.WriteCpu(0xc00000, 0x75);
  Check(f.chip.ReadCpu(0xc000fe) == 0x11 && f.pack[0x201fe] == 0x11,
        "Reset retains both page storage and flash array data");

  Fixture signature;
  signature.Bare();
  signature.chip.WriteCpu(0xc00000, 0x38);
  signature.chip.WriteCpu(0xc00000, 0xd0);
  signature.chip.WriteCpu(0xc00000, 0x72);
  signature.chip.WriteCpu(0xc00000, 0x75);
  Check(signature.chip.ReadCpu(0xc00000) == 'M' && signature.chip.ReadCpu(0xc00002) == 'P' &&
            signature.chip.ReadCpu(0xc00004) == 4 && signature.chip.ReadCpu(0xc00006) == 0x1a &&
            signature.chip.ReadCpu(0xc0000a) == 1 && signature.chip.ReadCpu(0xc00012) == 0x89,
        "The signature command writes capacity and the retained serial number into a page");
  signature.chip.WriteCpu(0xc00000, 0x99);
  signature.chip.WriteCpu(0xc00000, 0xd0);
  signature.chip.WriteCpu(0xc00000, 0x72);
  Check(signature.chip.ReadCpu(0xc00000) == 1 && signature.chip.ReadCpu(0xc00003) == 0x80 &&
            signature.chip.ReadCpu(0xc000d8) == 1 && signature.chip.ReadCpu(0xc000db) == 0x80,
        "Erase counts occupy the device-information table's defined byte offsets");

  Fixture large(0x400000);
  large.Bare();
  large.EnableFlashWrites();
  large.pack[0] = 0x43;
  large.pack[0x100000] = 0x96;
  Check(large.chip.ReadCpu(0xd00000) == 0x43,
        "The flash array retains its four-bit physical block identity on larger allocations");
  large.chip.WriteCpu(0xc00000, 0x90);
  Check(large.chip.ReadCpu(0xc00001) == 0x88,
        "Larger flash capacities expose the larger device identifier");
  std::vector<uint8_t> mask(0x400000, 0xff);
  mask[0xff00] = 'M';
  mask[0xff02] = 'P';
  mask[0xff06] = 0x70;
  mask[0x100000] = 0x96;
  Bsx romPack(large.bios, mask, large.sram);
  for (unsigned n = 1; n <= 13; ++n)
    romPack.WriteCpu((n << 16) | 0x5000, n == 2 || n == 12 || n == 13 ? 0x80 : 0);
  romPack.WriteCpu(0x0e5000, 0x80);
  romPack.WriteCpu(0xc05555, 0x40);
  romPack.WriteCpu(0xc00004, 0);
  Check(mask[4] == 0xff && romPack.ReadCpu(0xd00000) == 0x96 && romPack.FlashMetadata().empty(),
        "A mask ROM ignores flash commands and retains its complete address range");
}

void FlashEraseAndMetadata() {
  using snes::core::Timing;
  for (bool pal : {false, true}) {
    Fixture f;
    f.Bare();
    f.EnableFlashWrites();
    f.chip.SetPal(pal);
    f.pack[0x1234] = 0x32;
    f.pack[0x10000] = 0x54;
    const uint32_t frequency = pal ? Timing::kMasterClockHzPal : Timing::kMasterClockHz;
    f.chip.WriteCpu(0xc05555, 0x20);
    f.chip.WriteCpu(0xc01234, 0xd0);
    Check(f.chip.ReadCpu(0xc00000) == 0 && f.pack[0x1234] == 0x32,
          "Erase marks the chip busy without publishing its replacement data");
    f.chip.Advance(uint32_t(uint64_t(frequency) * 3 / 10));
    Check(f.pack[0x1234] == 0x32 && f.chip.ReadCpu(0xc00000) == 0,
          "The erase result remains pending through the 300-millisecond deadline");
    f.chip.Advance(1);
    Check(f.pack[0x1234] == 0xff && f.pack[0x10000] == 0x54 && f.chip.ReadCpu(0xc00000) == 0x80 &&
              !f.chip.CpuIrqPending(),
          "The next regional master clock completes exactly one block without an IRQ");
    const auto metadata = f.chip.FlashMetadata();
    Check(metadata.size() == 96 && metadata[16] == 2 && metadata[20] == 0,
          "Metadata records the block erase count and physical unlocked state");
    f.chip.Reset();
    Check(f.chip.FlashMetadata() == metadata, "Reset retains physical flash metadata");
    Fixture restored;
    Check(restored.chip.LoadFlashMetadata(metadata) && restored.chip.FlashMetadata() == metadata,
          "A new cartridge can restore the complete flash metadata record");
    auto invalid = metadata;
    invalid[20] = 2;
    Check(!restored.chip.LoadFlashMetadata(invalid) && restored.chip.FlashMetadata() == metadata,
          "Invalid lock flags are rejected without changing the current metadata");
    restored.Bare();
    restored.chip.WriteCpu(0xc00000, 0x97);
    restored.chip.WriteCpu(0xc00000, 0xd0);
    restored.chip.WriteCpu(0xc00000, 0x40);
    restored.chip.WriteCpu(0xc01234, 0x6a);
    Check(restored.pack[0x1234] == 0x6a,
          "Refreshing lock status exposes a restored unlocked block without overriding protection");
  }
}

void ClockedErasePolling() {
  for (unsigned slot = 0; slot < 3; ++slot)
    for (bool delayedStart : {false, true})
      for (bool pal : {false, true}) {
        snes::core::Emulator::Config config;
        config.entropyMode = snes::core::EntropyMode::None;
        snes::core::Emulator emulator(config);
        const auto rom = broadcast_test::ErasePollingRom(pal, delayedStart, slot);
        const std::vector<uint8_t> pack(0x100000);
        std::string error;
        Check(emulator.LoadBroadcastCartridge(rom, pack, &error),
              "The erase-polling guest loads through the normal cartridge API");
        for (unsigned pass = 0; pass < 2; ++pass) {
          if (pass)
            Check(emulator.Reset(), "The erase-polling guest resets normally");
          for (unsigned field = 0; field < 24; ++field)
            emulator.StepFrame();
          const auto *ram = emulator.GetBus().WramData();
          const unsigned polls = ram[0x200] | (unsigned(ram[0x201]) << 8);
          Check(ram[0x120] == 0xa5,
                "The guest observes erase completion and reaches its stop marker");
          const unsigned expected = delayedStart ? (pal ? 3109u : 3771u) : (pal ? 1825u : 2443u);
          Check(polls == expected,
                "Cold and warm erase polling retain the regional cartridge handoff boundary");
          Check(emulator.LoadedCartridge()->MemoryPackData()[0x1234] == (slot ? 0 : 0xff),
                "Erase completion preserves the slot's physical write protection");
        }
      }
}

void PageBufferStartup() {
  struct Scene {
    snes::core::EntropyMode mode;
    uint32_t seed;
    uint64_t digest;
    bool resetBeforeReload = false;
  };
  constexpr uint64_t zeroPages = 0x7da144b97d054b25;
  for (const auto scene : {Scene{snes::core::EntropyMode::None, 206, zeroPages},
                           Scene{snes::core::EntropyMode::Low, 194, 0x7a3e5bfa6a481c32},
                           Scene{snes::core::EntropyMode::High, 198, 0x1bdba64555c30c4e},
                           Scene{snes::core::EntropyMode::None, 358, zeroPages, true},
                           Scene{snes::core::EntropyMode::Low, 214, 0x70be016e39521cf5, true},
                           Scene{snes::core::EntropyMode::High, 213, 0xd08ba3c7bccd8341, true}})
    for (unsigned slot = 0; slot < 3; ++slot) {
      snes::core::Emulator::Config config;
      config.entropyMode = scene.mode;
      config.entropySeed = scene.seed;
      snes::core::Emulator emulator(config);
      const auto rom = broadcast_test::PageReadRom(slot);
      const std::vector<uint8_t> pack(0x100000);
      std::string error;
      const auto readPages = [&]() {
        emulator.StepFrame();
        const auto *ram = emulator.GetBus().WramData();
        Check(ram[0x120] == 0xa5, "The guest reads both complete page buffers");
        uint64_t digest = 0xcbf29ce484222325;
        for (unsigned index = 0x300; index < 0x500; ++index)
          digest = (digest ^ ram[index]) * 0x100000001b3;
        return digest;
      };
      Check(emulator.LoadBroadcastCartridge(rom, pack, &error), "The page-read guest loads");
      Check(readPages() == zeroPages, "First-load pages start before the first seeded power cycle");
      if (scene.resetBeforeReload)
        Check(emulator.Reset(), "The page-read guest resets before cartridge replacement");
      Check(emulator.LoadBroadcastCartridge(rom, pack, &error), "The page-read guest reloads");
      Check(readPages() == scene.digest,
            "Reloaded pages consume the stream retained by the previous power or reset");
      Check(emulator.Reset(), "The page-read guest resets normally");
      Check(readPages() == scene.digest, "A processor reset retains the existing flash pages");
    }
}

void RadioAndClock() {
  Fixture f;
  Check(f.chip.ReadCpu(0x2196) == 0 && f.chip.ReadCpu(0x2197) == 0,
        "Broadcast registers reset to zero");
  f.chip.WriteCpu(0x2189, 0xff);
  f.chip.WriteCpu(0x2194, 0xff);
  f.chip.WriteCpu(0x2197, 0x53);
  Check(f.chip.ReadCpu(0x802189) == 0xff && f.chip.ReadCpu(0x802194) == 0xff &&
            f.chip.ReadCpu(0x3f2197) == 0x53,
        "Stored radio registers retain all eight bits across bank mirrors");
  f.chip.WriteCpu(0x2193, 0xff);
  Check(f.chip.ReadCpu(0x2193) == 0xf3, "Status reads clear bits two and three");
  f.chip.WriteCpu(0x218e, 0x40);
  f.chip.WriteCpu(0x218f, 0xff);
  Check(f.chip.ReadCpu(0x218e) == 0xe0 && f.chip.ReadCpu(0x218f) == 0,
        "Receiver arithmetic uses its retained registers rather than the written byte");
  f.chip.WriteCpu(0x218f, 0x00);
  Check(f.chip.ReadCpu(0x218e) == 0x90, "Receiver subtraction wraps to eight bits");
  for (unsigned reg : {0x218bu, 0x218du, 0x2191u, 0x2195u, 0x2198u, 0x219au, 0x219fu}) {
    f.chip.WriteCpu(reg, 0x3c);
    Check(f.chip.ReadCpu(reg, 0xa5) == 0xa5, "Unreadable radio ports preserve open bus");
  }
  f.chip.WriteCpu(0x2192, 0x00);
  Check(f.chip.ReadCpu(0x2190) == 0x80, "Writing the serial clock sets receiver status");

  unsigned calls = 0;
  BroadcastTime now{2026, 9, 15, 3, 12, 34, 56};
  f.chip.SetTimeSource([&] {
    ++calls;
    return now;
  });
  std::array<uint8_t, 18> record{};
  for (unsigned i = 0; i < record.size(); ++i) {
    record[i] = f.chip.ReadCpu(0x2192);
    now.second = 1;
  }
  const std::array<uint8_t, 18> expected{0, 0, 0, 0, 0, 1, 1, 0, 0, 0, 56, 34, 12, 0, 0, 0, 0, 0};
  Check(calls == 1 && record == expected,
        "An eighteen-byte clock record samples only hours, minutes and seconds");
  f.chip.ReadCpu(0x2192);
  Check(calls == 2, "The next record samples the time provider again");
  f.chip.WriteCpu(0x2191, 0x55);
  Check(f.chip.ReadCpu(0x2192) == 0 && calls == 3,
        "Writing the clock control restarts its serial record");
  Check(Bsx::Selects(0x80219f) && !Bsx::Selects(0x0021a0),
        "The radio occupies all twenty-four ports");
}

void PacketStreams() {
  Fixture f;
  std::vector<uint8_t> first(45), second(22, 0xa5), large(22 * 130, 0x5a);
  for (unsigned i = 0; i < first.size(); ++i)
    first[i] = uint8_t(i);
  Check(!f.chip.LoadStream(0, 0, first) && !f.chip.LoadStream(0x4000, 0, first),
        "Host stream loader rejects reserved or unconnected channels");
  Check(f.chip.LoadStream(0x123, 0, first) && f.chip.LoadStream(0x123, 1, second) &&
            f.chip.LoadStream(0x456, 0, large),
        "Host can install independent channel sequences");
  f.Receiver(0, 0x123);
  f.Receiver(1, 0x456);
  Check(f.chip.ReadCpu(0x218a) == 3 && f.chip.ReadCpu(0x218a) == 3 && f.chip.ReadCpu(0x2190) == 127,
        "Packet count rounds up partial packets and saturates without consuming them");
  for (unsigned packet = 0; packet < 3; ++packet) {
    Check(f.chip.ReadCpu(0x218b) == (packet == 0   ? 0x10
                                     : packet == 2 ? 0x80
                                                   : 0),
          "Packet prefixes distinguish first, middle and last");
    for (unsigned byte = 0; byte < 22; ++byte) {
      const unsigned position = packet * 22 + byte;
      Check(f.chip.ReadCpu(0x218c) == (position < first.size() ? first[position] : 0xff),
            "Stream data remains sequential and pads the exhausted final packet with FF");
    }
  }
  Check(f.chip.ReadCpu(0x218b) == 0, "An extra prefix read cannot underflow an exhausted queue");
  Check(f.chip.ReadCpu(0x218d) == 0x90 && f.chip.ReadCpu(0x218d) == 0,
        "Prefix flags accumulate until OR status is read");
  Check(f.chip.ReadCpu(0x218a) == 1 && f.chip.ReadCpu(0x218b) == 0x90,
        "Next available sequence loads after the previous packet queue ends");
  for (unsigned i = 0; i < second.size(); ++i)
    Check(f.chip.ReadCpu(0x218c) == 0xa5, "Second stream asset has independent bytes");
  Check(f.chip.ReadCpu(0x218a) == 3,
        "Missing subsequent sequence wraps to the first installed asset");
  for (unsigned i = 0; i < 4; ++i)
    f.chip.ReadCpu(0x2191);
  Check(f.chip.ReadCpu(0x2190) == 126 && f.chip.ReadCpu(0x2192) == 0x5a,
        "The second receiver maintains its own prefix count and data cursor");

  f.Receiver(0, 0x123);
  f.Receiver(1, 0x123);
  f.chip.ReadCpu(0x218a);
  f.chip.ReadCpu(0x2190);
  Check(f.chip.ReadCpu(0x218c) == 0 && f.chip.ReadCpu(0x218c) == 1 && f.chip.ReadCpu(0x2192) == 0,
        "Two receivers can consume one asset independently");
  f.chip.ClearStreams();
  Check(f.chip.ReadCpu(0x218a) == 0 && f.chip.ReadCpu(0x2190) == 0 && f.chip.ReadCpu(0x218c) == 0,
        "Removing broadcast assets invalidates both receivers without leaving stale data");
  f.chip.LoadStream(0x123, 0, first);
  f.chip.Reset();
  f.Receiver(0, 0x123);
  Check(f.chip.ReadCpu(0x218a) == 3,
        "Controller reset retains host-installed broadcasts but rewinds receiver state");
  f.chip.LoadStream(0x123, 0, {});
  Check(f.chip.ReadCpu(0x218a) == 0, "An empty host payload removes the selected asset");
}
} // namespace

int main() {
  try {
    ControllerAndFlashRegressions();
    ColdStorage();
    MapperLatchesAndStorage();
    PsramAndPackMapping();
    MappingPriority();
    FlashCommandsAndIrq();
    FlashEraseAndMetadata();
    ClockedErasePolling();
    PageBufferStartup();
    RadioAndClock();
    PacketStreams();
  } catch (const std::exception &error) {
    std::fprintf(stderr, "BS-X regression: %s\n", error.what());
    return 1;
  }
  std::puts(
      "BS-X mapper, clocked flash, metadata, PSRAM, radio, clock and packet-stream checks passed");
  return 0;
}
